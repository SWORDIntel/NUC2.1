//! Comprehensive benchmark metrics and analysis
//!
//! Collects detailed performance data and provides actionable insights
//! for optimization opportunities.

use serde::{Deserialize, Serialize};
use std::collections::VecDeque;
// Duration and Instant will be used for time-based metrics in future updates

/// Comprehensive device metrics with statistics
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DeviceMetrics {
    /// Device index
    pub device_id: usize,

    /// Timestamp of collection
    pub timestamp: u64,

    /// Temperature readings
    pub thermal: ThermalMetrics,

    /// Memory statistics
    pub memory: MemoryMetrics,

    /// Performance statistics
    pub performance: PerformanceMetrics,

    /// Resource utilization
    pub resources: ResourceMetrics,
}

/// Thermal metrics with statistics
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ThermalMetrics {
    /// Current temperature (°C)
    pub current_temp: f32,
    /// Maximum safe temperature (°C)
    pub max_temp: f32,
    /// Throttling level (0=none, 1=lower, 2=upper)
    pub throttle_level: u8,
    /// Is currently throttling
    pub is_throttling: bool,
    /// Average temperature over collection period
    pub avg_temp: f32,
    /// Peak temperature observed
    pub peak_temp: f32,
    /// Time spent throttling (seconds)
    pub throttle_time: f32,
}

/// Memory metrics with statistics
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct MemoryMetrics {
    /// Total memory (bytes)
    pub total: u64,
    /// Currently used (bytes)
    pub used: u64,
    /// Usage percentage
    pub percent: f32,
    /// Average usage over period
    pub avg_used: u64,
    /// Peak usage observed
    pub peak_used: u64,
    /// Number of allocation failures
    pub alloc_failures: u32,
}

/// Performance metrics with statistics
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PerformanceMetrics {
    /// Total inferences completed
    pub total_inferences: u64,
    /// Current throughput (inferences/sec)
    pub throughput: f64,
    /// Average latency (milliseconds)
    pub avg_latency_ms: f64,
    /// P50 latency (milliseconds)
    pub p50_latency_ms: f64,
    /// P95 latency (milliseconds)
    pub p95_latency_ms: f64,
    /// P99 latency (milliseconds)
    pub p99_latency_ms: f64,
    /// Maximum latency observed (milliseconds)
    pub max_latency_ms: f64,
    /// Minimum latency observed (milliseconds)
    pub min_latency_ms: f64,
    /// Standard deviation of latency
    pub latency_stddev_ms: f64,
    /// Pipeline efficiency (compute / total time)
    pub efficiency: f64,
    /// Receive tensor wait time (microseconds)
    pub receive_wait_us: f64,
}

/// Resource utilization metrics
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ResourceMetrics {
    /// Allocated graphs
    pub graphs_allocated: u32,
    /// Maximum graphs
    pub graphs_max: u32,
    /// Graph utilization percentage
    pub graph_utilization: f32,
    /// Allocated FIFOs
    pub fifos_allocated: u32,
    /// Maximum FIFOs
    pub fifos_max: u32,
    /// FIFO utilization percentage
    pub fifo_utilization: f32,
    /// Queue depth (pending operations)
    pub queue_depth: usize,
}

/// Pool-level metrics
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PoolMetrics {
    /// Timestamp
    pub timestamp: u64,

    /// Total devices in pool
    pub device_count: usize,

    /// Current scheduling strategy
    pub strategy: String,

    /// Per-device metrics
    pub devices: Vec<DeviceMetrics>,

    /// Pool-wide statistics
    pub pool_stats: PoolStatistics,
}

/// Pool-wide statistics
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PoolStatistics {
    /// Total throughput across all devices
    pub total_throughput: f64,
    /// Average throughput per device
    pub avg_throughput: f64,
    /// Load balance coefficient (0.0=perfect, 1.0=worst)
    pub load_imbalance: f64,
    /// Is load balanced (<20% imbalance)
    pub is_balanced: bool,
    /// Total queue depth
    pub total_queue_depth: usize,
    /// Any device throttling?
    pub any_throttling: bool,
    /// Any device memory critical?
    pub any_memory_critical: bool,
}

/// Performance issue detected
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PerformanceIssue {
    /// Issue severity (Critical, Warning, Info)
    pub severity: IssueSeverity,
    /// Device ID (or None for pool-level)
    pub device_id: Option<usize>,
    /// Issue category
    pub category: IssueCategory,
    /// Human-readable description
    pub description: String,
    /// Recommended action
    pub recommendation: String,
}

/// Severity level of a performance issue
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum IssueSeverity {
    /// Critical issue requiring immediate attention
    Critical,
    /// Warning that may impact performance
    Warning,
    /// Informational notice
    Info,
}

/// Category of performance issue
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum IssueCategory {
    /// Thermal-related issue
    Thermal,
    /// Memory-related issue
    Memory,
    /// Performance degradation
    Performance,
    /// Load balancing issue
    LoadBalance,
    /// Resource availability issue
    Resource,
}

/// Comprehensive analysis report
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AnalysisReport {
    /// Collection start time
    pub start_time: u64,
    /// Collection duration (seconds)
    pub duration_secs: f64,
    /// Latest metrics snapshot
    pub metrics: PoolMetrics,
    /// Detected issues
    pub issues: Vec<PerformanceIssue>,
    /// Overall health score (0-100)
    pub health_score: u32,
    /// Summary recommendations
    pub recommendations: Vec<String>,
}

/// Latency tracker for percentile calculations
pub struct LatencyTracker {
    samples: VecDeque<f64>,
    max_samples: usize,
}

impl LatencyTracker {
    /// Create a new latency tracker with specified maximum sample count
    pub fn new(max_samples: usize) -> Self {
        Self {
            samples: VecDeque::with_capacity(max_samples),
            max_samples,
        }
    }

    /// Record a latency sample in milliseconds
    pub fn record(&mut self, latency_ms: f64) {
        if self.samples.len() >= self.max_samples {
            self.samples.pop_front();
        }
        self.samples.push_back(latency_ms);
    }

    /// Calculate the percentile latency (0-100)
    pub fn percentile(&self, p: f64) -> f64 {
        if self.samples.is_empty() {
            return 0.0;
        }

        let mut sorted: Vec<f64> = self.samples.iter().copied().collect();
        sorted.sort_by(|a, b| a.partial_cmp(b).unwrap());

        let idx = ((sorted.len() as f64 - 1.0) * p / 100.0) as usize;
        sorted[idx]
    }

    /// Calculate average latency
    pub fn avg(&self) -> f64 {
        if self.samples.is_empty() {
            return 0.0;
        }
        self.samples.iter().sum::<f64>() / self.samples.len() as f64
    }

    /// Calculate standard deviation of latency
    pub fn stddev(&self) -> f64 {
        if self.samples.is_empty() {
            return 0.0;
        }

        let avg = self.avg();
        let variance =
            self.samples.iter().map(|x| (x - avg).powi(2)).sum::<f64>() / self.samples.len() as f64;
        variance.sqrt()
    }

    /// Get minimum latency
    pub fn min(&self) -> f64 {
        self.samples
            .iter()
            .copied()
            .min_by(|a, b| a.partial_cmp(b).unwrap())
            .unwrap_or(0.0)
    }

    /// Get maximum latency
    pub fn max(&self) -> f64 {
        self.samples
            .iter()
            .copied()
            .max_by(|a, b| a.partial_cmp(b).unwrap())
            .unwrap_or(0.0)
    }
}

/// Analyzer that generates insights from metrics
pub struct MetricsAnalyzer;

impl MetricsAnalyzer {
    /// Analyze metrics and generate issues
    pub fn analyze(metrics: &PoolMetrics) -> Vec<PerformanceIssue> {
        let mut issues = Vec::new();

        // Check thermal issues
        for (idx, device) in metrics.devices.iter().enumerate() {
            if device.thermal.is_throttling {
                issues.push(PerformanceIssue {
                    severity: IssueSeverity::Critical,
                    device_id: Some(idx),
                    category: IssueCategory::Thermal,
                    description: format!(
                        "Device {} throttling at {:.1}°C",
                        idx, device.thermal.current_temp
                    ),
                    recommendation: "Improve cooling or reduce workload".to_string(),
                });
            } else if device.thermal.current_temp > 75.0 {
                issues.push(PerformanceIssue {
                    severity: IssueSeverity::Warning,
                    device_id: Some(idx),
                    category: IssueCategory::Thermal,
                    description: format!(
                        "Device {} temperature elevated: {:.1}°C",
                        idx, device.thermal.current_temp
                    ),
                    recommendation: "Monitor temperature, ensure adequate airflow".to_string(),
                });
            }
        }

        // Check memory issues
        for (idx, device) in metrics.devices.iter().enumerate() {
            if device.memory.percent > 90.0 {
                issues.push(PerformanceIssue {
                    severity: IssueSeverity::Critical,
                    device_id: Some(idx),
                    category: IssueCategory::Memory,
                    description: format!(
                        "Device {} memory critical: {:.1}%",
                        idx, device.memory.percent
                    ),
                    recommendation: "Reduce batch size or model size".to_string(),
                });
            } else if device.memory.percent > 80.0 {
                issues.push(PerformanceIssue {
                    severity: IssueSeverity::Warning,
                    device_id: Some(idx),
                    category: IssueCategory::Memory,
                    description: format!(
                        "Device {} memory high: {:.1}%",
                        idx, device.memory.percent
                    ),
                    recommendation: "Consider optimizing memory usage".to_string(),
                });
            }
        }

        // Check performance issues
        for (idx, device) in metrics.devices.iter().enumerate() {
            if device.performance.efficiency < 0.5 {
                issues.push(PerformanceIssue {
                    severity: IssueSeverity::Warning,
                    device_id: Some(idx),
                    category: IssueCategory::Performance,
                    description: format!(
                        "Device {} low efficiency: {:.1}%",
                        idx,
                        device.performance.efficiency * 100.0
                    ),
                    recommendation: "Check pipeline for bottlenecks (input/output starvation)"
                        .to_string(),
                });
            }

            if device.performance.receive_wait_us > 100.0 {
                issues.push(PerformanceIssue {
                    severity: IssueSeverity::Warning,
                    device_id: Some(idx),
                    category: IssueCategory::Performance,
                    description: format!(
                        "Device {} high receive wait: {:.1}μs",
                        idx, device.performance.receive_wait_us
                    ),
                    recommendation: "Increase FIFO depth or reduce data transfer overhead"
                        .to_string(),
                });
            }

            if device.performance.p95_latency_ms > device.performance.avg_latency_ms * 2.0 {
                issues.push(PerformanceIssue {
                    severity: IssueSeverity::Info,
                    device_id: Some(idx),
                    category: IssueCategory::Performance,
                    description: format!("Device {} high latency variance (P95: {:.2}ms, avg: {:.2}ms)",
                        idx, device.performance.p95_latency_ms, device.performance.avg_latency_ms),
                    recommendation: "Investigate latency spikes, check for thermal throttling or memory pressure".to_string(),
                });
            }
        }

        // Check load balance
        if !metrics.pool_stats.is_balanced {
            issues.push(PerformanceIssue {
                severity: IssueSeverity::Warning,
                device_id: None,
                category: IssueCategory::LoadBalance,
                description: format!(
                    "Load imbalance: {:.1}%",
                    metrics.pool_stats.load_imbalance * 100.0
                ),
                recommendation: "Try different scheduling strategy (current: {})".to_string(),
            });
        }

        // Check resource utilization
        for (idx, device) in metrics.devices.iter().enumerate() {
            if device.resources.graph_utilization > 90.0 {
                issues.push(PerformanceIssue {
                    severity: IssueSeverity::Warning,
                    device_id: Some(idx),
                    category: IssueCategory::Resource,
                    description: format!(
                        "Device {} graph resources near limit: {:.1}%",
                        idx, device.resources.graph_utilization
                    ),
                    recommendation: "May need to add more devices to pool".to_string(),
                });
            }

            if device.resources.fifo_utilization > 90.0 {
                issues.push(PerformanceIssue {
                    severity: IssueSeverity::Warning,
                    device_id: Some(idx),
                    category: IssueCategory::Resource,
                    description: format!(
                        "Device {} FIFO resources near limit: {:.1}%",
                        idx, device.resources.fifo_utilization
                    ),
                    recommendation: "Optimize FIFO usage or add more devices".to_string(),
                });
            }
        }

        issues
    }

    /// Calculate overall health score (0-100)
    pub fn calculate_health_score(metrics: &PoolMetrics, issues: &[PerformanceIssue]) -> u32 {
        let mut score = 100u32;

        // Deduct for critical issues
        let critical_count = issues
            .iter()
            .filter(|i| i.severity == IssueSeverity::Critical)
            .count();
        score = score.saturating_sub(critical_count as u32 * 20);

        // Deduct for warnings
        let warning_count = issues
            .iter()
            .filter(|i| i.severity == IssueSeverity::Warning)
            .count();
        score = score.saturating_sub(warning_count as u32 * 5);

        // Bonus for good metrics
        if metrics.pool_stats.is_balanced {
            score = score.saturating_add(5);
        }

        if !metrics.pool_stats.any_throttling {
            score = score.saturating_add(5);
        }

        if !metrics.pool_stats.any_memory_critical {
            score = score.saturating_add(5);
        }

        score.min(100)
    }

    /// Generate summary recommendations
    pub fn generate_recommendations(
        metrics: &PoolMetrics,
        issues: &[PerformanceIssue],
    ) -> Vec<String> {
        let mut recs = Vec::new();

        // Group issues by category
        let thermal_issues = issues
            .iter()
            .filter(|i| i.category == IssueCategory::Thermal)
            .count();
        let memory_issues = issues
            .iter()
            .filter(|i| i.category == IssueCategory::Memory)
            .count();
        let perf_issues = issues
            .iter()
            .filter(|i| i.category == IssueCategory::Performance)
            .count();

        if thermal_issues > 0 {
            recs.push("THERMAL: Improve cooling (add fans, better ventilation, or reduce ambient temperature)".to_string());
        }

        if memory_issues > 0 {
            recs.push(
                "MEMORY: Optimize model size or reduce batch size to lower memory pressure"
                    .to_string(),
            );
        }

        if perf_issues > 0 {
            recs.push("PERFORMANCE: Check pipeline efficiency - ensure input/output FIFOs are properly sized".to_string());
        }

        if !metrics.pool_stats.is_balanced {
            recs.push(format!(
                "LOAD BALANCE: Try switching from {} to a different scheduling strategy",
                metrics.strategy
            ));
        }

        // Device-specific recommendations
        let avg_throughput = metrics.pool_stats.avg_throughput;
        for (idx, device) in metrics.devices.iter().enumerate() {
            if device.performance.throughput < avg_throughput * 0.7 {
                recs.push(format!("Device {}: Significantly underperforming - check for throttling or hardware issues", idx));
            }
        }

        // If everything is good
        if recs.is_empty() {
            recs.push("✓ System operating optimally - no recommendations at this time".to_string());
        }

        recs
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_latency_tracker() {
        let mut tracker = LatencyTracker::new(100);

        for i in 1..=100 {
            tracker.record(i as f64);
        }

        assert_eq!(tracker.min(), 1.0);
        assert_eq!(tracker.max(), 100.0);
        assert!((tracker.avg() - 50.5).abs() < 0.1);
        assert!((tracker.percentile(50.0) - 50.0).abs() < 1.0);
    }
}
