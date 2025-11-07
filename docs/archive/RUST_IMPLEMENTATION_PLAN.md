# Rust Implementation Plan for NCAPI v2 Compatible Movidius Driver

## Overview

Based on the NCAPI v2 documentation analysis, this document outlines a **Rust-first** implementation strategy for achieving NCAPI compatibility while maintaining the high performance characteristics of the current driver.

---

## Architecture: Three-Tier Rust Implementation

```
┌─────────────────────────────────────────────────┐
│  User Applications                              │
│  - C applications via FFI                       │
│  - Rust applications via native API             │
│  - Python via PyO3 bindings (if needed)         │
└─────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────┐
│  Tier 1: libmovidius-ncapi (Rust)              │
│  ~/movidius-ncapi-rs/                           │
│  - Pure Rust NCAPI v2 implementation            │
│  - Device/Graph/FIFO abstractions               │
│  - Graph file parsing                           │
│  - Tensor descriptors                           │
│  - State machines                               │
│  - C FFI layer (for C compatibility)            │
│  - Optional: PyO3 bindings                      │
└─────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────┐
│  Tier 2: libmovidius-hal (Rust)                │
│  ~/movidius-hal-rs/                             │
│  - Hardware abstraction layer                   │
│  - io_uring wrapper                             │
│  - DMA management                               │
│  - USB protocol handling                        │
└─────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────┐
│  Tier 3: Kernel Driver (C/Rust)                │
│  - movidius_x_vpu.ko (existing C)               │
│  OR                                             │
│  - movidius_x_vpu_rs.ko (new Rust driver)       │
└─────────────────────────────────────────────────┘
```

---

## Tier 1: Pure Rust NCAPI v2 Library

### Project Structure
```
movidius-ncapi-rs/
├── Cargo.toml
├── src/
│   ├── lib.rs              # Main library entry
│   ├── device.rs           # Device handle and management
│   ├── graph.rs            # Graph handle and management
│   ├── fifo.rs             # FIFO handle and management
│   ├── tensor.rs           # Tensor descriptors
│   ├── status.rs           # Status codes and error types
│   ├── types.rs            # NCAPI types and enums
│   ├── global.rs           # Global options
│   ├── graph_file.rs       # Graph file parser
│   ├── conversion.rs       # FP32 ↔ FP16 conversion
│   ├── ffi.rs              # C FFI bindings
│   └── hal.rs              # HAL interface
├── examples/
│   ├── basic_inference.rs
│   ├── async_inference.rs
│   └── c_compat_test.c
├── tests/
│   ├── device_tests.rs
│   ├── graph_tests.rs
│   └── fifo_tests.rs
└── benches/
    └── inference_benchmark.rs
```

### Core Implementation

#### 1. Status Codes (src/status.rs)
```rust
use thiserror::Error;

#[derive(Error, Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum NcStatus {
    #[error("Success")]
    Ok = 0,

    #[error("Device busy, retry later")]
    Busy = -1,

    #[error("Unexpected error")]
    Error = -2,

    #[error("Host out of memory")]
    OutOfMemory = -3,

    #[error("No device found at index or name")]
    DeviceNotFound = -4,

    #[error("Invalid parameter(s)")]
    InvalidParameters = -5,

    #[error("Communication timeout")]
    Timeout = -6,

    #[error("Boot file not found")]
    MvcmdNotFound = -7,

    #[error("Graph or FIFO not allocated")]
    NotAllocated = -8,

    #[error("Unauthorized operation")]
    Unauthorized = -9,

    #[error("Unsupported graph file")]
    UnsupportedGraphFile = -10,

    #[error("Unsupported configuration file")]
    UnsupportedConfigurationFile = -11,

    #[error("Feature not supported by firmware")]
    UnsupportedFeature = -12,

    #[error("VPU-level error")]
    MyriadError = -13,

    #[error("Invalid data length")]
    InvalidDataLength = -14,

    #[error("Invalid handle")]
    InvalidHandle = -15,
}

pub type Result<T> = std::result::Result<T, NcStatus>;
```

#### 2. Device Handle (src/device.rs)
```rust
use std::fs::File;
use std::os::unix::io::{AsRawFd, RawFd};
use std::sync::{Arc, Mutex};
use io_uring::IoUring;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum DeviceState {
    Created = 0,
    Opened = 1,
    Closed = 2,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DeviceHwVersion {
    Ma2450 = 2450,
    Ma2480 = 2480,
}

pub struct DeviceHandle {
    index: i32,
    state: DeviceState,
    device_file: Option<File>,
    ring: Option<IoUring>,
    info: DeviceInfo,
    graphs: Vec<Arc<Mutex<GraphHandle>>>,
}

#[derive(Debug, Clone)]
pub struct DeviceInfo {
    pub hw_version: DeviceHwVersion,
    pub fw_version: [u32; 4],
    pub memory_size: u64,
    pub current_memory_used: u64,
    pub max_fifo_num: u32,
    pub allocated_fifo_num: u32,
    pub max_graph_num: u32,
    pub allocated_graph_num: u32,
    pub thermal_stats: Vec<f32>,
    pub throttling_level: u32,
    pub device_name: String,
}

impl DeviceHandle {
    /// Create a new device handle
    pub fn create(index: i32) -> Result<Self> {
        let dev_path = format!("/dev/movidius_x_vpu_{}", index);
        let device_file = File::open(&dev_path)
            .map_err(|_| NcStatus::DeviceNotFound)?;

        Ok(Self {
            index,
            state: DeviceState::Created,
            device_file: Some(device_file),
            ring: None,
            info: DeviceInfo::default(),
            graphs: Vec::new(),
        })
    }

    /// Open communication with device
    pub fn open(&mut self) -> Result<()> {
        if self.state != DeviceState::Created {
            return Err(NcStatus::InvalidHandle);
        }

        // Initialize io_uring
        let ring = IoUring::new(64)
            .map_err(|_| NcStatus::Error)?;

        self.ring = Some(ring);

        // Query device info
        self.query_device_info()?;

        self.state = DeviceState::Opened;
        Ok(())
    }

    /// Close communication with device
    pub fn close(&mut self) -> Result<()> {
        if self.state != DeviceState::Opened {
            return Err(NcStatus::InvalidHandle);
        }

        // Verify all graphs are destroyed
        if !self.graphs.is_empty() {
            return Err(NcStatus::Error);
        }

        self.ring = None;
        self.state = DeviceState::Closed;
        Ok(())
    }

    /// Get device option
    pub fn get_option(&self, option: DeviceOption) -> Result<DeviceOptionValue> {
        if self.state != DeviceState::Opened {
            return Err(NcStatus::InvalidHandle);
        }

        match option {
            DeviceOption::ThermalStats => {
                Ok(DeviceOptionValue::ThermalStats(self.info.thermal_stats.clone()))
            }
            DeviceOption::ThrottlingLevel => {
                Ok(DeviceOptionValue::Int(self.info.throttling_level as i32))
            }
            DeviceOption::DeviceState => {
                Ok(DeviceOptionValue::Int(self.state as i32))
            }
            DeviceOption::CurrentMemoryUsed => {
                Ok(DeviceOptionValue::Int(self.info.current_memory_used as i32))
            }
            DeviceOption::MemorySize => {
                Ok(DeviceOptionValue::Int(self.info.memory_size as i32))
            }
            DeviceOption::FwVersion => {
                Ok(DeviceOptionValue::FwVersion(self.info.fw_version))
            }
            DeviceOption::HwVersion => {
                Ok(DeviceOptionValue::Int(self.info.hw_version as i32))
            }
            // ... other options
        }
    }

    fn query_device_info(&mut self) -> Result<()> {
        // Issue ioctl to get device info
        let fd = self.device_file.as_ref().unwrap().as_raw_fd();

        #[repr(C)]
        struct DeviceInfoC {
            version: u32,
            max_batch_size: u32,
            total_memory: u64,
            num_compute_units: u32,
        }

        let mut info = DeviceInfoC {
            version: 0,
            max_batch_size: 0,
            total_memory: 0,
            num_compute_units: 0,
        };

        const MOVIDIUS_IOCTL_GET_DEVICE_INFO: u64 = 0x80184d03;

        unsafe {
            let ret = libc::ioctl(fd, MOVIDIUS_IOCTL_GET_DEVICE_INFO, &mut info);
            if ret < 0 {
                return Err(NcStatus::Error);
            }
        }

        self.info.memory_size = info.total_memory;
        // ... populate other fields

        Ok(())
    }

    pub fn fd(&self) -> RawFd {
        self.device_file.as_ref().unwrap().as_raw_fd()
    }
}

impl Drop for DeviceHandle {
    fn drop(&mut self) {
        if self.state == DeviceState::Opened {
            let _ = self.close();
        }
    }
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

#[derive(Debug, Clone)]
pub enum DeviceOptionValue {
    Int(i32),
    Float(f32),
    ThermalStats(Vec<f32>),
    FwVersion([u32; 4]),
    String(String),
}
```

#### 3. Graph Handle (src/graph.rs)
```rust
use super::device::DeviceHandle;
use super::fifo::{FifoHandle, FifoType, FifoDataType};
use super::tensor::TensorDescriptor;
use super::status::*;
use std::sync::{Arc, Mutex};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum GraphState {
    Created = 0,
    Allocated = 1,
    WaitingForBuffers = 2,
    Running = 3,
}

pub struct GraphHandle {
    name: String,
    state: GraphState,
    device: Option<Arc<Mutex<DeviceHandle>>>,
    graph_buffer: Option<Vec<u8>>,
    input_fifos: Vec<Arc<Mutex<FifoHandle>>>,
    output_fifos: Vec<Arc<Mutex<FifoHandle>>>,
    input_descriptors: Vec<TensorDescriptor>,
    output_descriptors: Vec<TensorDescriptor>,
    num_executors: u32,
    time_taken: Vec<f32>,
}

impl GraphHandle {
    /// Create a new graph handle
    pub fn create(name: &str) -> Result<Self> {
        if name.len() >= 28 {  // NC_MAX_NAME_SIZE
            return Err(NcStatus::InvalidParameters);
        }

        Ok(Self {
            name: name.to_string(),
            state: GraphState::Created,
            device: None,
            graph_buffer: None,
            input_fifos: Vec::new(),
            output_fifos: Vec::new(),
            input_descriptors: Vec::new(),
            output_descriptors: Vec::new(),
            num_executors: 1,
            time_taken: Vec::new(),
        })
    }

    /// Allocate graph to device
    pub fn allocate(
        &mut self,
        device: Arc<Mutex<DeviceHandle>>,
        graph_buffer: &[u8],
    ) -> Result<()> {
        if self.state != GraphState::Created {
            return Err(NcStatus::InvalidHandle);
        }

        // Parse graph file
        let graph_meta = parse_graph_file(graph_buffer)?;

        self.graph_buffer = Some(graph_buffer.to_vec());
        self.device = Some(device);
        self.input_descriptors = graph_meta.input_descriptors;
        self.output_descriptors = graph_meta.output_descriptors;

        // TODO: Actually allocate to device via ioctl

        self.state = GraphState::Allocated;
        Ok(())
    }

    /// Allocate graph with default FIFOs
    pub fn allocate_with_fifos(
        &mut self,
        device: Arc<Mutex<DeviceHandle>>,
        graph_buffer: &[u8],
    ) -> Result<(Arc<Mutex<FifoHandle>>, Arc<Mutex<FifoHandle>>)> {
        self.allocate(device.clone(), graph_buffer)?;

        // Create input FIFO
        let mut input_fifo = FifoHandle::create("input", FifoType::HostWo)?;
        input_fifo.allocate(
            device.clone(),
            &self.input_descriptors[0],
            2,  // default: 2 elements
        )?;

        // Create output FIFO
        let mut output_fifo = FifoHandle::create("output", FifoType::HostRo)?;
        output_fifo.allocate(
            device.clone(),
            &self.output_descriptors[0],
            2,  // default: 2 elements
        )?;

        let input_fifo = Arc::new(Mutex::new(input_fifo));
        let output_fifo = Arc::new(Mutex::new(output_fifo));

        self.input_fifos.push(input_fifo.clone());
        self.output_fifos.push(output_fifo.clone());

        Ok((input_fifo, output_fifo))
    }

    /// Queue inference
    pub fn queue_inference(
        &mut self,
        input_fifos: &[Arc<Mutex<FifoHandle>>],
        output_fifos: &[Arc<Mutex<FifoHandle>>],
    ) -> Result<()> {
        if self.state != GraphState::Allocated &&
           self.state != GraphState::WaitingForBuffers {
            return Err(NcStatus::NotAllocated);
        }

        self.state = GraphState::Running;

        // TODO: Queue inference to device

        self.state = GraphState::WaitingForBuffers;
        Ok(())
    }

    /// Queue inference with FIFO elements
    pub fn queue_inference_with_fifo_elem(
        &mut self,
        input_fifo: Arc<Mutex<FifoHandle>>,
        output_fifo: Arc<Mutex<FifoHandle>>,
        input_tensor: &[u8],
        user_param: Option<*mut std::ffi::c_void>,
    ) -> Result<()> {
        // Write to input FIFO
        {
            let mut fifo = input_fifo.lock().unwrap();
            fifo.write_elem(input_tensor, user_param)?;
        }

        // Queue inference
        self.queue_inference(&[input_fifo], &[output_fifo])?;

        Ok(())
    }

    /// Get graph option
    pub fn get_option(&self, option: GraphOption) -> Result<GraphOptionValue> {
        match option {
            GraphOption::State => {
                Ok(GraphOptionValue::Int(self.state as i32))
            }
            GraphOption::TimeTaken => {
                Ok(GraphOptionValue::TimeTaken(self.time_taken.clone()))
            }
            GraphOption::InputCount => {
                Ok(GraphOptionValue::Int(self.input_descriptors.len() as i32))
            }
            GraphOption::OutputCount => {
                Ok(GraphOptionValue::Int(self.output_descriptors.len() as i32))
            }
            GraphOption::InputTensorDescriptors => {
                Ok(GraphOptionValue::TensorDescriptors(self.input_descriptors.clone()))
            }
            GraphOption::OutputTensorDescriptors => {
                Ok(GraphOptionValue::TensorDescriptors(self.output_descriptors.clone()))
            }
            GraphOption::ExecutorsNum => {
                Ok(GraphOptionValue::Int(self.num_executors as i32))
            }
            GraphOption::Name => {
                Ok(GraphOptionValue::String(self.name.clone()))
            }
        }
    }

    /// Set graph option
    pub fn set_option(&mut self, option: GraphOption, value: GraphOptionValue) -> Result<()> {
        if self.state != GraphState::Created {
            return Err(NcStatus::InvalidHandle);
        }

        match option {
            GraphOption::ExecutorsNum => {
                if let GraphOptionValue::Int(num) = value {
                    self.num_executors = num as u32;
                    Ok(())
                } else {
                    Err(NcStatus::InvalidParameters)
                }
            }
            _ => Err(NcStatus::InvalidParameters),  // Read-only options
        }
    }
}

#[derive(Debug, Clone, Copy)]
pub enum GraphOption {
    State,
    TimeTaken,
    InputCount,
    OutputCount,
    InputTensorDescriptors,
    OutputTensorDescriptors,
    ExecutorsNum,
    Name,
}

#[derive(Debug, Clone)]
pub enum GraphOptionValue {
    Int(i32),
    TimeTaken(Vec<f32>),
    TensorDescriptors(Vec<TensorDescriptor>),
    String(String),
}

struct GraphMetadata {
    input_descriptors: Vec<TensorDescriptor>,
    output_descriptors: Vec<TensorDescriptor>,
}

fn parse_graph_file(buffer: &[u8]) -> Result<GraphMetadata> {
    // TODO: Parse actual Movidius graph file format
    // For now, return dummy descriptors
    Ok(GraphMetadata {
        input_descriptors: vec![TensorDescriptor::default()],
        output_descriptors: vec![TensorDescriptor::default()],
    })
}
```

#### 4. FIFO Handle (src/fifo.rs)
```rust
use super::device::DeviceHandle;
use super::tensor::TensorDescriptor;
use super::status::*;
use super::conversion::{fp32_to_fp16, fp16_to_fp32};
use std::collections::VecDeque;
use std::sync::{Arc, Mutex};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum FifoState {
    Created = 0,
    Allocated = 1,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum FifoType {
    HostRo = 0,  // Host read-only (for outputs)
    HostWo = 1,  // Host write-only (for inputs)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum FifoDataType {
    Fp16 = 0,
    Fp32 = 1,
}

pub struct FifoHandle {
    name: String,
    state: FifoState,
    fifo_type: FifoType,
    data_type: FifoDataType,
    device: Option<Arc<Mutex<DeviceHandle>>>,
    tensor_desc: TensorDescriptor,
    capacity: usize,
    queue: VecDeque<FifoElement>,
    dont_block: bool,
}

struct FifoElement {
    data: Vec<u8>,
    user_param: Option<*mut std::ffi::c_void>,
}

impl FifoHandle {
    /// Create a new FIFO handle
    pub fn create(name: &str, fifo_type: FifoType) -> Result<Self> {
        if name.len() >= 28 {  // NC_MAX_NAME_SIZE
            return Err(NcStatus::InvalidParameters);
        }

        Ok(Self {
            name: name.to_string(),
            state: FifoState::Created,
            fifo_type,
            data_type: FifoDataType::Fp32,  // default
            device: None,
            tensor_desc: TensorDescriptor::default(),
            capacity: 0,
            queue: VecDeque::new(),
            dont_block: false,
        })
    }

    /// Allocate FIFO
    pub fn allocate(
        &mut self,
        device: Arc<Mutex<DeviceHandle>>,
        tensor_desc: &TensorDescriptor,
        num_elem: usize,
    ) -> Result<()> {
        if self.state != FifoState::Created {
            return Err(NcStatus::InvalidHandle);
        }

        self.device = Some(device);
        self.tensor_desc = *tensor_desc;
        self.capacity = num_elem;
        self.queue = VecDeque::with_capacity(num_elem);

        // TODO: Allocate FIFO memory on device

        self.state = FifoState::Allocated;
        Ok(())
    }

    /// Write element to FIFO
    pub fn write_elem(
        &mut self,
        input_tensor: &[u8],
        user_param: Option<*mut std::ffi::c_void>,
    ) -> Result<()> {
        if self.state != FifoState::Allocated {
            return Err(NcStatus::NotAllocated);
        }

        if self.fifo_type != FifoType::HostWo {
            return Err(NcStatus::Unauthorized);
        }

        // Check queue capacity
        if self.queue.len() >= self.capacity {
            if self.dont_block {
                return Err(NcStatus::Busy);
            }
            // TODO: Block until space available
        }

        // Validate size
        let expected_size = self.tensor_desc.total_size as usize;
        if input_tensor.len() != expected_size {
            return Err(NcStatus::InvalidDataLength);
        }

        // Convert data type if needed
        let data = if self.data_type == FifoDataType::Fp16 &&
                     self.tensor_desc.data_type == FifoDataType::Fp32 {
            // Convert FP32 to FP16
            let fp32_data: &[f32] = unsafe {
                std::slice::from_raw_parts(
                    input_tensor.as_ptr() as *const f32,
                    input_tensor.len() / 4
                )
            };
            let fp16_data = fp32_to_fp16(fp32_data);
            fp16_data.iter()
                .flat_map(|&x| x.to_le_bytes())
                .collect()
        } else {
            input_tensor.to_vec()
        };

        self.queue.push_back(FifoElement {
            data,
            user_param,
        });

        Ok(())
    }

    /// Read element from FIFO
    pub fn read_elem(&mut self) -> Result<(Vec<u8>, Option<*mut std::ffi::c_void>)> {
        if self.state != FifoState::Allocated {
            return Err(NcStatus::NotAllocated);
        }

        if self.fifo_type != FifoType::HostRo {
            return Err(NcStatus::Unauthorized);
        }

        // Check if queue has data
        if self.queue.is_empty() {
            if self.dont_block {
                return Err(NcStatus::Busy);
            }
            // TODO: Block until data available
        }

        let elem = self.queue.pop_front()
            .ok_or(NcStatus::Error)?;

        // Convert data type if needed
        let data = if self.data_type == FifoDataType::Fp32 &&
                     self.tensor_desc.data_type == FifoDataType::Fp16 {
            // Convert FP16 to FP32
            let fp16_data: Vec<u16> = elem.data.chunks_exact(2)
                .map(|chunk| u16::from_le_bytes([chunk[0], chunk[1]]))
                .collect();
            let fp32_data = fp16_to_fp32(&fp16_data);
            fp32_data.iter()
                .flat_map(|&x| x.to_le_bytes())
                .collect()
        } else {
            elem.data
        };

        Ok((data, elem.user_param))
    }

    /// Get FIFO option
    pub fn get_option(&self, option: FifoOption) -> Result<FifoOptionValue> {
        match option {
            FifoOption::Type => {
                Ok(FifoOptionValue::Int(self.fifo_type as i32))
            }
            FifoOption::DataType => {
                Ok(FifoOptionValue::Int(self.data_type as i32))
            }
            FifoOption::DontBlock => {
                Ok(FifoOptionValue::Int(self.dont_block as i32))
            }
            FifoOption::Capacity => {
                Ok(FifoOptionValue::Int(self.capacity as i32))
            }
            FifoOption::ReadFillLevel => {
                if self.fifo_type == FifoType::HostRo {
                    Ok(FifoOptionValue::Int(self.queue.len() as i32))
                } else {
                    Err(NcStatus::InvalidParameters)
                }
            }
            FifoOption::WriteFillLevel => {
                if self.fifo_type == FifoType::HostWo {
                    Ok(FifoOptionValue::Int(self.queue.len() as i32))
                } else {
                    Err(NcStatus::InvalidParameters)
                }
            }
            FifoOption::State => {
                Ok(FifoOptionValue::Int(self.state as i32))
            }
            FifoOption::Name => {
                Ok(FifoOptionValue::String(self.name.clone()))
            }
            FifoOption::ElementDataSize => {
                Ok(FifoOptionValue::Int(self.tensor_desc.total_size as i32))
            }
        }
    }

    /// Set FIFO option
    pub fn set_option(&mut self, option: FifoOption, value: FifoOptionValue) -> Result<()> {
        if self.state != FifoState::Created {
            return Err(NcStatus::InvalidHandle);
        }

        match option {
            FifoOption::Type => {
                if let FifoOptionValue::Int(val) = value {
                    self.fifo_type = match val {
                        0 => FifoType::HostRo,
                        1 => FifoType::HostWo,
                        _ => return Err(NcStatus::InvalidParameters),
                    };
                    Ok(())
                } else {
                    Err(NcStatus::InvalidParameters)
                }
            }
            FifoOption::DataType => {
                if let FifoOptionValue::Int(val) = value {
                    self.data_type = match val {
                        0 => FifoDataType::Fp16,
                        1 => FifoDataType::Fp32,
                        _ => return Err(NcStatus::InvalidParameters),
                    };
                    Ok(())
                } else {
                    Err(NcStatus::InvalidParameters)
                }
            }
            FifoOption::DontBlock => {
                if let FifoOptionValue::Int(val) = value {
                    self.dont_block = val != 0;
                    Ok(())
                } else {
                    Err(NcStatus::InvalidParameters)
                }
            }
            _ => Err(NcStatus::InvalidParameters),  // Read-only options
        }
    }
}

#[derive(Debug, Clone, Copy)]
pub enum FifoOption {
    Type,
    DataType,
    DontBlock,
    Capacity,
    ReadFillLevel,
    WriteFillLevel,
    State,
    Name,
    ElementDataSize,
}

#[derive(Debug, Clone)]
pub enum FifoOptionValue {
    Int(i32),
    String(String),
}
```

#### 5. Tensor Descriptors (src/tensor.rs)
```rust
use super::fifo::FifoDataType;

#[derive(Debug, Clone, Copy, PartialEq)]
#[repr(C)]
pub struct TensorDescriptor {
    pub n: u32,           // Batch count (currently 1)
    pub c: u32,           // Channels per pixel
    pub w: u32,           // Width in pixels (1 for non-images)
    pub h: u32,           // Height in pixels
    pub total_size: u32,  // Total size in bytes
    pub c_stride: u32,    // Channel stride (bytes)
    pub w_stride: u32,    // Horizontal stride (bytes)
    pub h_stride: u32,    // Vertical stride (bytes)
    pub data_type: FifoDataType,
}

impl Default for TensorDescriptor {
    fn default() -> Self {
        Self {
            n: 1,
            c: 1,
            w: 1,
            h: 1,
            total_size: 4,
            c_stride: 4,
            w_stride: 4,
            h_stride: 4,
            data_type: FifoDataType::Fp32,
        }
    }
}

impl TensorDescriptor {
    pub fn new(n: u32, c: u32, h: u32, w: u32, data_type: FifoDataType) -> Self {
        let elem_size = match data_type {
            FifoDataType::Fp16 => 2,
            FifoDataType::Fp32 => 4,
        };

        let w_stride = elem_size;
        let h_stride = w * w_stride;
        let c_stride = h * h_stride;
        let total_size = n * c * h * w * elem_size;

        Self {
            n,
            c,
            w,
            h,
            total_size,
            c_stride,
            w_stride,
            h_stride,
            data_type,
        }
    }

    pub fn validate_buffer_size(&self, buffer: &[u8]) -> bool {
        buffer.len() == self.total_size as usize
    }
}
```

#### 6. FP32 ↔ FP16 Conversion (src/conversion.rs)
```rust
use half::f16;

pub fn fp32_to_fp16(fp32_data: &[f32]) -> Vec<u16> {
    fp32_data.iter()
        .map(|&x| f16::from_f32(x).to_bits())
        .collect()
}

pub fn fp16_to_fp32(fp16_data: &[u16]) -> Vec<f32> {
    fp16_data.iter()
        .map(|&x| f16::from_bits(x).to_f32())
        .collect()
}
```

#### 7. C FFI Layer (src/ffi.rs)
```rust
use std::ffi::{c_char, c_int, c_uint, c_void};
use std::ptr;
use std::sync::{Arc, Mutex};
use super::*;

// Opaque handles for C API
#[repr(C)]
pub struct ncDeviceHandle_t {
    _private: [u8; 0],
}

#[repr(C)]
pub struct ncGraphHandle_t {
    _private: [u8; 0],
}

#[repr(C)]
pub struct ncFifoHandle_t {
    _private: [u8; 0],
}

// Convert Rust handle to opaque C pointer
fn device_to_c(handle: Arc<Mutex<DeviceHandle>>) -> *mut ncDeviceHandle_t {
    Box::into_raw(Box::new(handle)) as *mut ncDeviceHandle_t
}

fn c_to_device(ptr: *mut ncDeviceHandle_t) -> Arc<Mutex<DeviceHandle>> {
    unsafe {
        let boxed = Box::from_raw(ptr as *mut Arc<Mutex<DeviceHandle>>);
        let handle = (*boxed).clone();
        Box::leak(boxed);  // Don't drop
        handle
    }
}

#[no_mangle]
pub extern "C" fn ncDeviceCreate(
    index: c_int,
    device_handle: *mut *mut ncDeviceHandle_t,
) -> c_int {
    if device_handle.is_null() {
        return NcStatus::InvalidParameters as c_int;
    }

    match DeviceHandle::create(index) {
        Ok(handle) => {
            let arc_handle = Arc::new(Mutex::new(handle));
            unsafe {
                *device_handle = device_to_c(arc_handle);
            }
            NcStatus::Ok as c_int
        }
        Err(e) => e as c_int,
    }
}

#[no_mangle]
pub extern "C" fn ncDeviceOpen(device_handle: *mut ncDeviceHandle_t) -> c_int {
    if device_handle.is_null() {
        return NcStatus::InvalidHandle as c_int;
    }

    let handle = c_to_device(device_handle);
    match handle.lock().unwrap().open() {
        Ok(()) => NcStatus::Ok as c_int,
        Err(e) => e as c_int,
    }
}

#[no_mangle]
pub extern "C" fn ncDeviceClose(device_handle: *mut ncDeviceHandle_t) -> c_int {
    if device_handle.is_null() {
        return NcStatus::InvalidHandle as c_int;
    }

    let handle = c_to_device(device_handle);
    match handle.lock().unwrap().close() {
        Ok(()) => NcStatus::Ok as c_int,
        Err(e) => e as c_int,
    }
}

#[no_mangle]
pub extern "C" fn ncDeviceDestroy(device_handle: *mut *mut ncDeviceHandle_t) -> c_int {
    if device_handle.is_null() || unsafe { (*device_handle).is_null() } {
        return NcStatus::InvalidHandle as c_int;
    }

    unsafe {
        let ptr = *device_handle as *mut Arc<Mutex<DeviceHandle>>;
        let _boxed = Box::from_raw(ptr);  // Drop the Arc
        *device_handle = ptr::null_mut();
    }

    NcStatus::Ok as c_int
}

// ... implement rest of C FFI functions

#[no_mangle]
pub extern "C" fn ncGraphCreate(
    name: *const c_char,
    graph_handle: *mut *mut ncGraphHandle_t,
) -> c_int {
    // ... implementation
    NcStatus::Ok as c_int
}

#[no_mangle]
pub extern "C" fn ncFifoCreate(
    name: *const c_char,
    fifo_type: c_int,
    fifo_handle: *mut *mut ncFifoHandle_t,
) -> c_int {
    // ... implementation
    NcStatus::Ok as c_int
}

// ... etc
```

#### 8. Cargo.toml
```toml
[package]
name = "movidius-ncapi"
version = "2.0.0"
edition = "2021"
authors = ["Your Name"]
license = "MIT OR Apache-2.0"
description = "Rust implementation of Movidius NCAPI v2"
repository = "https://github.com/yourusername/movidius-ncapi-rs"

[lib]
name = "movidius_ncapi"
crate-type = ["rlib", "cdylib", "staticlib"]

[dependencies]
thiserror = "1.0"
io-uring = "0.6"
half = "2.3"  # FP16 support
libc = "0.2"
zerocopy = "0.7"

[dev-dependencies]
criterion = "0.5"
tempfile = "3.8"

[profile.release]
lto = true
codegen-units = 1
opt-level = 3

[[example]]
name = "basic_inference"
path = "examples/basic_inference.rs"

[[bench]]
name = "inference_benchmark"
harness = false
```

---

## Tier 2: Hardware Abstraction Layer (Rust)

### Project Structure
```
movidius-hal-rs/
├── Cargo.toml
├── src/
│   ├── lib.rs
│   ├── uring.rs     # io_uring wrapper
│   ├── dma.rs       # DMA management
│   ├── usb.rs       # USB protocol
│   └── protocol.rs  # Device protocol
```

### Key Implementation
```rust
// src/uring.rs
use io_uring::{opcode, types, IoUring};
use std::os::unix::io::RawFd;

pub struct UringSubmitter {
    ring: IoUring,
}

impl UringSubmitter {
    pub fn new(entries: usize) -> std::io::Result<Self> {
        let ring = IoUring::new(entries as u32)?;
        Ok(Self { ring })
    }

    pub fn submit_inference(
        &mut self,
        fd: RawFd,
        request: &InferenceRequest,
    ) -> std::io::Result<u64> {
        // Prepare io_uring submission
        let cmd = opcode::UringCmd80::new(types::Fd(fd), 0)
            .build()
            .user_data(request.user_data);

        unsafe {
            self.ring.submission().push(&cmd)?;
        }

        self.ring.submit()?;

        // Return token for tracking
        Ok(request.user_data)
    }

    pub fn poll_completion(&mut self) -> std::io::Result<Option<(u64, i32)>> {
        if let Some(cqe) = self.ring.completion().next() {
            let user_data = cqe.user_data();
            let result = cqe.result();
            Ok(Some((user_data, result)))
        } else {
            Ok(None)
        }
    }
}

#[repr(C)]
pub struct InferenceRequest {
    pub hdr: CommandHeader,
    pub num_input_segs: u32,
    pub num_output_segs: u32,
    pub input_segs: [SgSegment; 16],
    pub output_segs: [SgSegment; 16],
    pub user_data: u64,
}

#[repr(C)]
pub struct CommandHeader {
    pub version: u16,
    pub op: u16,
    pub len: u32,
}

#[repr(C)]
pub struct SgSegment {
    pub offset: u32,
    pub len: u32,
}
```

---

## Tier 3: Rust Kernel Driver (Optional)

### Why Rust Kernel Driver?

The existing C driver (`movidius_x_vpu.c`) works well, but a Rust version offers:
- Memory safety guarantees
- Better error handling
- Modern language features
- Easier maintenance

### Rust-for-Linux Integration
```rust
// linux-rust/drivers/gpu/movidius/movidius_rs.rs

use kernel::prelude::*;
use kernel::{
    device, file, io_uring, module_usb_driver, platform, usb,
};

module! {
    type: MovidiusDriver,
    name: "movidius_x_vpu_rs",
    author: "Your Name",
    description: "Rust driver for Movidius Myriad X VPU",
    license: "GPL",
}

struct MovidiusDriver {
    _dev: device::Device,
}

impl usb::Driver for MovidiusDriver {
    const ID_TABLE: &'static [usb::DeviceId] = &[
        usb::DeviceId::new(0x03e7, 0x2485),  // NCS2
    ];

    fn probe(
        dev: &mut usb::Device,
        _id: &usb::DeviceId,
    ) -> Result<Pin<Box<Self>>> {
        pr_info!("Movidius device probed: {:?}\n", dev);

        // ... driver initialization

        Ok(Box::pin_init(MovidiusDriver {
            _dev: dev.clone(),
        })?)
    }

    fn disconnect(_data: &Self) {
        pr_info!("Movidius device disconnected\n");
    }
}

module_usb_driver! {
    type: MovidiusDriver,
    name: "movidius_x_vpu_rs",
    license: "GPL",
}
```

**Note:** Rust-for-Linux is still experimental. For production, keeping the C driver is recommended while the userspace layer can be pure Rust.

---

## Python Bindings (Optional, via PyO3)

### Only if Python API is absolutely necessary

```rust
// python/src/lib.rs
use pyo3::prelude::*;
use movidius_ncapi::*;

#[pyclass]
struct Device {
    handle: Arc<Mutex<DeviceHandle>>,
}

#[pymethods]
impl Device {
    #[new]
    fn new(index: i32) -> PyResult<Self> {
        let handle = DeviceHandle::create(index)
            .map_err(|e| PyErr::new::<pyo3::exceptions::PyRuntimeError, _>(
                format!("{:?}", e)
            ))?;

        Ok(Self {
            handle: Arc::new(Mutex::new(handle)),
        })
    }

    fn open(&self) -> PyResult<()> {
        self.handle.lock().unwrap().open()
            .map_err(|e| PyErr::new::<pyo3::exceptions::PyRuntimeError, _>(
                format!("{:?}", e)
            ))
    }

    fn close(&self) -> PyResult<()> {
        self.handle.lock().unwrap().close()
            .map_err(|e| PyErr::new::<pyo3::exceptions::PyRuntimeError, _>(
                format!("{:?}", e)
            ))
    }
}

#[pymodule]
fn mvncapi(_py: Python, m: &PyModule) -> PyResult<()> {
    m.add_class::<Device>()?;
    // ... add other classes
    Ok(())
}
```

---

## Implementation Phases

### Phase 1: Pure Rust NCAPI Library (6-8 weeks)
- [ ] Core abstractions (Device, Graph, FIFO)
- [ ] State machines
- [ ] Tensor descriptors
- [ ] Error handling
- [ ] Basic graph file parsing
- [ ] FP32/FP16 conversion
- [ ] Unit tests

### Phase 2: HAL Integration (2-3 weeks)
- [ ] io_uring wrapper
- [ ] DMA management
- [ ] USB protocol handling
- [ ] Integration with existing C driver

### Phase 3: C FFI Layer (2-3 weeks)
- [ ] Complete C API bindings
- [ ] C example applications
- [ ] C compatibility tests

### Phase 4: Advanced Features (3-4 weeks)
- [ ] Async/await Rust API
- [ ] Multi-threading support
- [ ] Performance optimizations
- [ ] Advanced error recovery

### Phase 5: Testing & Documentation (2-3 weeks)
- [ ] Comprehensive test suite
- [ ] Benchmarks vs official SDK
- [ ] API documentation
- [ ] Usage examples
- [ ] Migration guide

### Phase 6 (Optional): Rust Kernel Driver (4-6 weeks)
- [ ] Port C driver to Rust
- [ ] Rust-for-Linux integration
- [ ] Kernel testing

### Phase 7 (Optional): Python Bindings (1-2 weeks)
- [ ] PyO3 bindings
- [ ] Python API tests
- [ ] Python examples

**Total Estimated Effort:** 15-21 weeks (Rust only), 20-29 weeks (with Rust kernel driver), 21-31 weeks (with Python)

---

## Build System

### Cross-Language Build with Cargo
```toml
# workspace Cargo.toml
[workspace]
members = [
    "movidius-ncapi",
    "movidius-hal",
    "python",  # optional
]

[workspace.package]
edition = "2021"
license = "MIT OR Apache-2.0"

[workspace.dependencies]
thiserror = "1.0"
io-uring = "0.6"
half = "2.3"
libc = "0.2"
```

### C Header Generation
```bash
# Generate C headers from Rust
cbindgen --config cbindgen.toml --crate movidius-ncapi --output movidius_ncapi.h
```

---

## Advantages of Rust Implementation

1. **Memory Safety:** No buffer overflows, use-after-free, or data races
2. **Type Safety:** Strong type system catches errors at compile time
3. **Zero-Cost Abstractions:** Same performance as C
4. **Modern Tooling:** Cargo, rustfmt, clippy, rust-analyzer
5. **Better Error Handling:** Result types force explicit error handling
6. **Fearless Concurrency:** Thread safety guaranteed by compiler
7. **Easy Testing:** Built-in test framework
8. **Documentation:** rustdoc generates beautiful docs
9. **Package Management:** Cargo handles dependencies
10. **Cross-Platform:** Write once, compile everywhere

---

## Testing Strategy

### Unit Tests (Rust)
```rust
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_device_create() {
        let device = DeviceHandle::create(0);
        assert!(device.is_ok());
    }

    #[test]
    fn test_graph_state_machine() {
        let mut graph = GraphHandle::create("test").unwrap();
        assert_eq!(graph.state, GraphState::Created);

        // Test invalid state transition
        let result = graph.queue_inference(&[], &[]);
        assert_eq!(result, Err(NcStatus::NotAllocated));
    }

    #[test]
    fn test_fp16_conversion() {
        let fp32_data = vec![1.0f32, 2.0, 3.0];
        let fp16_data = fp32_to_fp16(&fp32_data);
        let converted_back = fp16_to_fp32(&fp16_data);

        for (a, b) in fp32_data.iter().zip(converted_back.iter()) {
            assert!((a - b).abs() < 0.001);
        }
    }
}
```

### Integration Tests (C FFI)
```c
// tests/c_compat_test.c
#include "movidius_ncapi.h"
#include <assert.h>

int main() {
    ncDeviceHandle_t* device;
    ncStatus_t status;

    // Test device creation
    status = ncDeviceCreate(0, &device);
    assert(status == NC_OK);

    // Test device open
    status = ncDeviceOpen(device);
    assert(status == NC_OK);

    // Test device close
    status = ncDeviceClose(device);
    assert(status == NC_OK);

    // Test device destroy
    status = ncDeviceDestroy(&device);
    assert(status == NC_OK);
    assert(device == NULL);

    return 0;
}
```

### Benchmarks (Criterion)
```rust
use criterion::{black_box, criterion_group, criterion_main, Criterion};

fn benchmark_inference(c: &mut Criterion) {
    c.bench_function("single_inference", |b| {
        let device = setup_device();
        let graph = setup_graph(&device);
        let input = vec![0.0f32; 224 * 224 * 3];

        b.iter(|| {
            graph.queue_inference_with_fifo_elem(
                black_box(&input),
                None,
            )
        });
    });
}

criterion_group!(benches, benchmark_inference);
criterion_main!(benches);
```

---

## Conclusion

This Rust-first implementation strategy provides:

1. **✅ NCAPI v2 Compatibility** - Full API implementation in Rust
2. **✅ Memory Safety** - Guaranteed by Rust's type system
3. **✅ High Performance** - Zero-cost abstractions, same as C
4. **✅ C Compatibility** - FFI layer for existing C applications
5. **✅ Modern Tooling** - Cargo, rustfmt, clippy, rust-analyzer
6. **✅ Optional Python** - Only if absolutely necessary via PyO3
7. **✅ Future-Proof** - Can port kernel driver to Rust when Rust-for-Linux matures

The pure Rust userspace library approach is **recommended** as the first phase, keeping the existing C kernel driver. This provides immediate NCAPI compatibility while allowing gradual migration to Rust throughout the stack.

**Next Steps:**
1. Review and approve architecture
2. Set up Rust project structure
3. Begin Phase 1 implementation
4. Iterate based on testing feedback
