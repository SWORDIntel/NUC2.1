//! Multi-device load balancer and work scheduler
//!
//! Based on ncappzoo multi-device patterns for optimal work distribution.
//! Implements round-robin and least-loaded scheduling strategies.

use crate::config::threading::TOTAL_REQUESTS_PER_DEVICE;
use crate::device::Device;
use crate::performance::PerformanceCounter;
use crate::{Error, Result};
use parking_lot::RwLock;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;

/// Load balancing strategy
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SchedulingStrategy {
    /// Round-robin: Simple rotation through devices
    RoundRobin,
    /// Least-loaded: Select device with lowest current load
    LeastLoaded,
    /// Performance-based: Select device with best throughput history
    PerformanceBased,
}

/// Per-device load tracking
#[derive(Debug)]
struct DeviceLoad {
    /// Current number of queued requests
    queued_requests: AtomicUsize,
    /// Performance counter for this device
    perf_counter: PerformanceCounter,
}

impl DeviceLoad {
    fn new() -> Self {
        Self {
            queued_requests: AtomicUsize::new(0),
            perf_counter: PerformanceCounter::new(),
        }
    }

    /// Increment queued request count
    fn increment(&self) {
        self.queued_requests.fetch_add(1, Ordering::Relaxed);
    }

    /// Decrement queued request count
    fn decrement(&self) {
        self.queued_requests.fetch_sub(1, Ordering::Relaxed);
    }

    /// Get current load
    fn load(&self) -> usize {
        self.queued_requests.load(Ordering::Relaxed)
    }

    /// Get performance counter
    fn performance(&self) -> &PerformanceCounter {
        &self.perf_counter
    }
}

/// Multi-device pool with load balancing
pub struct MultiDevicePool {
    /// All managed devices
    devices: Vec<Arc<RwLock<Device>>>,

    /// Load tracking per device
    load_metrics: Vec<DeviceLoad>,

    /// Current scheduling strategy
    strategy: SchedulingStrategy,

    /// Round-robin counter
    rr_counter: AtomicUsize,
}

impl MultiDevicePool {
    /// Create new multi-device pool
    ///
    /// # Arguments
    /// * `device_indices` - Device indices to include in pool (e.g., [0, 1] for dual-device)
    /// * `strategy` - Initial scheduling strategy
    ///
    /// # Example
    /// ```no_run
    /// use movidius_ncapi::scheduler::{MultiDevicePool, SchedulingStrategy};
    ///
    /// // Create pool with devices 0 and 1
    /// let pool = MultiDevicePool::new(&[0, 1], SchedulingStrategy::LeastLoaded)?;
    /// # Ok::<(), movidius_ncapi::Error>(())
    /// ```
    pub fn new(device_indices: &[usize], strategy: SchedulingStrategy) -> Result<Self> {
        if device_indices.is_empty() {
            return Err(Error::InvalidArgument(
                "Must specify at least one device".to_string(),
            ));
        }

        let mut devices = Vec::new();
        let mut load_metrics = Vec::new();

        for &index in device_indices {
            let device = Device::create(index as i32)?;
            devices.push(device);
            load_metrics.push(DeviceLoad::new());
        }

        Ok(Self {
            devices,
            load_metrics,
            strategy,
            rr_counter: AtomicUsize::new(0),
        })
    }

    /// Get number of devices in pool
    pub fn device_count(&self) -> usize {
        self.devices.len()
    }

    /// Get optimal request count per device (from ncappzoo: 3×6=18)
    pub fn optimal_request_count(&self) -> usize {
        TOTAL_REQUESTS_PER_DEVICE
    }

    /// Get total optimal concurrent requests across all devices
    pub fn total_optimal_requests(&self) -> usize {
        TOTAL_REQUESTS_PER_DEVICE * self.device_count()
    }

    /// Change scheduling strategy
    pub fn set_strategy(&mut self, strategy: SchedulingStrategy) {
        self.strategy = strategy;
        tracing::info!("Scheduling strategy changed to {:?}", strategy);
    }

    /// Get current scheduling strategy
    pub fn strategy(&self) -> SchedulingStrategy {
        self.strategy
    }

    /// Select next device for work assignment
    ///
    /// Returns device index in pool (not global device index)
    pub fn select_device(&self) -> usize {
        match self.strategy {
            SchedulingStrategy::RoundRobin => self.round_robin_select(),
            SchedulingStrategy::LeastLoaded => self.least_loaded_select(),
            SchedulingStrategy::PerformanceBased => self.performance_select(),
        }
    }

    /// Round-robin device selection
    fn round_robin_select(&self) -> usize {
        let counter = self.rr_counter.fetch_add(1, Ordering::Relaxed);
        counter % self.device_count()
    }

    /// Least-loaded device selection
    fn least_loaded_select(&self) -> usize {
        self.load_metrics
            .iter()
            .enumerate()
            .min_by_key(|(_, load)| load.load())
            .map(|(idx, _)| idx)
            .unwrap_or(0)
    }

    /// Performance-based device selection (highest throughput)
    fn performance_select(&self) -> usize {
        self.load_metrics
            .iter()
            .enumerate()
            .max_by(|(_, a), (_, b)| {
                a.performance()
                    .throughput()
                    .partial_cmp(&b.performance().throughput())
                    .unwrap_or(std::cmp::Ordering::Equal)
            })
            .map(|(idx, _)| idx)
            .unwrap_or(0)
    }

    /// Get device by pool index
    pub fn get_device(&self, pool_index: usize) -> Option<Arc<RwLock<Device>>> {
        self.devices.get(pool_index).cloned()
    }

    /// Mark request as queued on device
    pub fn mark_queued(&self, pool_index: usize) {
        if let Some(load) = self.load_metrics.get(pool_index) {
            load.increment();
        }
    }

    /// Mark request as completed on device
    pub fn mark_completed(&self, pool_index: usize) {
        if let Some(load) = self.load_metrics.get(pool_index) {
            load.decrement();
        }
    }

    /// Get performance counter for device
    pub fn performance(&self, pool_index: usize) -> Option<&PerformanceCounter> {
        self.load_metrics
            .get(pool_index)
            .map(|load| load.performance())
    }

    /// Get current load for device
    pub fn load(&self, pool_index: usize) -> Option<usize> {
        self.load_metrics.get(pool_index).map(|load| load.load())
    }

    /// Get pool-wide statistics
    pub fn pool_stats(&self) -> PoolStats {
        let total_load: usize = self.load_metrics.iter().map(|load| load.load()).sum();

        let total_throughput: f64 = self
            .load_metrics
            .iter()
            .map(|load| load.performance().throughput())
            .sum();

        let loads: Vec<usize> = self.load_metrics.iter().map(|load| load.load()).collect();

        let max_load = loads.iter().copied().max().unwrap_or(0);
        let min_load = loads.iter().copied().min().unwrap_or(0);

        PoolStats {
            device_count: self.device_count(),
            total_load,
            total_throughput,
            max_load,
            min_load,
            load_imbalance: if max_load > 0 {
                (max_load as f64 - min_load as f64) / max_load as f64
            } else {
                0.0
            },
        }
    }

    /// Check if pool is healthy (all devices operational)
    pub fn is_healthy(&self) -> Result<bool> {
        for device in &self.devices {
            if !device.read().is_healthy()? {
                return Ok(false);
            }
        }
        Ok(true)
    }

    /// Open all devices in pool
    pub fn open_all(&self) -> Result<()> {
        for (idx, device) in self.devices.iter().enumerate() {
            device.write().open().map_err(|e| {
                Error::DeviceError(format!("Failed to open device {}: {}", idx, e))
            })?;
        }
        tracing::info!("Opened {} devices in pool", self.device_count());
        Ok(())
    }

    /// Close all devices in pool
    pub fn close_all(&self) -> Result<()> {
        for (idx, device) in self.devices.iter().enumerate() {
            device.write().close().map_err(|e| {
                Error::DeviceError(format!("Failed to close device {}: {}", idx, e))
            })?;
        }
        tracing::info!("Closed {} devices in pool", self.device_count());
        Ok(())
    }
}

/// Pool-wide statistics
#[derive(Debug, Clone)]
pub struct PoolStats {
    /// Number of devices in pool
    pub device_count: usize,

    /// Total queued requests across all devices
    pub total_load: usize,

    /// Combined throughput (inferences/sec)
    pub total_throughput: f64,

    /// Maximum load on any single device
    pub max_load: usize,

    /// Minimum load on any single device
    pub min_load: usize,

    /// Load imbalance ratio (0.0 = perfect balance, 1.0 = maximum imbalance)
    pub load_imbalance: f64,
}

impl PoolStats {
    /// Check if load is well-balanced (<20% imbalance)
    pub fn is_balanced(&self) -> bool {
        self.load_imbalance < 0.2
    }

    /// Average load per device
    pub fn avg_load(&self) -> f64 {
        if self.device_count == 0 {
            0.0
        } else {
            self.total_load as f64 / self.device_count as f64
        }
    }

    /// Average throughput per device
    pub fn avg_throughput(&self) -> f64 {
        if self.device_count == 0 {
            0.0
        } else {
            self.total_throughput / self.device_count as f64
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_device_load() {
        let load = DeviceLoad::new();
        assert_eq!(load.load(), 0);

        load.increment();
        assert_eq!(load.load(), 1);

        load.increment();
        load.increment();
        assert_eq!(load.load(), 3);

        load.decrement();
        assert_eq!(load.load(), 2);
    }

    #[test]
    fn test_round_robin() {
        let load1 = DeviceLoad::new();
        let load2 = DeviceLoad::new();

        let pool = MultiDevicePool {
            devices: vec![],
            load_metrics: vec![load1, load2],
            strategy: SchedulingStrategy::RoundRobin,
            rr_counter: AtomicUsize::new(0),
        };

        assert_eq!(pool.round_robin_select(), 0);
        assert_eq!(pool.round_robin_select(), 1);
        assert_eq!(pool.round_robin_select(), 0);
        assert_eq!(pool.round_robin_select(), 1);
    }

    #[test]
    fn test_least_loaded() {
        let load1 = DeviceLoad::new();
        let load2 = DeviceLoad::new();

        load1.increment();
        load1.increment();
        load1.increment(); // load1 = 3

        load2.increment(); // load2 = 1

        let pool = MultiDevicePool {
            devices: vec![],
            load_metrics: vec![load1, load2],
            strategy: SchedulingStrategy::LeastLoaded,
            rr_counter: AtomicUsize::new(0),
        };

        // Should select device 1 (lower load)
        assert_eq!(pool.least_loaded_select(), 1);
    }

    #[test]
    fn test_pool_stats() {
        let load1 = DeviceLoad::new();
        let load2 = DeviceLoad::new();

        load1.increment();
        load1.increment(); // load1 = 2

        load2.increment();
        load2.increment();
        load2.increment();
        load2.increment(); // load2 = 4

        let pool = MultiDevicePool {
            devices: vec![],
            load_metrics: vec![load1, load2],
            strategy: SchedulingStrategy::LeastLoaded,
            rr_counter: AtomicUsize::new(0),
        };

        let stats = pool.pool_stats();
        assert_eq!(stats.device_count, 2);
        assert_eq!(stats.total_load, 6);
        assert_eq!(stats.max_load, 4);
        assert_eq!(stats.min_load, 2);
        assert!((stats.avg_load() - 3.0).abs() < 0.01);

        // Imbalance = (4-2)/4 = 0.5
        assert!((stats.load_imbalance - 0.5).abs() < 0.01);
    }

    #[test]
    fn test_mark_queued_completed() {
        let load1 = DeviceLoad::new();
        let load2 = DeviceLoad::new();

        let pool = MultiDevicePool {
            devices: vec![],
            load_metrics: vec![load1, load2],
            strategy: SchedulingStrategy::RoundRobin,
            rr_counter: AtomicUsize::new(0),
        };

        pool.mark_queued(0);
        pool.mark_queued(0);
        assert_eq!(pool.load(0), Some(2));

        pool.mark_completed(0);
        assert_eq!(pool.load(0), Some(1));
    }
}
