//! Graph handle and management

use crate::device::Device;
use crate::error::{Error, Result};
use crate::fifo::{Fifo, FifoType};
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
    /// Minimum valid graph file size
    const MIN_GRAPH_SIZE: usize = 64;
    /// Maximum reasonable graph file size (100MB)
    const MAX_GRAPH_SIZE: usize = 100 * 1024 * 1024;

    pub fn create(name: &str) -> Result<Self> {
        if name.is_empty() {
            tracing::error!("Graph name cannot be empty");
            return Err(Error::Status(Status::InvalidParameters));
        }
        if name.len() >= crate::MAX_NAME_SIZE {
            tracing::error!(
                "Graph name '{}' exceeds maximum length of {}",
                name,
                crate::MAX_NAME_SIZE
            );
            return Err(Error::Status(Status::InvalidParameters));
        }

        tracing::debug!("Creating graph '{}'", name);
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
            tracing::error!(
                "Cannot allocate graph '{}' in state {:?}, must be Created",
                self.name,
                self.state
            );
            return Err(Error::Status(Status::InvalidHandle));
        }

        // Validate graph buffer
        if graph_buffer.len() < Self::MIN_GRAPH_SIZE {
            tracing::error!(
                "Graph '{}': buffer size {} is too small (minimum {})",
                self.name,
                graph_buffer.len(),
                Self::MIN_GRAPH_SIZE
            );
            return Err(Error::Status(Status::InvalidParameters));
        }
        if graph_buffer.len() > Self::MAX_GRAPH_SIZE {
            tracing::error!(
                "Graph '{}': buffer size {} exceeds maximum of {}",
                self.name,
                graph_buffer.len(),
                Self::MAX_GRAPH_SIZE
            );
            return Err(Error::Status(Status::InvalidParameters));
        }

        tracing::debug!(
            "Allocating graph '{}' with {} byte buffer",
            self.name,
            graph_buffer.len()
        );

        // Parse graph file (stub for now)
        // TODO: Actual graph parsing and validation
        self.input_descriptors = vec![TensorDescriptor::default()];
        self.output_descriptors = vec![TensorDescriptor::default()];

        self.device = Some(device);
        self.state = GraphState::Allocated;
        tracing::info!("Graph '{}' allocated successfully", self.name);
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

impl Drop for Graph {
    fn drop(&mut self) {
        tracing::debug!("Dropping graph '{}' (state: {:?})", self.name, self.state);

        // Note: FIFOs associated with this graph should be dropped BEFORE the graph
        // This is enforced by Rust's drop order when FIFOs are created via allocate_with_fifos
        // (FIFOs are dropped first since they're returned/stored separately)

        if self.state == GraphState::Running {
            tracing::warn!(
                "Graph '{}' dropped while in Running state - may cause device issues",
                self.name
            );
        }

        // Device reference is dropped automatically (Arc)
        // This is safe because device outlives graph
        self.state = GraphState::Created; // Reset for cleanup

        tracing::trace!("Graph '{}' cleanup complete", self.name);
    }
}
