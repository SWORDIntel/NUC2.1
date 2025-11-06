//! Optimal configuration constants based on ncappzoo analysis

/// Optimal threading configuration (from benchmark_ncs.py)
pub mod threading {
    /// Optimal number of threads per device for Myriad X
    /// Myriad X has 2 NCEs and performs best with 3 threads
    pub const THREADS_PER_DEVICE: usize = 3;

    /// Number of simultaneous async inference requests per thread
    pub const ASYNC_REQUESTS_PER_THREAD: usize = 6;

    /// Total concurrent requests per device (3 × 6 = 18)
    pub const TOTAL_REQUESTS_PER_DEVICE: usize = THREADS_PER_DEVICE * ASYNC_REQUESTS_PER_THREAD;

    /// Calculate global thread index from device and local thread indices
    #[inline]
    pub const fn thread_global_index(device_idx: usize, thread_idx: usize) -> usize {
        thread_idx + (THREADS_PER_DEVICE * device_idx)
    }

    /// Calculate device index from global thread index
    #[inline]
    pub const fn device_from_thread(global_idx: usize) -> usize {
        global_idx / THREADS_PER_DEVICE
    }

    /// Calculate local thread index from global thread index
    #[inline]
    pub const fn thread_local_index(global_idx: usize) -> usize {
        global_idx % THREADS_PER_DEVICE
    }
}

/// FIFO queue depth configuration
pub mod fifo {
    /// Default FIFO depth (optimal for most workloads)
    pub const DEFAULT_DEPTH: usize = 4;

    /// Minimum recommended FIFO depth
    pub const MIN_DEPTH: usize = 2;

    /// Maximum practical FIFO depth before diminishing returns
    pub const MAX_DEPTH: usize = 10;

    /// Result queue size for consumer threads (from benchmark_ncs.py)
    pub const RESULT_QUEUE_SIZE: usize = 6;

    /// Determine optimal FIFO depth based on tensor size
    #[inline]
    pub const fn optimal_depth_for_size(tensor_bytes: usize) -> usize {
        match tensor_bytes {
            0..=1_000_000 => 6,          // Small tensors: deeper queue
            1_000_001..=10_000_000 => 4, // Medium: default
            _ => 2,                      // Large: shallow queue
        }
    }
}

/// VPU hardware configuration flags
pub mod vpu {
    /// Enable hardware pipeline optimization
    /// WARNING: Requires models compiled with --scale (4, 8, or 16)
    /// Can cause precision issues with half-precision overflow otherwise
    pub const HW_STAGES_OPTIMIZATION: bool = true;

    /// Tensor memory layout format
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub enum ComputeLayout {
        /// Native VPU format (NCHW)
        VpuNchw,
        /// Alternative format (NHWC)
        VpuNhwc,
    }

    impl Default for ComputeLayout {
        fn default() -> Self {
            Self::VpuNchw
        }
    }

    /// Enable reshape optimization
    pub const RESHAPE_OPTIMIZATION: bool = false;
}

/// Performance monitoring thresholds
pub mod performance {
    use std::time::Duration;

    /// Maximum acceptable time for VPU to wait for input tensor
    /// In optimal pipeline, this should be near zero
    pub const MAX_RECEIVE_TENSOR_TIME: Duration = Duration::from_micros(100);

    /// Timeout for blocking FIFO read operations
    pub const FIFO_READ_TIMEOUT: Duration = Duration::from_secs(10);

    /// Timeout for blocking FIFO write operations
    pub const FIFO_WRITE_TIMEOUT: Duration = Duration::from_secs(10);

    /// Memory usage warning threshold (percentage)
    pub const MEMORY_WARNING_THRESHOLD: u8 = 80;

    /// Memory usage critical threshold (percentage)
    pub const MEMORY_CRITICAL_THRESHOLD: u8 = 90;
}

/// Thermal management
pub mod thermal {
    /// Thermal throttling levels
    #[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
    #[repr(u8)]
    pub enum ThrottleLevel {
        /// No throttling
        None = 0,
        /// Lower threshold reached (warning)
        Lower = 1,
        /// Upper threshold reached (critical)
        Upper = 2,
    }

    impl ThrottleLevel {
        pub fn from_u32(value: u32) -> Self {
            match value {
                0 => Self::None,
                1 => Self::Lower,
                2 => Self::Upper,
                _ => Self::Upper, // Treat unknown as critical
            }
        }

        pub fn is_throttling(&self) -> bool {
            *self != Self::None
        }

        pub fn is_critical(&self) -> bool {
            *self == Self::Upper
        }
    }

    /// Temperature warning threshold (Celsius)
    pub const WARNING_TEMP_C: f32 = 75.0;

    /// Temperature critical threshold (Celsius)
    pub const CRITICAL_TEMP_C: f32 = 85.0;
}

/// Resource limits
pub mod limits {
    /// Maximum number of devices to enumerate
    pub const MAX_DEVICE_INDEX: i32 = 32;

    /// Maximum graphs per device (memory-limited, typically ~10)
    pub const TYPICAL_MAX_GRAPHS: usize = 10;

    /// Maximum FIFOs per device
    pub const TYPICAL_MAX_FIFOS: usize = 20;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_thread_indexing() {
        assert_eq!(threading::thread_global_index(0, 0), 0);
        assert_eq!(threading::thread_global_index(0, 2), 2);
        assert_eq!(threading::thread_global_index(1, 0), 3);
        assert_eq!(threading::thread_global_index(1, 2), 5);

        assert_eq!(threading::device_from_thread(0), 0);
        assert_eq!(threading::device_from_thread(5), 1);
        assert_eq!(threading::thread_local_index(5), 2);
    }

    #[test]
    fn test_fifo_depth_tuning() {
        assert_eq!(fifo::optimal_depth_for_size(500_000), 6);
        assert_eq!(fifo::optimal_depth_for_size(5_000_000), 4);
        assert_eq!(fifo::optimal_depth_for_size(50_000_000), 2);
    }

    #[test]
    fn test_throttle_levels() {
        assert!(!thermal::ThrottleLevel::None.is_throttling());
        assert!(thermal::ThrottleLevel::Lower.is_throttling());
        assert!(thermal::ThrottleLevel::Upper.is_critical());
    }
}
