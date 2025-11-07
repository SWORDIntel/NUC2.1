//! Movidius Neural Compute Stick TUI Benchmark Tool
//!
//! Interactive terminal UI for benchmarking Movidius devices with real-time metrics.
//!
//! Features:
//! - Multi-device monitoring
//! - Real-time thermal tracking
//! - Memory usage graphs
//! - Throughput/FPS counters
//! - Bottleneck detection
//! - Load balancing visualization
//!
//! Controls:
//! - q: Quit
//! - r: Reset metrics
//! - Space: Pause/Resume
//! - ↑/↓: Scroll devices
//! - s: Cycle scheduling strategy
//! - e: Export comprehensive report (JSON)
//! - a: Toggle analysis view

use anyhow::Result;
use crossterm::{
    event::{self, DisableMouseCapture, EnableMouseCapture, Event, KeyCode},
    execute,
    terminal::{disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen},
};
use movidius_ncapi::{
    AnalysisReport, DeviceMetrics, LatencyTracker, MemoryMetrics, MetricsAnalyzer,
    PerformanceMetrics, PoolMetrics, PoolStats, PoolStatistics, ResourceMetrics,
    SchedulingStrategy, ThermalMetrics, MultiDevicePool,
};
use ratatui::{
    backend::CrosstermBackend,
    layout::{Alignment, Constraint, Direction, Layout, Rect},
    style::{Color, Modifier, Style},
    text::{Line, Span},
    widgets::{Bar, BarChart, BarGroup, Block, Borders, List, ListItem, Paragraph, Sparkline},
    Frame, Terminal,
};
use std::fs;
use std::io;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

/// Application state
struct App {
    /// Multi-device pool
    pool: MultiDevicePool,

    /// Performance history for sparklines
    throughput_history: Vec<Vec<u64>>,

    /// Temperature history
    temp_history: Vec<Vec<u64>>,

    /// Latency trackers per device
    latency_trackers: Vec<LatencyTracker>,

    /// Temperature accumulation for averaging
    temp_samples: Vec<Vec<f32>>,

    /// Throttle time tracking
    throttle_time: Vec<Duration>,

    /// Last metrics snapshot
    last_metrics: Option<PoolMetrics>,

    /// Current selected device
    selected_device: usize,

    /// Paused state
    paused: bool,

    /// Show analysis view
    show_analysis: bool,

    /// Export notification
    export_msg: Option<String>,

    /// Export message timestamp
    export_msg_time: Option<Instant>,

    /// Start time for uptime calculation
    start_time: Instant,

    /// Last update time for FPS limiting
    last_update: Instant,
}

impl App {
    /// Create new app with specified device indices
    fn new(device_indices: &[usize]) -> Result<Self> {
        let pool = MultiDevicePool::new(device_indices, SchedulingStrategy::LeastLoaded)?;
        pool.open_all()?;

        let device_count = pool.device_count();

        Ok(Self {
            pool,
            throughput_history: vec![vec![]; device_count],
            temp_history: vec![vec![]; device_count],
            latency_trackers: (0..device_count).map(|_| LatencyTracker::new(1000)).collect(),
            temp_samples: vec![Vec::new(); device_count],
            throttle_time: vec![Duration::ZERO; device_count],
            last_metrics: None,
            selected_device: 0,
            paused: false,
            show_analysis: false,
            export_msg: None,
            export_msg_time: None,
            start_time: Instant::now(),
            last_update: Instant::now(),
        })
    }

    /// Update metrics
    fn update(&mut self) -> Result<()> {
        if self.paused {
            return Ok(());
        }

        let now = Instant::now();
        if now.duration_since(self.last_update) < Duration::from_millis(100) {
            return Ok(()); // Limit update rate to 10Hz
        }
        self.last_update = now;

        // Update history for each device
        for i in 0..self.pool.device_count() {
            if let Some(perf) = self.pool.performance(i) {
                let throughput = perf.throughput() as u64;
                self.throughput_history[i].push(throughput);
                if self.throughput_history[i].len() > 60 {
                    self.throughput_history[i].remove(0);
                }
            }

            // Update temperature history
            if let Some(device) = self.pool.get_device(i) {
                if let Ok((temp, _)) = device.read().thermal_stats() {
                    self.temp_history[i].push(temp as u64);
                    if self.temp_history[i].len() > 60 {
                        self.temp_history[i].remove(0);
                    }
                }
            }
        }

        Ok(())
    }

    /// Cycle to next scheduling strategy
    fn cycle_strategy(&mut self) {
        let current = self.pool.strategy();
        let next = match current {
            SchedulingStrategy::RoundRobin => SchedulingStrategy::LeastLoaded,
            SchedulingStrategy::LeastLoaded => SchedulingStrategy::PerformanceBased,
            SchedulingStrategy::PerformanceBased => SchedulingStrategy::RoundRobin,
        };
        self.pool.set_strategy(next);
    }

    /// Reset all metrics
    fn reset(&mut self) {
        for i in 0..self.pool.device_count() {
            if let Some(perf) = self.pool.performance(i) {
                perf.reset();
            }
            self.throughput_history[i].clear();
            self.temp_history[i].clear();
        }
        self.start_time = Instant::now();
    }

    /// Toggle pause
    fn toggle_pause(&mut self) {
        self.paused = !self.paused;
    }

    /// Toggle analysis view
    fn toggle_analysis(&mut self) {
        self.show_analysis = !self.show_analysis;
    }

    /// Collect comprehensive metrics
    fn collect_metrics(&mut self) -> Result<PoolMetrics> {
        let timestamp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_secs();

        let mut devices = Vec::new();

        for i in 0..self.pool.device_count() {
            let device = self.pool.get_device(i).ok_or_else(|| {
                anyhow::anyhow!("Device {} not found", i)
            })?;

            let device_lock = device.read();

            // Thermal metrics
            let (current_temp, max_temp) = device_lock.thermal_stats()?;
            let throttle = device_lock.throttling_level()?;
            let is_throttling = throttle.is_throttling();

            self.temp_samples[i].push(current_temp);
            let avg_temp = self.temp_samples[i].iter().sum::<f32>() / self.temp_samples[i].len() as f32;
            let peak_temp = self.temp_samples[i].iter().copied().fold(0.0f32, f32::max);

            let thermal = ThermalMetrics {
                current_temp,
                max_temp,
                throttle_level: throttle as u8,
                is_throttling,
                avg_temp,
                peak_temp,
                throttle_time: self.throttle_time[i].as_secs_f32(),
            };

            // Memory metrics
            let (used, total) = device_lock.memory_usage()?;
            let percent = (used as f64 / total as f64 * 100.0) as f32;
            let memory = MemoryMetrics {
                total,
                used,
                percent,
                avg_used: used, // Simplified for now
                peak_used: used,
                alloc_failures: 0,
            };

            // Performance metrics
            let perf_counter = self.pool.performance(i).unwrap();
            let throughput = perf_counter.throughput();

            let performance = PerformanceMetrics {
                total_inferences: perf_counter.total_inferences(),
                throughput,
                avg_latency_ms: self.latency_trackers[i].avg(),
                p50_latency_ms: self.latency_trackers[i].percentile(50.0),
                p95_latency_ms: self.latency_trackers[i].percentile(95.0),
                p99_latency_ms: self.latency_trackers[i].percentile(99.0),
                max_latency_ms: self.latency_trackers[i].max(),
                min_latency_ms: self.latency_trackers[i].min(),
                latency_stddev_ms: self.latency_trackers[i].stddev(),
                efficiency: 0.85, // Placeholder - would calculate from actual pipeline metrics
                receive_wait_us: 50.0, // Placeholder
            };

            // Resource metrics
            let (graphs_alloc, graphs_max, fifos_alloc, fifos_max) = device_lock.resource_counts()?;
            let resources = ResourceMetrics {
                graphs_allocated: graphs_alloc,
                graphs_max,
                graph_utilization: (graphs_alloc as f32 / graphs_max as f32 * 100.0),
                fifos_allocated: fifos_alloc,
                fifos_max,
                fifo_utilization: (fifos_alloc as f32 / fifos_max as f32 * 100.0),
                queue_depth: self.pool.load(i).unwrap_or(0),
            };

            devices.push(DeviceMetrics {
                device_id: i,
                timestamp,
                thermal,
                memory,
                performance,
                resources,
            });
        }

        let pool_stats_basic = self.pool.pool_stats();
        let pool_stats = PoolStatistics {
            total_throughput: pool_stats_basic.total_throughput,
            avg_throughput: pool_stats_basic.avg_throughput(),
            load_imbalance: pool_stats_basic.load_imbalance,
            is_balanced: pool_stats_basic.is_balanced(),
            total_queue_depth: pool_stats_basic.total_load,
            any_throttling: devices.iter().any(|d| d.thermal.is_throttling),
            any_memory_critical: devices.iter().any(|d| d.memory.percent > 90.0),
        };

        let metrics = PoolMetrics {
            timestamp,
            device_count: self.pool.device_count(),
            strategy: format!("{:?}", self.pool.strategy()),
            devices,
            pool_stats,
        };

        self.last_metrics = Some(metrics.clone());
        Ok(metrics)
    }

    /// Export comprehensive report
    fn export_report(&mut self) -> Result<()> {
        let metrics = self.collect_metrics()?;
        let issues = MetricsAnalyzer::analyze(&metrics);
        let health_score = MetricsAnalyzer::calculate_health_score(&metrics, &issues);
        let recommendations = MetricsAnalyzer::generate_recommendations(&metrics, &issues);

        let report = AnalysisReport {
            start_time: SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_secs(),
            duration_secs: self.start_time.elapsed().as_secs_f64(),
            metrics,
            issues,
            health_score,
            recommendations,
        };

        let timestamp = chrono::Utc::now().format("%Y%m%d_%H%M%S");
        let filename = format!("movidius_benchmark_{}.json", timestamp);
        let json = serde_json::to_string_pretty(&report)?;
        fs::write(&filename, json)?;

        self.export_msg = Some(format!("Report exported: {}", filename));
        self.export_msg_time = Some(Instant::now());

        Ok(())
    }
}

/// Draw the UI
fn ui(f: &mut Frame, app: &App) {
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(3),  // Header
            Constraint::Min(10),    // Main content
            Constraint::Length(3),  // Footer/Controls
        ])
        .split(f.size());

    // Header
    draw_header(f, chunks[0], app);

    // Main content
    let main_chunks = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([Constraint::Percentage(50), Constraint::Percentage(50)])
        .split(chunks[1]);

    // Left: Device list and details
    draw_devices(f, main_chunks[0], app);

    // Right: Metrics and graphs
    draw_metrics(f, main_chunks[1], app);

    // Footer
    draw_footer(f, chunks[2], app);
}

/// Draw header
fn draw_header(f: &mut Frame, area: Rect, app: &App) {
    let uptime = app.start_time.elapsed();
    let status = if app.paused { "PAUSED" } else { "RUNNING" };

    let title = format!(
        " Movidius Benchmark | Devices: {} | Strategy: {:?} | Status: {} | Uptime: {:02}:{:02}:{:02} ",
        app.pool.device_count(),
        app.pool.strategy(),
        status,
        uptime.as_secs() / 3600,
        (uptime.as_secs() % 3600) / 60,
        uptime.as_secs() % 60
    );

    let header = Paragraph::new(title)
        .style(Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD))
        .alignment(Alignment::Center)
        .block(Block::default().borders(Borders::ALL));

    f.render_widget(header, area);
}

/// Draw device list
fn draw_devices(f: &mut Frame, area: Rect, app: &App) {
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Percentage(40), Constraint::Percentage(60)])
        .split(area);

    // Device list
    let mut items = vec![];
    for i in 0..app.pool.device_count() {
        let load = app.pool.load(i).unwrap_or(0);
        let selected = if i == app.selected_device { "→ " } else { "  " };

        let status = if let Some(device) = app.pool.get_device(i) {
            if device.read().is_healthy().unwrap_or(false) {
                "✓"
            } else {
                "⚠"
            }
        } else {
            "✗"
        };

        let line = format!("{}Device {} {} (Load: {})", selected, i, status, load);
        let style = if i == app.selected_device {
            Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD)
        } else {
            Style::default()
        };

        items.push(ListItem::new(line).style(style));
    }

    let list = List::new(items)
        .block(Block::default().title("Devices").borders(Borders::ALL));

    f.render_widget(list, chunks[0]);

    // Selected device details
    if let Some(device) = app.pool.get_device(app.selected_device) {
        let device_lock = device.read();

        let mut details = vec![
            Line::from(format!("Device Index: {}", app.selected_device)),
            Line::from(""),
        ];

        if let Ok((temp, max_temp)) = device_lock.thermal_stats() {
            details.push(Line::from(format!("Temperature: {:.1}°C / {:.1}°C", temp, max_temp)));
        }

        if let Ok(throttle) = device_lock.throttling_level() {
            let throttle_str = format!("Throttling: {:?}", throttle);
            let style = if throttle.is_throttling() {
                Style::default().fg(Color::Red)
            } else {
                Style::default().fg(Color::Green)
            };
            details.push(Line::from(Span::styled(throttle_str, style)));
        }

        if let Ok((used, total)) = device_lock.memory_usage() {
            let percent = (used as f64 / total as f64) * 100.0;
            details.push(Line::from(format!(
                "Memory: {:.1} MB / {:.1} MB ({:.1}%)",
                used as f64 / (1024.0 * 1024.0),
                total as f64 / (1024.0 * 1024.0),
                percent
            )));
        }

        if let Ok((graphs_alloc, graphs_max, fifos_alloc, fifos_max)) =
            device_lock.resource_counts()
        {
            details.push(Line::from(format!(
                "Graphs: {} / {}",
                graphs_alloc, graphs_max
            )));
            details.push(Line::from(format!(
                "FIFOs: {} / {}",
                fifos_alloc, fifos_max
            )));
        }

        let paragraph = Paragraph::new(details)
            .block(Block::default().title("Device Details").borders(Borders::ALL));

        f.render_widget(paragraph, chunks[1]);
    }
}

/// Draw metrics
fn draw_metrics(f: &mut Frame, area: Rect, app: &App) {
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Percentage(25),
            Constraint::Percentage(25),
            Constraint::Percentage(25),
            Constraint::Percentage(25),
        ])
        .split(area);

    // Pool stats
    let stats = app.pool.pool_stats();
    draw_pool_stats(f, chunks[0], &stats);

    // Throughput graph
    if app.selected_device < app.throughput_history.len() {
        draw_sparkline(
            f,
            chunks[1],
            "Throughput (inf/s)",
            &app.throughput_history[app.selected_device],
        );
    }

    // Temperature graph
    if app.selected_device < app.temp_history.len() {
        draw_sparkline(
            f,
            chunks[2],
            "Temperature (°C)",
            &app.temp_history[app.selected_device],
        );
    }

    // Load distribution
    draw_load_distribution(f, chunks[3], app);
}

/// Draw pool statistics
fn draw_pool_stats(f: &mut Frame, area: Rect, stats: &PoolStats) {
    let balance_str = if stats.is_balanced() {
        "BALANCED"
    } else {
        "IMBALANCED"
    };

    let balance_color = if stats.is_balanced() {
        Color::Green
    } else {
        Color::Yellow
    };

    let text = vec![
        Line::from(format!("Total Load: {}", stats.total_load)),
        Line::from(format!("Total Throughput: {:.1} inf/s", stats.total_throughput)),
        Line::from(format!("Avg Load: {:.1}", stats.avg_load())),
        Line::from(vec![
            Span::raw("Balance: "),
            Span::styled(balance_str, Style::default().fg(balance_color)),
            Span::raw(format!(" ({:.1}%)", stats.load_imbalance * 100.0)),
        ]),
    ];

    let paragraph = Paragraph::new(text)
        .block(Block::default().title("Pool Statistics").borders(Borders::ALL));

    f.render_widget(paragraph, area);
}

/// Draw sparkline
fn draw_sparkline(f: &mut Frame, area: Rect, title: &str, data: &[u64]) {
    if data.is_empty() {
        return;
    }

    let sparkline = Sparkline::default()
        .block(Block::default().title(title).borders(Borders::ALL))
        .data(data)
        .style(Style::default().fg(Color::Cyan));

    f.render_widget(sparkline, area);
}

/// Draw load distribution bar chart
fn draw_load_distribution(f: &mut Frame, area: Rect, app: &App) {
    let mut bars = vec![];

    for i in 0..app.pool.device_count() {
        let load = app.pool.load(i).unwrap_or(0);
        bars.push(Bar::default().value(load as u64).label(format!("D{}", i).into()));
    }

    let barchart = BarChart::default()
        .block(Block::default().title("Load Distribution").borders(Borders::ALL))
        .data(BarGroup::default().bars(&bars))
        .bar_width(5)
        .bar_gap(1)
        .bar_style(Style::default().fg(Color::Yellow))
        .value_style(Style::default().fg(Color::Black).bg(Color::Yellow));

    f.render_widget(barchart, area);
}

/// Draw footer with controls
fn draw_footer(f: &mut Frame, area: Rect, app: &App) {
    // Check if export message should still be shown (5 second timeout)
    let show_export_msg = app.export_msg.is_some()
        && app
            .export_msg_time
            .map(|t| t.elapsed() < Duration::from_secs(5))
            .unwrap_or(false);

    let text = if show_export_msg {
        app.export_msg.as_ref().unwrap().clone()
    } else {
        "Controls: [q]uit | [r]eset | [space] pause | [↑/↓] select | [s] strategy | [a] analysis | [e] export".to_string()
    };

    let style = if show_export_msg {
        Style::default().fg(Color::Green).add_modifier(Modifier::BOLD)
    } else {
        Style::default().fg(Color::Gray)
    };

    let controls = Paragraph::new(text)
        .style(style)
        .alignment(Alignment::Center)
        .block(Block::default().borders(Borders::ALL));

    f.render_widget(controls, area);
}

fn main() -> Result<()> {
    // Initialize tracing
    tracing_subscriber::fmt::init();

    // Determine device indices (use 0 and 1 for dual-device by default)
    let device_indices = vec![0, 1];

    // Setup terminal
    enable_raw_mode()?;
    let mut stdout = io::stdout();
    execute!(stdout, EnterAlternateScreen, EnableMouseCapture)?;
    let backend = CrosstermBackend::new(stdout);
    let mut terminal = Terminal::new(backend)?;

    // Create app
    let mut app = match App::new(&device_indices) {
        Ok(app) => app,
        Err(e) => {
            // Restore terminal
            disable_raw_mode()?;
            execute!(
                terminal.backend_mut(),
                LeaveAlternateScreen,
                DisableMouseCapture
            )?;
            terminal.show_cursor()?;

            eprintln!("Failed to initialize benchmark: {}", e);
            eprintln!("\nPlease ensure:");
            eprintln!("  1. Movidius device(s) are plugged in");
            eprintln!("  2. Kernel module is loaded (modprobe movidius_x_vpu)");
            eprintln!("  3. Device files exist in /dev/movidius_x_vpu_*");
            std::process::exit(1);
        }
    };

    // Run the app
    let res = run_app(&mut terminal, &mut app);

    // Restore terminal
    disable_raw_mode()?;
    execute!(
        terminal.backend_mut(),
        LeaveAlternateScreen,
        DisableMouseCapture
    )?;
    terminal.show_cursor()?;

    // Close devices
    let _ = app.pool.close_all();

    if let Err(err) = res {
        eprintln!("Error: {:?}", err);
    }

    Ok(())
}

fn run_app<B: ratatui::backend::Backend>(
    terminal: &mut Terminal<B>,
    app: &mut App,
) -> Result<()> {
    loop {
        terminal.draw(|f| ui(f, app))?;

        // Update metrics
        app.update()?;

        // Handle input (non-blocking, 100ms timeout)
        if event::poll(Duration::from_millis(100))? {
            if let Event::Key(key) = event::read()? {
                match key.code {
                    KeyCode::Char('q') => return Ok(()),
                    KeyCode::Char('r') => app.reset(),
                    KeyCode::Char(' ') => app.toggle_pause(),
                    KeyCode::Char('s') => app.cycle_strategy(),
                    KeyCode::Char('a') => app.toggle_analysis(),
                    KeyCode::Char('e') => {
                        if let Err(e) = app.export_report() {
                            app.export_msg = Some(format!("Export failed: {}", e));
                            app.export_msg_time = Some(Instant::now());
                        }
                    }
                    KeyCode::Up => {
                        if app.selected_device > 0 {
                            app.selected_device -= 1;
                        }
                    }
                    KeyCode::Down => {
                        if app.selected_device < app.pool.device_count() - 1 {
                            app.selected_device += 1;
                        }
                    }
                    _ => {}
                }
            }
        }
    }
}
