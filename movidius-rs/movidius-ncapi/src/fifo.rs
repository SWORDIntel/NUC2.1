//! High-performance lock-free FIFO queue implementation

use crate::device::Device;
use crate::error::{Error, Result};
use crate::status::Status;
use crate::tensor::TensorDescriptor;
use bytemuck::{Pod, Zeroable};
use crossbeam::queue::ArrayQueue;
use parking_lot::RwLock;
use std::sync::Arc;

/// FIFO data types (Pod-safe wrapper)
#[derive(Debug, Clone, Copy, PartialEq, Eq, Pod, Zeroable)]
#[repr(transparent)]
pub struct FifoDataType(i32);

impl FifoDataType {
    /// 16-bit floating point
    pub const FP16: Self = Self(0);
    /// 32-bit floating point (default)
    pub const FP32: Self = Self(1);

    /// Size in bytes
    #[inline]
    pub const fn size_bytes(self) -> u32 {
        match self.0 {
            0 => 2, // FP16
            1 => 4, // FP32
            _ => 4,
        }
    }

    /// Get the inner i32 value
    #[inline]
    pub const fn as_i32(self) -> i32 {
        self.0
    }
}

impl Default for FifoDataType {
    fn default() -> Self {
        Self::FP32
    }
}

/// FIFO access types
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum FifoType {
    /// Host read-only (for outputs)
    HostRo = 0,
    /// Host write-only (for inputs)
    HostWo = 1,
}

/// FIFO states
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum FifoState {
    /// Created but not allocated
    Created = 0,
    /// Allocated to device
    Allocated = 1,
}

/// FIFO options
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FifoOption {
    /// FIFO type (read/write)
    Type,
    /// Data type (read/write)
    DataType,
    /// Non-blocking mode (read/write)
    DontBlock,
    /// Capacity (read-only)
    Capacity,
    /// Read fill level (read-only)
    ReadFillLevel,
    /// Write fill level (read-only)
    WriteFillLevel,
    /// State (read-only)
    State,
    /// Name (read-only)
    Name,
    /// Element data size (read-only)
    ElementDataSize,
}

/// FIFO element with user data
struct FifoElement {
    data: Vec<u8>,
    user_param: Option<usize>,
}

/// High-performance FIFO handle
pub struct Fifo {
    name: String,
    state: FifoState,
    fifo_type: FifoType,
    data_type: FifoDataType,
    device: Option<Arc<RwLock<Device>>>,
    tensor_desc: TensorDescriptor,
    capacity: usize,
    /// Lock-free queue for maximum performance
    queue: Arc<ArrayQueue<FifoElement>>,
    dont_block: bool,
}

impl Fifo {
    /// Create a new FIFO
    pub fn create(name: &str, fifo_type: FifoType) -> Result<Self> {
        if name.len() >= crate::MAX_NAME_SIZE {
            return Err(Error::Status(Status::InvalidParameters));
        }

        Ok(Self {
            name: name.to_string(),
            state: FifoState::Created,
            fifo_type,
            data_type: FifoDataType::default(),
            device: None,
            tensor_desc: TensorDescriptor::default(),
            capacity: 0,
            queue: Arc::new(ArrayQueue::new(2)), // Will be reallocated
            dont_block: false,
        })
    }

    /// Allocate FIFO to device
    pub fn allocate(
        &mut self,
        device: Arc<RwLock<Device>>,
        tensor_desc: &TensorDescriptor,
        num_elem: usize,
    ) -> Result<()> {
        if self.state != FifoState::Created {
            tracing::error!(
                "Cannot allocate FIFO '{}' in state {:?}, must be Created",
                self.name,
                self.state
            );
            return Err(Error::Status(Status::InvalidHandle));
        }

        // Validate num_elem
        if num_elem == 0 {
            tracing::error!("FIFO '{}': num_elem must be > 0", self.name);
            return Err(Error::Status(Status::InvalidParameters));
        }
        if num_elem > 1000000 {
            // Reasonable upper bound
            tracing::error!(
                "FIFO '{}': num_elem {} exceeds maximum of 1000000",
                self.name,
                num_elem
            );
            return Err(Error::Status(Status::InvalidParameters));
        }

        // Validate tensor descriptor
        if !tensor_desc.is_contiguous() {
            tracing::error!("FIFO '{}': tensor descriptor must be contiguous", self.name);
            return Err(Error::Status(Status::InvalidParameters));
        }

        tracing::debug!(
            "Allocating FIFO '{}' with {} elements of {} bytes each",
            self.name,
            num_elem,
            tensor_desc.total_size
        );

        self.device = Some(device);
        self.tensor_desc = *tensor_desc;
        self.capacity = num_elem;
        self.queue = Arc::new(ArrayQueue::new(num_elem));
        self.state = FifoState::Allocated;

        tracing::info!("FIFO '{}' allocated successfully", self.name);
        Ok(())
    }

    /// Write element to FIFO (zero-copy when possible)
    #[inline]
    pub fn write_elem(&self, input_tensor: &[u8], user_param: Option<usize>) -> Result<()> {
        if self.state != FifoState::Allocated {
            tracing::error!("FIFO '{}' not allocated for write", self.name);
            return Err(Error::Status(Status::NotAllocated));
        }

        if self.fifo_type != FifoType::HostWo {
            tracing::error!(
                "FIFO '{}' is not write-only (type: {:?})",
                self.name,
                self.fifo_type
            );
            return Err(Error::Status(Status::Unauthorized));
        }

        // Validate size
        let expected_size = self.tensor_desc.total_size as usize;
        if input_tensor.len() != expected_size {
            tracing::error!(
                "FIFO '{}': invalid tensor size {} (expected {})",
                self.name,
                input_tensor.len(),
                expected_size
            );
            return Err(Error::Status(Status::InvalidDataLength));
        }

        tracing::trace!(
            "Writing {} bytes to FIFO '{}'",
            input_tensor.len(),
            self.name
        );

        // Convert data type if needed
        let data = self.convert_to_device_format(input_tensor)?;

        let element = FifoElement { data, user_param };

        // Lock-free push
        if self.queue.push(element).is_err() {
            if self.dont_block {
                return Err(Error::Status(Status::Busy));
            }
            // Busy-wait for space (will be replaced with proper blocking)
            while self
                .queue
                .push(FifoElement {
                    data: self.convert_to_device_format(input_tensor)?,
                    user_param,
                })
                .is_err()
            {
                std::hint::spin_loop();
            }
        }

        Ok(())
    }

    /// Read element from FIFO (zero-copy when possible)
    #[inline]
    pub fn read_elem(&self) -> Result<(Vec<u8>, Option<usize>)> {
        if self.state != FifoState::Allocated {
            tracing::error!("FIFO '{}' not allocated for read", self.name);
            return Err(Error::Status(Status::NotAllocated));
        }

        if self.fifo_type != FifoType::HostRo {
            tracing::error!(
                "FIFO '{}' is not read-only (type: {:?})",
                self.name,
                self.fifo_type
            );
            return Err(Error::Status(Status::Unauthorized));
        }

        tracing::trace!("Reading from FIFO '{}'", self.name);

        // Lock-free pop
        let element = if let Some(elem) = self.queue.pop() {
            elem
        } else if self.dont_block {
            return Err(Error::Status(Status::Busy));
        } else {
            // Busy-wait for data (will be replaced with proper blocking)
            loop {
                if let Some(elem) = self.queue.pop() {
                    break elem;
                }
                std::hint::spin_loop();
            }
        };

        // Convert from device format if needed
        let data = self.convert_from_device_format(&element.data)?;

        Ok((data, element.user_param))
    }

    /// Get FIFO option
    pub fn get_option(&self, option: FifoOption) -> Result<Vec<u8>> {
        match option {
            FifoOption::Type => Ok((self.fifo_type as i32).to_le_bytes().to_vec()),
            FifoOption::DataType => Ok(self.data_type.as_i32().to_le_bytes().to_vec()),
            FifoOption::DontBlock => Ok((self.dont_block as i32).to_le_bytes().to_vec()),
            FifoOption::Capacity => Ok((self.capacity as i32).to_le_bytes().to_vec()),
            FifoOption::ReadFillLevel | FifoOption::WriteFillLevel => {
                Ok((self.queue.len() as i32).to_le_bytes().to_vec())
            }
            FifoOption::State => Ok((self.state as i32).to_le_bytes().to_vec()),
            FifoOption::Name => Ok(self.name.as_bytes().to_vec()),
            FifoOption::ElementDataSize => Ok(self.tensor_desc.total_size.to_le_bytes().to_vec()),
        }
    }

    /// Set FIFO option (only before allocation)
    pub fn set_option(&mut self, option: FifoOption, value: &[u8]) -> Result<()> {
        if self.state != FifoState::Created {
            return Err(Error::Status(Status::InvalidHandle));
        }

        match option {
            FifoOption::Type => {
                let val = i32::from_le_bytes(
                    value
                        .try_into()
                        .map_err(|_| Error::Status(Status::InvalidParameters))?,
                );
                self.fifo_type = match val {
                    0 => FifoType::HostRo,
                    1 => FifoType::HostWo,
                    _ => return Err(Error::Status(Status::InvalidParameters)),
                };
            }
            FifoOption::DataType => {
                let val = i32::from_le_bytes(
                    value
                        .try_into()
                        .map_err(|_| Error::Status(Status::InvalidParameters))?,
                );
                self.data_type = match val {
                    0 => FifoDataType::FP16,
                    1 => FifoDataType::FP32,
                    _ => return Err(Error::Status(Status::InvalidParameters)),
                };
            }
            FifoOption::DontBlock => {
                let val = i32::from_le_bytes(
                    value
                        .try_into()
                        .map_err(|_| Error::Status(Status::InvalidParameters))?,
                );
                self.dont_block = val != 0;
            }
            _ => return Err(Error::Status(Status::InvalidParameters)),
        }

        Ok(())
    }

    /// Convert data to device format (FP32 -> FP16 if needed)
    #[inline]
    fn convert_to_device_format(&self, data: &[u8]) -> Result<Vec<u8>> {
        if self.data_type == self.tensor_desc.data_type {
            // No conversion needed
            return Ok(data.to_vec());
        }

        // Convert FP32 host -> FP16 device
        if self.tensor_desc.data_type == FifoDataType::FP32 && self.data_type == FifoDataType::FP16
        {
            let fp32_slice = bytemuck::cast_slice::<u8, f32>(data);
            let fp16_vec = crate::conversion::fp32_to_fp16_vec(fp32_slice);
            return Ok(bytemuck::cast_slice(&fp16_vec).to_vec());
        }

        // Other conversions not yet implemented
        Ok(data.to_vec())
    }

    /// Convert data from device format (FP16 -> FP32 if needed)
    #[inline]
    fn convert_from_device_format(&self, data: &[u8]) -> Result<Vec<u8>> {
        if self.data_type == self.tensor_desc.data_type {
            // No conversion needed
            return Ok(data.to_vec());
        }

        // Convert FP16 device -> FP32 host
        if self.tensor_desc.data_type == FifoDataType::FP16 && self.data_type == FifoDataType::FP32
        {
            let fp16_slice = bytemuck::cast_slice::<u8, half::f16>(data);
            let fp32_vec = crate::conversion::fp16_to_fp32_vec(fp16_slice);
            return Ok(bytemuck::cast_slice(&fp32_vec).to_vec());
        }

        // Other conversions not yet implemented
        Ok(data.to_vec())
    }

    /// Get current fill level
    #[inline]
    pub fn len(&self) -> usize {
        self.queue.len()
    }

    /// Check if FIFO is empty
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.queue.is_empty()
    }

    /// Check if FIFO is full
    #[inline]
    pub fn is_full(&self) -> bool {
        self.queue.is_full()
    }
}

impl Drop for Fifo {
    fn drop(&mut self) {
        tracing::debug!("Dropping FIFO '{}' (state: {:?})", self.name, self.state);

        // Drain queue before dropping
        let drained = self.queue.len();
        while self.queue.pop().is_some() {}

        if drained > 0 {
            tracing::warn!(
                "FIFO '{}' dropped with {} unprocessed elements",
                self.name,
                drained
            );
        }

        // Transition to destroyed state
        self.state = FifoState::Created; // Reset for cleanup

        tracing::trace!("FIFO '{}' cleanup complete", self.name);
    }
}

unsafe impl Send for Fifo {}
unsafe impl Sync for Fifo {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_fifo_create() {
        let fifo = Fifo::create("test", FifoType::HostWo).unwrap();
        assert_eq!(fifo.state, FifoState::Created);
        assert_eq!(fifo.fifo_type, FifoType::HostWo);
    }

    #[test]
    fn test_fifo_name_too_long() {
        let long_name = "a".repeat(100);
        assert!(Fifo::create(&long_name, FifoType::HostWo).is_err());
    }
}
