//! # Movidius NCAPI v2 - High-Performance Rust Implementation
//!
//! Complete NCAPI v2 implementation with zero-cost abstractions and maximum performance.

#![warn(missing_docs, rust_2018_idioms, clippy::all, clippy::perf)]
#![deny(unsafe_op_in_unsafe_fn)]

pub mod analytics;
pub mod config;
pub mod conversion;
pub mod device;
pub mod error;
pub mod fifo;
pub mod global;
pub mod graph;
pub mod performance;
pub mod scheduler;
pub mod status;
pub mod tensor;
pub mod types;

pub use analytics::{
    AnalysisReport, DeviceMetrics, IssueCategory, IssueSeverity, LatencyTracker, MemoryMetrics,
    MetricsAnalyzer, PerformanceIssue, PerformanceMetrics, PoolMetrics, PoolStatistics,
    ResourceMetrics, ThermalMetrics,
};
pub use device::{Device, DeviceHwVersion, DeviceOption, DeviceState};
pub use error::{Error, Result};
pub use fifo::{Fifo, FifoDataType, FifoOption, FifoState, FifoType};
pub use graph::{Graph, GraphOption, GraphState};
pub use performance::{Bottleneck, PerformanceCounter, PipelineMetrics};
pub use scheduler::{MultiDevicePool, PoolStats, SchedulingStrategy};
pub use status::Status;
pub use tensor::TensorDescriptor;
pub use types::{GlobalOption, LogLevel};

#[cfg(not(target_env = "msvc"))]
pub mod ffi;

pub const VERSION: (u32, u32, u32, u32) = (2, 0, 0, 0);
pub const MAX_NAME_SIZE: usize = 28;
