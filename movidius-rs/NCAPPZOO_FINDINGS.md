# NCAPPZOO Analysis: Critical Patterns for Rust NCAPI Implementation

Based on comprehensive analysis of Intel's ncappzoo repository and NCAPI v2 documentation.

## 1. Optimal Threading Configuration

**From benchmark_ncs.py (proven production config):**

```python
threads_per_dev = 3
simultaneous_infer_per_thread = 6
# Total async requests per device = 18
```

**Hardware Rationale:**
- Myriad X has 2 NCEs (Neural Compute Engines)
- Gets better results with 3-4 simultaneous inferences
- 18 total requests keeps hardware fully saturated

**Implementation for Rust:**
```rust
pub const THREADS_PER_DEVICE: usize = 3;
pub const ASYNC_REQUESTS_PER_THREAD: usize = 6;
pub const TOTAL_REQUESTS_PER_DEVICE: usize = 18;

fn thread_global_index(device_idx: usize, thread_idx: usize) -> usize {
    thread_idx + (THREADS_PER_DEVICE * device_idx)
}
```

## 2. Critical Device Configuration Flags

**OpenVINO VPU Configuration (applies to NCAPI):**

```python
net_config = {
    'HW_STAGES_OPTIMIZATION': 'YES',   # ⚠️ Critical for performance
    'COMPUTE_LAYOUT': 'VPU_NCHW',      # Tensor layout format
    'RESHAPE_OPTIMIZATION': 'NO'        # Stability vs performance
}
```

**IMPORTANT WARNING:**
> `HW_STAGES_OPTIMIZATION` can cause precision issues with certain models due to half-precision overflow. Models must be regenerated with Model Optimizer `--scale` parameter (4, 8, or 16) to enable this safely.

**Rust Implementation:**
```rust
pub struct VpuConfig {
    pub hw_stages_optimization: bool,
    pub compute_layout: ComputeLayout,
    pub reshape_optimization: bool,
}

pub enum ComputeLayout {
    VpuNchw,  // Native format for VPU
    VpuNhwc,
}
```

## 3. FIFO Queue Depth Tuning

**Optimal Configuration:**
- Start with **2-6 elements per FIFO**
- Balance memory usage vs queue depth
- Larger queues help with variable processing times

**From NCAPI v2 docs:**
> "Using threads allows queuing inferences with one thread and reading inferences with another thread, achieving higher throughput without needing large FIFO capacity."

**Implementation:**
```rust
pub const DEFAULT_FIFO_DEPTH: usize = 4;  // Sweet spot
pub const MIN_FIFO_DEPTH: usize = 2;
pub const MAX_FIFO_DEPTH: usize = 10;

impl Fifo {
    pub fn create_with_optimal_depth(
        name: &str,
        fifo_type: FifoType,
        tensor_size: usize,
    ) -> Result<Self> {
        let depth = match tensor_size {
            0..=1_000_000 => 6,      // Small tensors: deeper queue
            1_000_001..=10_000_000 => 4,  // Medium: default
            _ => 2,                  // Large: shallow queue
        };
        Self::create_with_depth(name, fifo_type, depth)
    }
}
```

## 4. Async Inference Pipeline Pattern

**Producer-Consumer Architecture:**

```rust
// Producer thread (inference submission)
async fn inference_producer(
    graph: Arc<Graph>,
    input_fifo: Arc<Fifo>,
    work_queue: Receiver<InferenceRequest>,
) -> Result<()> {
    for request in work_queue {
        // Non-blocking write (or block if queue full)
        input_fifo.write_elem(&request.tensor, Some(request.id))?;
        graph.queue_inference()?;
    }
    Ok(())
}

// Consumer thread (result collection)
async fn inference_consumer(
    output_fifo: Arc<Fifo>,
    result_queue: Sender<InferenceResult>,
) -> Result<()> {
    loop {
        // Blocking read (10 second timeout)
        match output_fifo.read_elem_timeout(Duration::from_secs(10)) {
            Ok((data, user_param)) => {
                let result = InferenceResult {
                    id: user_param.unwrap_or(0),
                    data,
                    timestamp: Instant::now(),
                };
                result_queue.send(result).await?;
            }
            Err(e) if e.is_timeout() => continue,
            Err(e) => return Err(e),
        }
    }
}
```

**Key Pattern:**
- Fixed-size result queue (6 elements as in benchmark_ncs.py)
- Barrier synchronization for thread launch/completion
- Image preprocessing BEFORE threading (cache all inputs)

## 5. Multi-Device Load Balancing

**Critical Insight from Community:**
> "Create separate ExecutableNetwork instances per device (not one shared plugin)"

**Device Specification Format:**
```python
device_string = "MULTI:MYRIAD.1.1.2-ma2480,MYRIAD.1.1.4-ma2480"
# Format: comma-separated, no spaces
```

**Rust Implementation:**
```rust
pub struct MultiDevicePool {
    devices: Vec<DeviceHandle>,
    load_metrics: Vec<AtomicUsize>,
}

impl MultiDevicePool {
    pub fn distribute_work(&self, tensor: Tensor) -> usize {
        // Round-robin or least-loaded selection
        self.devices
            .iter()
            .enumerate()
            .min_by_key(|(idx, _)| self.load_metrics[*idx].load(Ordering::Relaxed))
            .map(|(idx, _)| idx)
            .unwrap_or(0)
    }

    pub fn optimal_request_count(&self, device_idx: usize) -> usize {
        // Query device capability
        self.devices[device_idx]
            .get_option(DeviceOption::OptimalInferRequests)
            .unwrap_or(18)
    }
}
```

## 6. Resource Cleanup Order (CRITICAL!)

**From NCAPI v2 Documentation:**
> Cleanup order MUST be: FIFOs → Graphs → Devices

**Wrong Order = Segfault or Resource Leak**

```rust
impl Drop for InferencePipeline {
    fn drop(&mut self) {
        // 1. Destroy FIFOs first
        if let Some(fifo) = self.input_fifo.take() {
            let _ = fifo.destroy();  // Ignore errors during drop
        }
        if let Some(fifo) = self.output_fifo.take() {
            let _ = fifo.destroy();
        }

        // 2. Destroy graph second
        if let Some(graph) = self.graph.take() {
            let _ = graph.destroy();
        }

        // 3. Device destroyed separately (may be shared)
        // Do NOT destroy device here if it's Arc-shared!
    }
}
```

## 7. Performance Monitoring & Thermal Management

**Key Device Options:**

```rust
pub enum DeviceOption {
    // Performance monitoring
    ThermalStats,              // Temperature in Celsius (f32)
    ThermalThrottlingLevel,    // 0=none, 1=lower, 2=upper threshold
    CurrentMemoryUsed,         // Bytes allocated
    MemorySize,                // Total available bytes

    // Resource limits
    MaxFifoNum,                // Max FIFO queues
    AllocatedFifoNum,          // Currently allocated
    MaxGraphNum,               // Max graphs
    AllocatedGraphNum,         // Currently allocated

    // Performance tuning
    MaxExecutorsNum,           // Max executor threads
}

// Monitor thermal throttling
pub fn check_thermal_throttling(device: &Device) -> Result<ThrottleLevel> {
    let level = device.get_option_u32(DeviceOption::ThermalThrottlingLevel)?;
    Ok(match level {
        0 => ThrottleLevel::None,
        1 => ThrottleLevel::LowerThreshold,  // Warning
        2 => ThrottleLevel::UpperThreshold,  // Critical!
        _ => ThrottleLevel::Unknown,
    })
}
```

## 8. Pipelining Performance Validation

**Key Performance Counter:**
> `KEY_VPU_PRINT_RECEIVE_TENSOR_TIME`: Check efficiency of pipelining. In a perfect pipeline, this time should be near zero.

**Implementation:**
```rust
pub struct PipelineMetrics {
    pub receive_tensor_time: Duration,  // Time VPU waits for input
    pub compute_time: Duration,         // Actual inference time
    pub queue_depth: usize,             // Current FIFO fill level
}

impl PipelineMetrics {
    pub fn is_optimal(&self) -> bool {
        // VPU should never wait for input
        self.receive_tensor_time < Duration::from_micros(100)
    }

    pub fn bottleneck(&self) -> Bottleneck {
        if self.receive_tensor_time > Duration::from_millis(1) {
            Bottleneck::InputStarved  // Increase FIFO depth or producer threads
        } else if self.queue_depth > 5 {
            Bottleneck::OutputStalled  // Speed up consumer threads
        } else {
            Bottleneck::None
        }
    }
}
```

## 9. Memory Management Best Practices

**CRITICAL LIMITATION:**
> "NCSDK v2 processes only one inference at a time, but can queue multiple inferences; batch inference (multiple simultaneous inferences) is NOT supported."

**Resource Monitoring:**
```rust
pub struct ResourceMonitor {
    device: Arc<Device>,
}

impl ResourceMonitor {
    pub fn check_resources(&self) -> Result<ResourceStatus> {
        let memory_used = self.device.get_option_u64(DeviceOption::CurrentMemoryUsed)?;
        let memory_total = self.device.get_option_u64(DeviceOption::MemorySize)?;
        let graphs = self.device.get_option_u32(DeviceOption::AllocatedGraphNum)?;
        let max_graphs = self.device.get_option_u32(DeviceOption::MaxGraphNum)?;

        Ok(ResourceStatus {
            memory_percent: (memory_used * 100 / memory_total) as u8,
            graphs_used: graphs,
            graphs_available: max_graphs - graphs,
            can_allocate_graph: graphs < max_graphs && memory_percent < 90,
        })
    }
}
```

## 10. Data Type Handling & Conversion

**From NCAPI v2 Docs:**
- FP32 input **automatically converts** to FP16 for processing
- FP16 output **automatically converts** back to FP32 if requested
- Default: FP32 for ease of use

**Our SIMD Implementation Advantage:**
- AVX2: 8 conversions per instruction (8x speedup)
- NEON: 4 conversions per instruction (4x speedup)
- Already implemented in `conversion.rs`

**Configuration:**
```rust
pub struct FifoDataConfig {
    pub host_type: DataType,    // What app provides (FP32)
    pub device_type: DataType,  // What VPU uses (FP16)
    pub auto_convert: bool,     // Let driver handle conversion
}

impl Fifo {
    pub fn optimal_config() -> FifoDataConfig {
        FifoDataConfig {
            host_type: DataType::FP32,    // Easier for app
            device_type: DataType::FP16,  // VPU native
            auto_convert: true,            // Our SIMD shines here!
        }
    }
}
```

## 11. User Parameter Pattern (Request Tracking)

**From benchmark_ncs.py:**
```python
dev_thread_request_id = dev_thread_index * simultaneous_infer_per_thread
for infer_id in range(simultaneous_infer_per_thread):
    request_id = dev_thread_request_id + infer_id
    # Attach request_id to inference
```

**Rust Implementation:**
```rust
pub type RequestId = usize;

pub struct InferenceTracker {
    next_id: AtomicUsize,
    pending: DashMap<RequestId, InferenceMetadata>,
}

impl InferenceTracker {
    pub fn allocate_id(&self, device_idx: usize, thread_idx: usize, infer_idx: usize) -> RequestId {
        device_idx * 100_000 + thread_idx * 1_000 + infer_idx
    }

    pub fn submit(&self, id: RequestId, metadata: InferenceMetadata) {
        self.pending.insert(id, metadata);
    }

    pub fn complete(&self, id: RequestId) -> Option<InferenceMetadata> {
        self.pending.remove(&id).map(|(_, v)| v)
    }
}
```

## 12. Recommended Implementation Priority

1. **HIGH PRIORITY** - Must implement for production:
   - Threading model (3 threads × 6 requests)
   - FIFO depth tuning (2-6 elements)
   - Cleanup order enforcement (Drop impl)
   - Thermal monitoring
   - Resource tracking

2. **MEDIUM PRIORITY** - Significant performance impact:
   - HW_STAGES_OPTIMIZATION flag
   - Async inference pipeline
   - Multi-device load balancing
   - Pipeline metrics

3. **LOW PRIORITY** - Nice to have:
   - Request ID tracking
   - Detailed performance counters
   - Auto-tuning FIFO depth

## Summary: Key Takeaways

1. **18 concurrent requests per device** (3×6) is the proven sweet spot
2. **FIFO depth of 4** is optimal for most workloads
3. **HW_STAGES_OPTIMIZATION** is critical but requires model scaling
4. **Cleanup order** (FIFOs→Graphs→Devices) prevents crashes
5. **Thermal monitoring** prevents performance degradation
6. **Producer-consumer pattern** with separate threads maximizes throughput
7. **Multi-device** requires separate handles and load balancing
8. **Our SIMD conversions** are already optimal (8x speedup on AVX2)
9. **One inference at a time** (no batch processing in hardware)
10. **Pipeline metrics** validate optimal configuration

## Next Steps

1. Implement threading model with 18 concurrent requests
2. Add thermal monitoring and resource tracking
3. Enforce cleanup order in Drop implementations
4. Add FIFO depth auto-tuning
5. Implement pipeline performance metrics
6. Add multi-device load balancing
7. Create benchmarks matching benchmark_ncs.py patterns
