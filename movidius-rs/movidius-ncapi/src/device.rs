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
        let fd = self
            .fd
            .ok_or_else(|| Error::InvalidState("Device not opened".to_string()))?;

        let thermal_info = movidius_hal::IoctlInterface::get_thermal(fd)
            .map_err(|e| Error::Hardware(format!("Failed to get thermal stats: {}", e)))?;

        tracing::trace!(
            "Device {} thermal: {}°C (max {}°C, throttle={})",
            self.index,
            thermal_info.current_temp,
            thermal_info.max_temp,
            thermal_info.throttle_level
        );

        Ok((thermal_info.current_temp, thermal_info.max_temp))
    }

    /// Get current thermal throttling level
    pub fn throttling_level(&self) -> Result<crate::config::thermal::ThrottleLevel> {
        let fd = self
            .fd
            .ok_or_else(|| Error::InvalidState("Device not opened".to_string()))?;

        let thermal_info = movidius_hal::IoctlInterface::get_thermal(fd)
            .map_err(|e| Error::Hardware(format!("Failed to get throttling level: {}", e)))?;

        let level = match thermal_info.throttle_level {
            0 => crate::config::thermal::ThrottleLevel::None,
            1 => crate::config::thermal::ThrottleLevel::Lower,
            2 => crate::config::thermal::ThrottleLevel::Upper,
            _ => crate::config::thermal::ThrottleLevel::Upper, // Treat unknown as worst case
        };

        tracing::trace!("Device {} throttling: {:?}", self.index, level);
        Ok(level)
    }

    /// Get current memory usage statistics
    /// Returns (used_bytes, total_bytes)
    pub fn memory_usage(&self) -> Result<(u64, u64)> {
        let fd = self
            .fd
            .ok_or_else(|| Error::InvalidState("Device not opened".to_string()))?;

        let mem_info = movidius_hal::IoctlInterface::get_memory_usage(fd)
            .map_err(|e| Error::Hardware(format!("Failed to get memory usage: {}", e)))?;

        tracing::trace!(
            "Device {} memory: {} / {} bytes ({:.1}%)",
            self.index,
            mem_info.used,
            mem_info.total,
            (mem_info.used as f64 / mem_info.total as f64) * 100.0
        );

        Ok((mem_info.used, mem_info.total))
    }

    /// Get resource allocation counts
    /// Returns (allocated_graphs, max_graphs, allocated_fifos, max_fifos)
    pub fn resource_counts(&self) -> Result<(u32, u32, u32, u32)> {
        let fd = self
            .fd
            .ok_or_else(|| Error::InvalidState("Device not opened".to_string()))?;

        let res_info = movidius_hal::IoctlInterface::get_resources(fd)
            .map_err(|e| Error::Hardware(format!("Failed to get resource counts: {}", e)))?;

        tracing::trace!(
            "Device {} resources: graphs {}/{}, fifos {}/{}",
            self.index,
            res_info.graphs_allocated,
            res_info.graphs_max,
            res_info.fifos_allocated,
            res_info.fifos_max
        );

        Ok((
            res_info.graphs_allocated,
            res_info.graphs_max,
            res_info.fifos_allocated,
            res_info.fifos_max,
        ))
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
        let (graphs_allocated, graphs_max, fifos_allocated, fifos_max) = self.resource_counts()?;

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
