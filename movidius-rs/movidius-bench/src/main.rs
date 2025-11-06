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

use anyhow::Result;
use crossterm::{
    event::{self, DisableMouseCapture, EnableMouseCapture, Event, KeyCode},
    execute,
    terminal::{disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen},
};
use movidius_ncapi::{MultiDevicePool, PoolStats, SchedulingStrategy};
use ratatui::{
    backend::CrosstermBackend,
    layout::{Alignment, Constraint, Direction, Layout, Rect},
    style::{Color, Modifier, Style},
    text::{Line, Span},
    widgets::{Bar, BarChart, BarGroup, Block, Borders, List, ListItem, Paragraph, Sparkline},
    Frame, Terminal,
};
use std::io;
use std::time::{Duration, Instant};

/// Application state
struct App {
    /// Multi-device pool
    pool: MultiDevicePool,

    /// Performance history for sparklines
    throughput_history: Vec<Vec<u64>>,

    /// Temperature history
    temp_history: Vec<Vec<u64>>,

    /// Current selected device
    selected_device: usize,

    /// Paused state
    paused: bool,

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
            selected_device: 0,
            paused: false,
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
fn draw_footer(f: &mut Frame, area: Rect, _app: &App) {
    let controls = Paragraph::new(
        "Controls: [q]uit | [r]eset | [space] pause/resume | [↑/↓] select device | [s] cycle strategy",
    )
    .style(Style::default().fg(Color::Gray))
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
