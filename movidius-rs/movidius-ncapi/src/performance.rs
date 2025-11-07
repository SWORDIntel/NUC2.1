//! Pipeline performance counters and metrics
//!
//! Based on ncappzoo KEY_VPU_PRINT_RECEIVE_TENSOR_TIME analysis

use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

/// Pipeline bottleneck detection
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Bottleneck {
    /// No bottleneck detected - optimal performance
    None,
    /// VPU is waiting for input tensors (increase producer throughput or FIFO depth)
    InputStarved,
    /// Output FIFO is backing up (increase consumer throughput)
    OutputStalled,
    /// Both input and output issues
    BothEnds,
}

/// Pipeline performance metrics
#[derive(Debug, Clone)]
pub struct PipelineMetrics {
    /// Time VPU spends waiting for input tensor (KEY_VPU_PRINT_RECEIVE_TENSOR_TIME)
    /// In optimal pipeline, this should be near zero (<100μs)
    pub receive_tensor_time: Duration,

    /// Actual inference computation time
    pub compute_time: Duration,

    /// Input FIFO queue depth (current)
    pub input_queue_depth: usize,

    /// Output FIFO queue depth (current)
    pub output_queue_depth: usize,

    /// Total inferences processed
    pub total_inferences: u64,

    /// Timestamp of last update
    pub last_update: Instant,
}

impl PipelineMetrics {
    /// Create new metrics instance
    pub fn new() -> Self {
        Self {
            receive_tensor_time: Duration::ZERO,
            compute_time: Duration::ZERO,
            input_queue_depth: 0,
            output_queue_depth: 0,
            total_inferences: 0,
            last_update: Instant::now(),
        }
    }

    /// Check if pipeline is operating optimally
    pub fn is_optimal(&self) -> bool {
        // VPU should never wait for input in optimal pipeline
        self.receive_tensor_time < crate::config::performance::MAX_RECEIVE_TENSOR_TIME
            && self.input_queue_depth > 0  // Always has work queued
            && self.output_queue_depth < crate::config::fifo::MAX_DEPTH - 1 // Not backing up
    }

    /// Detect pipeline bottleneck
    pub fn bottleneck(&self) -> Bottleneck {
        let input_starved =
            self.receive_tensor_time > crate::config::performance::MAX_RECEIVE_TENSOR_TIME;
        let output_stalled = self.output_queue_depth >= crate::config::fifo::DEFAULT_DEPTH;

        match (input_starved, output_stalled) {
            (true, true) => Bottleneck::BothEnds,
            (true, false) => Bottleneck::InputStarved,
            (false, true) => Bottleneck::OutputStalled,
            (false, false) => Bottleneck::None,
        }
    }

    /// Calculate current FPS (frames per second)
    pub fn fps(&self) -> f64 {
        let elapsed = self.last_update.elapsed();
        if elapsed.as_secs_f64() < 0.001 {
            return 0.0;
        }
        self.total_inferences as f64 / elapsed.as_secs_f64()
    }

    /// Calculate average inference time
    pub fn avg_inference_time(&self) -> Duration {
        if self.total_inferences == 0 {
            return Duration::ZERO;
        }
        self.compute_time / self.total_inferences as u32
    }

    /// Get performance efficiency (0.0-1.0)
    /// 1.0 = VPU never waits for input, always computing
    pub fn efficiency(&self) -> f64 {
        let total = self.receive_tensor_time + self.compute_time;
        if total.as_secs_f64() < 0.001 {
            return 1.0;
        }
        self.compute_time.as_secs_f64() / total.as_secs_f64()
    }

    /// Get diagnostic recommendation
    pub fn recommendation(&self) -> &'static str {
        match self.bottleneck() {
            Bottleneck::None => "Pipeline is optimal",
            Bottleneck::InputStarved => "Increase FIFO depth or add more producer threads",
            Bottleneck::OutputStalled => {
                "Speed up consumer threads or increase output processing rate"
            }
            Bottleneck::BothEnds => {
                "Critical: Both input and output bottlenecks detected - review entire pipeline"
            }
        }
    }
}

impl Default for PipelineMetrics {
    fn default() -> Self {
        Self::new()
    }
}

/// Atomic performance counter for thread-safe updates
#[derive(Debug)]
pub struct PerformanceCounter {
    /// Total receive tensor wait time (nanoseconds)
    receive_time_ns: AtomicU64,

    /// Total compute time (nanoseconds)
    compute_time_ns: AtomicU64,

    /// Total inferences
    inference_count: AtomicU64,

    /// Start time for rate calculation
    start_time: Instant,
}

impl PerformanceCounter {
    /// Create new performance counter
    pub fn new() -> Self {
        Self {
            receive_time_ns: AtomicU64::new(0),
            compute_time_ns: AtomicU64::new(0),
            inference_count: AtomicU64::new(0),
            start_time: Instant::now(),
        }
    }

    /// Record receive tensor wait time
    pub fn record_receive_time(&self, duration: Duration) {
        self.receive_time_ns
            .fetch_add(duration.as_nanos() as u64, Ordering::Relaxed);
    }

    /// Record compute time
    pub fn record_compute_time(&self, duration: Duration) {
        self.compute_time_ns
            .fetch_add(duration.as_nanos() as u64, Ordering::Relaxed);
    }

    /// Increment inference count
    pub fn increment_inference(&self) {
        self.inference_count.fetch_add(1, Ordering::Relaxed);
    }

    /// Get current metrics snapshot
    pub fn snapshot(&self, input_depth: usize, output_depth: usize) -> PipelineMetrics {
        let receive_ns = self.receive_time_ns.load(Ordering::Relaxed);
        let compute_ns = self.compute_time_ns.load(Ordering::Relaxed);
        let count = self.inference_count.load(Ordering::Relaxed);

        PipelineMetrics {
            receive_tensor_time: Duration::from_nanos(receive_ns),
            compute_time: Duration::from_nanos(compute_ns),
            input_queue_depth: input_depth,
            output_queue_depth: output_depth,
            total_inferences: count,
            last_update: Instant::now(),
        }
    }

    /// Reset all counters
    pub fn reset(&self) {
        self.receive_time_ns.store(0, Ordering::Relaxed);
        self.compute_time_ns.store(0, Ordering::Relaxed);
        self.inference_count.store(0, Ordering::Relaxed);
    }

    /// Get elapsed time since counter creation
    pub fn elapsed(&self) -> Duration {
        self.start_time.elapsed()
    }

    /// Calculate current throughput (inferences per second)
    pub fn throughput(&self) -> f64 {
        let count = self.inference_count.load(Ordering::Relaxed);
        let elapsed = self.elapsed().as_secs_f64();
        if elapsed < 0.001 {
            return 0.0;
        }
        count as f64 / elapsed
    }

    /// Get total number of inferences
    pub fn total_inferences(&self) -> u64 {
        self.inference_count.load(Ordering::Relaxed)
    }
}

impl Default for PerformanceCounter {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_optimal_pipeline() {
        let mut metrics = PipelineMetrics::new();
        metrics.receive_tensor_time = Duration::from_micros(50); // Good
        metrics.compute_time = Duration::from_millis(10);
        metrics.input_queue_depth = 3;
        metrics.output_queue_depth = 1;

        assert!(metrics.is_optimal());
        assert_eq!(metrics.bottleneck(), Bottleneck::None);
    }

    #[test]
    fn test_input_starved() {
        let mut metrics = PipelineMetrics::new();
        metrics.receive_tensor_time = Duration::from_millis(5); // Bad
        metrics.input_queue_depth = 0;

        assert!(!metrics.is_optimal());
        assert_eq!(metrics.bottleneck(), Bottleneck::InputStarved);
    }

    #[test]
    fn test_output_stalled() {
        let mut metrics = PipelineMetrics::new();
        metrics.receive_tensor_time = Duration::from_micros(50);
        metrics.output_queue_depth = 10; // Full queue

        assert_eq!(metrics.bottleneck(), Bottleneck::OutputStalled);
    }

    #[test]
    fn test_performance_counter() {
        let counter = PerformanceCounter::new();

        counter.record_receive_time(Duration::from_micros(100));
        counter.record_compute_time(Duration::from_millis(10));
        counter.increment_inference();

        let metrics = counter.snapshot(2, 1);
        assert_eq!(metrics.total_inferences, 1);
        assert!(metrics.receive_tensor_time >= Duration::from_micros(100));
        assert!(metrics.compute_time >= Duration::from_millis(10));
    }

    #[test]
    fn test_efficiency_calculation() {
        let mut metrics = PipelineMetrics::new();
        metrics.receive_tensor_time = Duration::from_millis(1);
        metrics.compute_time = Duration::from_millis(9);

        // 90% efficiency (9ms compute / 10ms total)
        let eff = metrics.efficiency();
        assert!((eff - 0.9).abs() < 0.01);
    }
}
