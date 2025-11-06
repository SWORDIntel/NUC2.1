//! Graph handle and management

use crate::device::Device;
use crate::error::{Error, Result};
use crate::fifo::{Fifo, FifoDataType, FifoType};
use crate::status::Status;
use crate::tensor::TensorDescriptor;
use parking_lot::RwLock;
use std::sync::Arc;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum GraphState {
    Created = 0,
    Allocated = 1,
    WaitingForBuffers = 2,
    Running = 3,
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

pub struct Graph {
    name: String,
    state: GraphState,
    device: Option<Arc<RwLock<Device>>>,
    input_descriptors: Vec<TensorDescriptor>,
    output_descriptors: Vec<TensorDescriptor>,
}

impl Graph {
    pub fn create(name: &str) -> Result<Self> {
        if name.len() >= crate::MAX_NAME_SIZE {
            return Err(Error::Status(Status::InvalidParameters));
        }

        Ok(Self {
            name: name.to_string(),
            state: GraphState::Created,
            device: None,
            input_descriptors: Vec::new(),
            output_descriptors: Vec::new(),
        })
    }

    pub fn allocate(
        &mut self,
        device: Arc<RwLock<Device>>,
        graph_buffer: &[u8],
    ) -> Result<()> {
        if self.state != GraphState::Created {
            return Err(Error::Status(Status::InvalidHandle));
        }

        // Parse graph file (stub for now)
        self.input_descriptors = vec![TensorDescriptor::default()];
        self.output_descriptors = vec![TensorDescriptor::default()];

        self.device = Some(device);
        self.state = GraphState::Allocated;
        Ok(())
    }

    pub fn allocate_with_fifos(
        &mut self,
        device: Arc<RwLock<Device>>,
        graph_buffer: &[u8],
    ) -> Result<(Arc<RwLock<Fifo>>, Arc<RwLock<Fifo>>)> {
        self.allocate(device.clone(), graph_buffer)?;

        let mut input_fifo = Fifo::create("input", FifoType::HostWo)?;
        input_fifo.allocate(device.clone(), &self.input_descriptors[0], 2)?;

        let mut output_fifo = Fifo::create("output", FifoType::HostRo)?;
        output_fifo.allocate(device.clone(), &self.output_descriptors[0], 2)?;

        Ok((
            Arc::new(RwLock::new(input_fifo)),
            Arc::new(RwLock::new(output_fifo)),
        ))
    }

    pub fn queue_inference_with_fifo_elem(
        &mut self,
        input_fifo: &Arc<RwLock<Fifo>>,
        _output_fifo: &Arc<RwLock<Fifo>>,
        input_tensor: &[u8],
        user_param: Option<usize>,
    ) -> Result<()> {
        if self.state != GraphState::Allocated
            && self.state != GraphState::WaitingForBuffers
        {
            return Err(Error::Status(Status::NotAllocated));
        }

        input_fifo.write().write_elem(input_tensor, user_param)?;

        self.state = GraphState::Running;
        // TODO: Actually queue to device
        self.state = GraphState::WaitingForBuffers;

        Ok(())
    }

    pub fn state(&self) -> GraphState {
        self.state
    }
}
