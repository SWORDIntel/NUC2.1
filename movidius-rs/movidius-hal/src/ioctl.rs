//! IOCTL interface to kernel driver
//!
//! Provides direct communication with the Movidius kernel driver for:
//! - Device enumeration and capabilities
//! - Thermal monitoring and throttling
//! - Memory usage tracking
//! - Resource management

use super::{Error, Result};
use std::os::unix::io::RawFd;

// IOCTL command codes (must match kernel driver)
const MOVIDIUS_IOCTL_GET_DEVICE_INFO: u64 = 0x80184d03;
const MOVIDIUS_IOCTL_GET_THERMAL: u64 = 0x80104d04;
const MOVIDIUS_IOCTL_GET_MEMORY_USAGE: u64 = 0x80104d05;
const MOVIDIUS_IOCTL_GET_RESOURCES: u64 = 0x80184d06;

/// Device information from kernel driver
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct DeviceInfo {
    /// Hardware version
    pub version: u32,
    /// Maximum batch size supported
    pub max_batch_size: u32,
    /// Total memory in bytes
    pub total_memory: u64,
    /// Number of compute units (SHAVE processors)
    pub num_compute_units: u32,
}

/// Thermal information
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ThermalInfo {
    /// Current temperature in Celsius
    pub current_temp: f32,
    /// Maximum safe temperature in Celsius
    pub max_temp: f32,
    /// Throttling level (0=none, 1=lower, 2=upper)
    pub throttle_level: u8,
    /// Reserved for future use
    _reserved: [u8; 3],
}

/// Memory usage information
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct MemoryUsage {
    /// Total memory available in bytes
    pub total: u64,
    /// Currently used memory in bytes
    pub used: u64,
}

/// Resource allocation information
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ResourceInfo {
    /// Number of allocated graphs
    pub graphs_allocated: u32,
    /// Maximum graphs supported
    pub graphs_max: u32,
    /// Number of allocated FIFOs
    pub fifos_allocated: u32,
    /// Maximum FIFOs supported
    pub fifos_max: u32,
}

/// IOCTL interface
pub struct IoctlInterface;

impl IoctlInterface {
    /// Get device info
    pub fn get_device_info(fd: RawFd) -> Result<DeviceInfo> {
        let mut info = DeviceInfo {
            version: 0,
            max_batch_size: 0,
            total_memory: 0,
            num_compute_units: 0,
        };

        unsafe {
            let ret = libc::ioctl(fd, MOVIDIUS_IOCTL_GET_DEVICE_INFO, &mut info);
            if ret < 0 {
                return Err(Error::Device("ioctl GET_DEVICE_INFO failed".to_string()));
            }
        }

        Ok(info)
    }

    /// Get thermal information
    pub fn get_thermal(fd: RawFd) -> Result<ThermalInfo> {
        let mut info = ThermalInfo {
            current_temp: 0.0,
            max_temp: 0.0,
            throttle_level: 0,
            _reserved: [0; 3],
        };

        unsafe {
            let ret = libc::ioctl(fd, MOVIDIUS_IOCTL_GET_THERMAL, &mut info);
            if ret < 0 {
                // If ioctl not supported, return safe defaults
                tracing::warn!("IOCTL GET_THERMAL not supported, using defaults");
                return Ok(ThermalInfo {
                    current_temp: 45.0,
                    max_temp: 85.0,
                    throttle_level: 0,
                    _reserved: [0; 3],
                });
            }
        }

        Ok(info)
    }

    /// Get memory usage
    pub fn get_memory_usage(fd: RawFd) -> Result<MemoryUsage> {
        let mut info = MemoryUsage { total: 0, used: 0 };

        unsafe {
            let ret = libc::ioctl(fd, MOVIDIUS_IOCTL_GET_MEMORY_USAGE, &mut info);
            if ret < 0 {
                // If ioctl not supported, try to get total from device info
                tracing::warn!("IOCTL GET_MEMORY_USAGE not supported, using defaults");
                return Ok(MemoryUsage {
                    total: 512 * 1024 * 1024, // 512MB default for Myriad X
                    used: 0,
                });
            }
        }

        Ok(info)
    }

    /// Get resource allocation info
    pub fn get_resources(fd: RawFd) -> Result<ResourceInfo> {
        let mut info = ResourceInfo {
            graphs_allocated: 0,
            graphs_max: 0,
            fifos_allocated: 0,
            fifos_max: 0,
        };

        unsafe {
            let ret = libc::ioctl(fd, MOVIDIUS_IOCTL_GET_RESOURCES, &mut info);
            if ret < 0 {
                // If ioctl not supported, return conservative defaults
                tracing::warn!("IOCTL GET_RESOURCES not supported, using defaults");
                return Ok(ResourceInfo {
                    graphs_allocated: 0,
                    graphs_max: 8, // Typical limit
                    fifos_allocated: 0,
                    fifos_max: 16, // Typical limit
                });
            }
        }

        Ok(info)
    }
}
