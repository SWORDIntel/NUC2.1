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
}

unsafe impl Send for Device {}
unsafe impl Sync for Device {}
