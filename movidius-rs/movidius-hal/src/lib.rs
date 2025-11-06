//! Hardware Abstraction Layer for Movidius VPU
//!
//! Provides high-performance io_uring interface, DMA management,
//! and USB protocol handling for maximum throughput.

#![warn(missing_docs, rust_2018_idioms)]

pub mod dma;
pub mod ioctl;
pub mod uring;
pub mod usb;

pub use dma::DmaArena;
pub use ioctl::{DeviceInfo, IoctlInterface};
pub use uring::UringSubmitter;

/// Result type for HAL operations
pub type Result<T> = std::result::Result<T, Error>;

/// HAL error types
#[derive(Debug, thiserror::Error)]
pub enum Error {
    /// IO error
    #[error("IO error: {0}")]
    Io(#[from] std::io::Error),

    /// io_uring error
    #[error("io_uring error: {0}")]
    Uring(String),

    /// DMA error
    #[error("DMA error: {0}")]
    Dma(String),

    /// Device error
    #[error("Device error: {0}")]
    Device(String),
}
