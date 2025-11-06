//! IOCTL interface to kernel driver

use super::{Error, Result};
use std::os::unix::io::RawFd;

const MOVIDIUS_IOCTL_GET_DEVICE_INFO: u64 = 0x80184d03;

/// Device information from kernel driver
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct DeviceInfo {
    pub version: u32,
    pub max_batch_size: u32,
    pub total_memory: u64,
    pub num_compute_units: u32,
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
                return Err(Error::Device("ioctl failed".to_string()));
            }
        }

        Ok(info)
    }
}
