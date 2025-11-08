//! Graph handle and management

use crate::device::Device;
use crate::error::{Error, Result};
use crate::fifo::{Fifo, FifoType};
use crate::status::Status;
use crate::tensor::TensorDescriptor;
use parking_lot::RwLock;
use std::sync::Arc;

/// Type alias for a pair of FIFOs (input and output)
type FifoPair = (Arc<RwLock<Fifo>>, Arc<RwLock<Fifo>>);

/// Represents the current state of a neural network graph.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum GraphState {
    /// Graph has been created but not yet allocated to a device.
    Created = 0,
    /// Graph has been allocated to a device and is ready for inference.
    Allocated = 1,
    /// Graph is waiting for input/output buffers before execution.
    WaitingForBuffers = 2,
    /// Graph is currently executing an inference operation.
    Running = 3,
}

/// Configuration and query options for a neural network graph.
#[derive(Debug, Clone, Copy)]
pub enum GraphOption {
    /// Query the current state of the graph.
    State,
    /// Query the time taken for the last inference operation.
    TimeTaken,
    /// Query the number of input tensors required by the graph.
    InputCount,
    /// Query the number of output tensors produced by the graph.
    OutputCount,
    /// Query descriptors for all input tensors.
    InputTensorDescriptors,
    /// Query descriptors for all output tensors.
    OutputTensorDescriptors,
    /// Query the number of executor instances for the graph.
    ExecutorsNum,
    /// Query the name of the graph.
    Name,
}

/// A neural network graph that can be loaded onto a Movidius device for inference.
///
/// Manages the lifecycle of a neural network model from creation through allocation
/// to execution on the device.
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

    /// Creates a new graph with the specified name.
    ///
    /// # Arguments
    ///
    /// * `name` - A unique identifier for the graph (must not be empty and less than MAX_NAME_SIZE)
    ///
    /// # Errors
    ///
    /// Returns an error if the name is empty or exceeds the maximum name size.
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

    /// Allocates the graph to a device using the provided graph file buffer.
    ///
    /// # Arguments
    ///
    /// * `device` - The device to allocate the graph on
    /// * `graph_buffer` - Binary buffer containing the compiled neural network graph
    ///
    /// # Errors
    ///
    /// Returns an error if:
    /// - The graph is not in the Created state
    /// - The graph buffer size is invalid (too small or too large)
    pub fn allocate(&mut self, device: Arc<RwLock<Device>>, graph_buffer: &[u8]) -> Result<()> {
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

    /// Allocates the graph to a device and creates associated input/output FIFOs.
    ///
    /// This is a convenience method that combines graph allocation with FIFO creation.
    ///
    /// # Arguments
    ///
    /// * `device` - The device to allocate the graph on
    /// * `graph_buffer` - Binary buffer containing the compiled neural network graph
    ///
    /// # Returns
    ///
    /// A tuple containing (input_fifo, output_fifo) for data transfer.
    ///
    /// # Errors
    ///
    /// Returns an error if allocation or FIFO creation fails.
    pub fn allocate_with_fifos(
        &mut self,
        device: Arc<RwLock<Device>>,
        graph_buffer: &[u8],
    ) -> Result<FifoPair> {
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

    /// Queues an inference operation using FIFO buffers for input/output data.
    ///
    /// # Arguments
    ///
    /// * `input_fifo` - FIFO for writing input tensor data
    /// * `_output_fifo` - FIFO for reading output tensor data (currently unused)
    /// * `input_tensor` - Input data to process
    /// * `user_param` - Optional user-defined parameter associated with this inference
    ///
    /// # Errors
    ///
    /// Returns an error if the graph is not in an appropriate state (Allocated or WaitingForBuffers).
    pub fn queue_inference_with_fifo_elem(
        &mut self,
        input_fifo: &Arc<RwLock<Fifo>>,
        _output_fifo: &Arc<RwLock<Fifo>>,
        input_tensor: &[u8],
        user_param: Option<usize>,
    ) -> Result<()> {
        if self.state != GraphState::Allocated && self.state != GraphState::WaitingForBuffers {
            return Err(Error::Status(Status::NotAllocated));
        }

        input_fifo.write().write_elem(input_tensor, user_param)?;

        self.state = GraphState::Running;
        // TODO: Actually queue to device
        self.state = GraphState::WaitingForBuffers;

        Ok(())
    }

    /// Returns the current state of the graph.
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
