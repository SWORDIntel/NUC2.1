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
    pub fn create(index: i32) -> Result<Arc<RwLock<Self>>> {
        let dev_path = format!("/dev/movidius_x_vpu_{}", index);
        let device_file = File::open(&dev_path)
            .map_err(|_| Error::Status(Status::DeviceNotFound))?;

        let fd = device_file.as_raw_fd();

        Ok(Arc::new(RwLock::new(Self {
            index,
            state: DeviceState::Created,
            device_file: Some(device_file),
            fd: Some(fd),
        })))
    }

    pub fn open(&mut self) -> Result<()> {
        if self.state != DeviceState::Created {
            return Err(Error::Status(Status::InvalidHandle));
        }

        self.state = DeviceState::Opened;
        Ok(())
    }

    pub fn close(&mut self) -> Result<()> {
        if self.state != DeviceState::Opened {
            return Err(Error::Status(Status::InvalidHandle));
        }

        self.state = DeviceState::Closed;
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
