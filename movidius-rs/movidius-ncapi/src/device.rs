//! Device handle and management

use crate::error::{Error, Result};
use crate::status::Status;
use parking_lot::RwLock;
use std::fs::File;
use std::os::unix::io::{AsRawFd, RawFd};
use std::sync::Arc;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum DeviceState {
    Created = 0,
    Opened = 1,
    Closed = 2,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum DeviceHwVersion {
    Ma2450 = 2450,
    Ma2480 = 2480,
}

#[derive(Debug, Clone, Copy)]
pub enum DeviceOption {
    ThermalStats,
    ThrottlingLevel,
    DeviceState,
    CurrentMemoryUsed,
    MemorySize,
    MaxFifoNum,
    AllocatedFifoNum,
    MaxGraphNum,
    AllocatedGraphNum,
    FwVersion,
    DeviceName,
    HwVersion,
}

pub struct Device {
    index: i32,
    state: DeviceState,
    device_file: Option<File>,
    fd: Option<RawFd>,
}

impl Device {
    /// Maximum supported device index
    const MAX_DEVICE_INDEX: i32 = 32;

    pub fn create(index: i32) -> Result<Arc<RwLock<Self>>> {
        // Validate device index
        if index < 0 {
            tracing::error!("Invalid device index: {} (must be >= 0)", index);
            return Err(Error::Status(Status::InvalidParameters));
        }
        if index >= Self::MAX_DEVICE_INDEX {
            tracing::error!(
                "Device index {} exceeds maximum of {}",
                index,
                Self::MAX_DEVICE_INDEX - 1
            );
            return Err(Error::Status(Status::InvalidParameters));
        }

        let dev_path = format!("/dev/movidius_x_vpu_{}", index);
        tracing::debug!("Opening device at {}", dev_path);

        let device_file = File::open(&dev_path).map_err(|e| {
            tracing::error!("Failed to open device {}: {}", dev_path, e);
            Error::Status(Status::DeviceNotFound)
        })?;

        let fd = device_file.as_raw_fd();
        tracing::info!("Device {} created successfully (fd: {})", index, fd);

        Ok(Arc::new(RwLock::new(Self {
            index,
            state: DeviceState::Created,
            device_file: Some(device_file),
            fd: Some(fd),
        })))
    }

    pub fn open(&mut self) -> Result<()> {
        if self.state != DeviceState::Created {
            tracing::error!(
                "Cannot open device {} in state {:?}, must be Created",
                self.index,
                self.state
            );
            return Err(Error::Status(Status::InvalidHandle));
        }

        tracing::debug!("Opening device {}", self.index);
        // TODO: Actual device initialization via ioctl
        self.state = DeviceState::Opened;
        tracing::info!("Device {} opened successfully", self.index);
        Ok(())
    }

    pub fn close(&mut self) -> Result<()> {
        if self.state != DeviceState::Opened {
            tracing::error!(
                "Cannot close device {} in state {:?}, must be Opened",
                self.index,
                self.state
            );
            return Err(Error::Status(Status::InvalidHandle));
        }

        tracing::debug!("Closing device {}", self.index);
        // TODO: Actual device cleanup via ioctl
        self.state = DeviceState::Closed;
        tracing::info!("Device {} closed successfully", self.index);
        Ok(())
    }

    pub fn fd(&self) -> Option<RawFd> {
        self.fd
    }

    pub fn state(&self) -> DeviceState {
        self.state
    }

    /// Get thermal statistics (temperature in Celsius)
    /// Returns (current_temp, max_temp)
    pub fn thermal_stats(&self) -> Result<(f32, f32)> {
        // TODO: Actual ioctl to read temperature
        // For now, return placeholder values
        tracing::trace!("Reading thermal stats for device {}", self.index);
        Ok((45.0, 85.0)) // Mock: current 45°C, max 85°C
    }

    /// Get current thermal throttling level
    pub fn throttling_level(&self) -> Result<crate::config::thermal::ThrottleLevel> {
        // TODO: Actual ioctl to read throttling status
        tracing::trace!("Reading throttling level for device {}", self.index);
        Ok(crate::config::thermal::ThrottleLevel::None)
    }

    /// Get current memory usage statistics
    /// Returns (used_bytes, total_bytes)
    pub fn memory_usage(&self) -> Result<(u64, u64)> {
        // TODO: Actual ioctl to read memory stats
        tracing::trace!("Reading memory usage for device {}", self.index);
        Ok((0, 512 * 1024 * 1024)) // Mock: 512MB total
    }

    /// Get resource allocation counts
    /// Returns (allocated_graphs, max_graphs, allocated_fifos, max_fifos)
    pub fn resource_counts(&self) -> Result<(u32, u32, u32, u32)> {
        // TODO: Actual ioctl to read resource counts
        tracing::trace!("Reading resource counts for device {}", self.index);
        Ok((0, 10, 0, 20)) // Mock values
    }

    /// Check if device is healthy (not throttling, memory OK)
    pub fn is_healthy(&self) -> Result<bool> {
        let throttle = self.throttling_level()?;
        let (used, total) = self.memory_usage()?;
        let memory_percent = (used * 100 / total) as u8;

        let healthy = !throttle.is_throttling()
            && memory_percent < crate::config::performance::MEMORY_CRITICAL_THRESHOLD;

        if !healthy {
            tracing::warn!(
                "Device {} health check: throttle={:?}, memory={}%",
                self.index,
                throttle,
                memory_percent
            );
        }

        Ok(healthy)
    }
}

/// Device performance metrics
#[derive(Debug, Clone)]
pub struct DeviceMetrics {
    /// Current temperature (Celsius)
    pub temperature_c: f32,
    /// Thermal throttling level
    pub throttling: crate::config::thermal::ThrottleLevel,
    /// Memory used (bytes)
    pub memory_used: u64,
    /// Total memory (bytes)
    pub memory_total: u64,
    /// Memory usage percentage
    pub memory_percent: u8,
    /// Allocated graphs
    pub graphs_allocated: u32,
    /// Maximum graphs
    pub graphs_max: u32,
    /// Allocated FIFOs
    pub fifos_allocated: u32,
    /// Maximum FIFOs
    pub fifos_max: u32,
}

impl Device {
    /// Get comprehensive device metrics
    pub fn metrics(&self) -> Result<DeviceMetrics> {
        let (temp, _max_temp) = self.thermal_stats()?;
        let throttling = self.throttling_level()?;
        let (memory_used, memory_total) = self.memory_usage()?;
        let (graphs_allocated, graphs_max, fifos_allocated, fifos_max) =
            self.resource_counts()?;

        let memory_percent = ((memory_used * 100) / memory_total) as u8;

        Ok(DeviceMetrics {
            temperature_c: temp,
            throttling,
            memory_used,
            memory_total,
            memory_percent,
            graphs_allocated,
            graphs_max,
            fifos_allocated,
            fifos_max,
        })
    }
}

unsafe impl Send for Device {}
unsafe impl Sync for Device {}
