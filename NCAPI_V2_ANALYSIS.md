# NCAPI v2 Driver Analysis and Improvement Recommendations

## Executive Summary

After comprehensive analysis of the Movidius NCAPI v2 documentation (both C and Python APIs) and the current driver implementation, I've identified significant architectural gaps and opportunities for improvement. The current driver is a **high-performance USB transport layer** but **lacks the complete NCAPI v2 abstraction layer**.

---

## Current Driver Architecture

### Strengths ✅

1. **Modern Linux Features**
   - io_uring async interface (more modern than NCAPI)
   - Zero-copy DMA with `pin_user_pages`
   - VFIO platform driver for device passthrough
   - Runtime power management
   - URB pooling for efficient USB transfers

2. **Performance Optimizations**
   - Adaptive batching with configurable thresholds
   - Dedicated submission kernel thread
   - CPU affinity support
   - High-resolution timers for batch dispatch
   - Lock-free URB pool

3. **Monitoring & Telemetry**
   - sysfs performance statistics
   - Hardware performance counters (simulated)
   - Thermal monitoring (simulated)
   - Real-time queue depth tracking

### Current API Surface

```c
// UAPI structures
struct inference_request {
    struct movidius_cmd_hdr hdr;
    uint32_t num_input_segs;
    uint32_t num_output_segs;
    struct movidius_sg_segment input_segs[MAX_SG_SEGMENTS];
    struct movidius_sg_segment output_segs[MAX_SG_SEGMENTS];
    uint64_t user_data;
};

// IOCTLs
MOVIDIUS_IOCTL_REGISTER_DMA_ARENA
MOVIDIUS_IOCTL_UNREGISTER_DMA_ARENA
MOVIDIUS_IOCTL_GET_DEVICE_INFO

// io_uring commands
MOVIDIUS_URING_CMD_SUBMIT_INFERENCE
MOVIDIUS_URING_CMD_SUBMIT_BATCH
```

---

## NCAPI v2 Specification Gap Analysis

### Critical Missing Components

#### 1. **Device Management Layer** ⚠️ HIGH PRIORITY

**What's Missing:**
- Device enumeration API (`ncDeviceCreate`, enumerate_devices)
- Device state machine (CREATED → OPENED → CLOSED)
- Device handles as first-class objects
- Device capability queries

**NCAPI v2 API:**
```c
// C API
ncStatus_t ncDeviceCreate(int index, struct ncDeviceHandle_t** deviceHandle);
ncStatus_t ncDeviceOpen(struct ncDeviceHandle_t* deviceHandle);
ncStatus_t ncDeviceClose(struct ncDeviceHandle_t* deviceHandle);
ncStatus_t ncDeviceDestroy(struct ncDeviceHandle_t** deviceHandle);

// Device options (all read-only)
NC_RO_DEVICE_THERMAL_STATS      // float[] - temperatures in °C
NC_RO_THERMAL_THROTTLING_LEVEL  // int (0-2)
NC_RO_DEVICE_CURRENT_MEMORY_USED // int - bytes
NC_RO_DEVICE_MEMORY_SIZE         // int - total bytes
NC_RO_DEVICE_MAX_FIFO_NUM        // int
NC_RO_DEVICE_ALLOCATED_FIFO_NUM  // int
NC_RO_DEVICE_MAX_GRAPH_NUM       // int
NC_RO_ALLOCATED_GRAPH_NUM        // int
NC_RO_DEVICE_FW_VERSION          // uint[4] - [major, minor, hw_type, build]
NC_RO_DEVICE_NAME                // char[]
NC_RO_DEVICE_HW_VERSION          // int - MA2450 or MA2480
```

**Current Driver:**
- Automatic USB device detection
- Single global device model
- Basic device info ioctl
- No state management

**Recommendation:**
Add a userspace library layer that provides NCAPI-compatible device management on top of the existing USB driver.

---

#### 2. **Graph Management System** ⚠️ HIGH PRIORITY

**What's Missing:**
- Graph handle abstraction
- Compiled graph loading from buffer
- Graph state machine (CREATED → ALLOCATED → WAITING_FOR_BUFFERS → RUNNING)
- Multiple graphs per device
- Graph options and metadata

**NCAPI v2 API:**
```c
// C API
ncStatus_t ncGraphCreate(const char* name, struct ncGraphHandle_t** graphHandle);
ncStatus_t ncGraphAllocate(struct ncDeviceHandle_t* deviceHandle,
                           struct ncGraphHandle_t* graphHandle,
                           const void* graphBuffer,
                           unsigned int graphBufferLength);
ncStatus_t ncGraphAllocateWithFifos(...);
ncStatus_t ncGraphAllocateWithFifosEx(...);
ncStatus_t ncGraphQueueInference(...);
ncStatus_t ncGraphDestroy(struct ncGraphHandle_t** graphHandle);

// Graph options
NC_RO_GRAPH_STATE                        // Current state
NC_RO_GRAPH_TIME_TAKEN                   // float[] - layer times (ms)
NC_RO_GRAPH_INPUT_COUNT                  // int (currently 1)
NC_RO_GRAPH_OUTPUT_COUNT                 // int (currently 1)
NC_RO_GRAPH_INPUT_TENSOR_DESCRIPTORS     // ncTensorDescriptor_t[]
NC_RO_GRAPH_OUTPUT_TENSOR_DESCRIPTORS    // ncTensorDescriptor_t[]
NC_RW_GRAPH_EXECUTORS_NUM                // int - executor threads
```

**Current Driver:**
- No graph concept
- Direct inference submission
- No graph metadata
- No compiled graph support

**Recommendation:**
Implement a graph management layer that:
1. Parses compiled graph files (mvNCCompile output)
2. Manages graph lifecycle and state
3. Stores tensor descriptors
4. Supports multiple concurrent graphs

---

#### 3. **FIFO Queue System** ⚠️ CRITICAL PRIORITY

**What's Missing:**
This is the **most critical gap**. NCAPI v2's entire data flow model is based on FIFOs:

**NCAPI v2 FIFO Concepts:**
```c
// FIFO creation and lifecycle
ncStatus_t ncFifoCreate(const char* name, ncFifoType_t type,
                        struct ncFifoHandle_t** fifoHandle);
ncStatus_t ncFifoAllocate(struct ncFifoHandle_t* fifo,
                         struct ncDeviceHandle_t* device,
                         struct ncTensorDescriptor_t* tensorDesc,
                         unsigned int numElem);
ncStatus_t ncFifoWriteElem(struct ncFifoHandle_t* fifoHandle,
                           const void* inputTensor,
                           unsigned int* inputTensorLength,
                           void* userParam);
ncStatus_t ncFifoReadElem(struct ncFifoHandle_t* fifoHandle,
                          void* outputData,
                          unsigned int* outputDataLen,
                          void** userParam);
ncStatus_t ncFifoDestroy(struct ncFifoHandle_t** fifo);

// FIFO types
NC_FIFO_HOST_RO  // Host read-only (for outputs)
NC_FIFO_HOST_WO  // Host write-only (for inputs)

// FIFO data types
NC_FIFO_FP16  // 16-bit floating point
NC_FIFO_FP32  // 32-bit floating point (default)

// FIFO options
NC_RW_FIFO_TYPE                   // FifoType
NC_RW_FIFO_CONSUMER_COUNT         // int (currently 1)
NC_RW_FIFO_DATA_TYPE              // FifoDataType
NC_RW_FIFO_DONT_BLOCK             // int (0=block, 1=non-block)
NC_RW_FIFO_HOST_TENSOR_DESCRIPTOR // ncTensorDescriptor_t*
NC_RO_FIFO_CAPACITY               // int - max elements
NC_RO_FIFO_READ_FILL_LEVEL        // int - readable tensors
NC_RO_FIFO_WRITE_FILL_LEVEL       // int - writable tensors
NC_RO_FIFO_ELEMENT_DATA_SIZE      // int - bytes per element
```

**FIFO Purpose:**
- Separate input and output data streams
- Support for multiple readers/writers
- Blocking vs non-blocking semantics
- Queue depth management (backpressure)
- Data type conversion (FP32 host ↔ FP16 device)
- Multiple FIFOs per graph (future: multi-input/output networks)

**Current Driver:**
- Direct inference submission with input/output segments
- No queue abstraction for users
- No separate input/output management
- No blocking/non-blocking control for users

**Recommendation:**
Implement a complete FIFO subsystem that:
1. Manages separate input/output queues
2. Handles data type conversion (FP32 ↔ FP16)
3. Provides blocking/non-blocking modes
4. Tracks fill levels for backpressure
5. Associates FIFOs with graphs

---

#### 4. **Tensor Descriptor System** ⚠️ HIGH PRIORITY

**What's Missing:**
- Tensor shape information (n, c, w, h)
- Stride information for non-contiguous data
- Data type specification
- Automatic size validation

**NCAPI v2 Structure:**
```c
struct ncTensorDescriptor_t {
    unsigned int n;          // Batch count (currently 1)
    unsigned int c;          // Channels per pixel
    unsigned int w;          // Width in pixels (1 for non-images)
    unsigned int h;          // Height in pixels
    unsigned int totalSize;  // Total size in bytes
    unsigned int cStride;    // Channel stride (bytes)
    unsigned int wStride;    // Horizontal stride (bytes)
    unsigned int hStride;    // Vertical stride (bytes)
    ncFifoDataType_t dataType; // FP16 or FP32
};
```

**Purpose:**
- Automatic size validation
- Data type specification
- Support for non-contiguous tensors
- Multi-dimensional array handling
- Stride-based access patterns

**Current Driver:**
- Manual size specification
- No shape validation
- No stride support
- Fixed data layout assumptions

**Recommendation:**
Add tensor descriptor support to:
1. Validate input/output sizes
2. Support various data layouts
3. Enable automatic data type conversion
4. Provide shape information to users

---

#### 5. **Compiled Graph File Support** ⚠️ MEDIUM PRIORITY

**What's Missing:**
- Graph file parsing (from mvNCCompile)
- Graph version validation
- Layer metadata extraction
- Multi-layer network support

**NCAPI v2 Workflow:**
```bash
# Compile network offline
mvNCCompile network.pb -s 12 -in input -on output -o graph.blob

# Load in application
with open('graph.blob', 'rb') as f:
    graph_buffer = f.read()

graph.allocate(device, graph_buffer)
```

**Graph File Contains:**
- Network topology
- Weights and biases
- Layer configurations
- Input/output tensor descriptors
- Optimization parameters
- Version information

**Current Driver:**
- No graph file support
- Direct inference submission
- No network topology awareness

**Recommendation:**
Either:
1. Add kernel-level graph file parsing, OR
2. Provide userspace library for graph management

---

#### 6. **Status Code System** ⚠️ MEDIUM PRIORITY

**What's Missing:**
Comprehensive error reporting system

**NCAPI v2 Status Codes:**
```c
typedef enum {
    NC_OK = 0,
    NC_BUSY = -1,
    NC_ERROR = -2,
    NC_OUT_OF_MEMORY = -3,
    NC_DEVICE_NOT_FOUND = -4,
    NC_INVALID_PARAMETERS = -5,
    NC_TIMEOUT = -6,
    NC_MVCMD_NOT_FOUND = -7,              // Boot file not found
    NC_NOT_ALLOCATED = -8,
    NC_UNAUTHORIZED = -9,
    NC_UNSUPPORTED_GRAPH_FILE = -10,
    NC_UNSUPPORTED_CONFIGURATION_FILE = -11,
    NC_UNSUPPORTED_FEATURE = -12,         // Feature not in firmware
    NC_MYRIAD_ERROR = -13,                // VPU-level error
    NC_INVALID_DATA_LENGTH = -14,
    NC_INVALID_HANDLE = -15
} ncStatus_t;
```

**Current Driver:**
- Standard Linux error codes (EINVAL, ENOMEM, etc.)
- Less specific error information

**Recommendation:**
Map driver errors to NCAPI-compatible status codes in userspace library.

---

#### 7. **Global Options** ⚠️ LOW PRIORITY

**What's Missing:**
```c
// Global options
NC_RW_LOG_LEVEL      // Logging verbosity
NC_RO_API_VERSION    // uint[4] - [major, minor, hotfix, release]

// Log levels
NC_LOG_DEBUG  // Full verbosity
NC_LOG_INFO   // Info and above
NC_LOG_WARN   // Warnings and above (default)
NC_LOG_ERROR  // Errors and above
NC_LOG_FATAL  // Fatal only
```

**Current Driver:**
- Kernel logging only
- No user-controllable log levels

**Recommendation:**
Add global options support in userspace library.

---

## Firmware Loading Implementation

**Current Status:** Stubbed out with TODOs

**What NCAPI Expects:**
```c
// Firmware is loaded automatically during ncDeviceOpen()
// No explicit firmware API exposed to users
```

**Required Implementation:**
```c
static int load_firmware(struct movidius_x_vpu_dev *dev)
{
    int ret;
    const struct firmware *fw;

    // 1. Request firmware from /lib/firmware/
    ret = request_firmware(&fw, MOVIDIUS_FIRMWARE_NAME, dev->dev);
    if (ret) {
        dev_err(dev->dev, "Firmware not found: %s\n", MOVIDIUS_FIRMWARE_NAME);
        return ret;
    }

    // 2. Validate firmware
    if (fw->size < MIN_FIRMWARE_SIZE || fw->size > MAX_FIRMWARE_SIZE) {
        dev_err(dev->dev, "Invalid firmware size: %zu\n", fw->size);
        release_firmware(fw);
        return -EINVAL;
    }

    // 3. Put device in bootloader mode
    ret = usb_control_msg(dev->udev,
                         usb_sndctrlpipe(dev->udev, 0),
                         USB_REQ_ENTER_BOOTLOADER,  // 0x01
                         USB_DIR_OUT | USB_TYPE_VENDOR,
                         0, 0, NULL, 0, 1000);
    if (ret < 0) {
        dev_err(dev->dev, "Failed to enter bootloader: %d\n", ret);
        release_firmware(fw);
        return ret;
    }

    // 4. Upload firmware in chunks
    size_t offset = 0;
    const size_t chunk_size = 512;  // USB control transfer limit

    while (offset < fw->size) {
        size_t to_send = min(chunk_size, fw->size - offset);

        ret = usb_control_msg(dev->udev,
                             usb_sndctrlpipe(dev->udev, 0),
                             USB_REQ_WRITE_FIRMWARE,  // 0x02
                             USB_DIR_OUT | USB_TYPE_VENDOR,
                             offset & 0xFFFF,      // wValue - low offset
                             offset >> 16,         // wIndex - high offset
                             (void *)(fw->data + offset),
                             to_send,
                             5000);

        if (ret < 0) {
            dev_err(dev->dev, "Firmware upload failed at offset %zu: %d\n",
                   offset, ret);
            release_firmware(fw);
            return ret;
        }

        offset += to_send;
    }

    // 5. Verify firmware CRC
    uint32_t expected_crc = calculate_crc32(fw->data, fw->size);
    uint32_t device_crc = 0;

    ret = usb_control_msg(dev->udev,
                         usb_rcvctrlpipe(dev->udev, 0),
                         USB_REQ_VERIFY_FIRMWARE,  // 0x03
                         USB_DIR_IN | USB_TYPE_VENDOR,
                         0, 0,
                         &device_crc, sizeof(device_crc),
                         1000);

    if (ret < 0 || device_crc != expected_crc) {
        dev_err(dev->dev, "Firmware verification failed\n");
        release_firmware(fw);
        return -EIO;
    }

    // 6. Boot new firmware
    ret = usb_control_msg(dev->udev,
                         usb_sndctrlpipe(dev->udev, 0),
                         USB_REQ_BOOT_FIRMWARE,  // 0x04
                         USB_DIR_OUT | USB_TYPE_VENDOR,
                         0, 0, NULL, 0, 1000);

    if (ret < 0) {
        dev_err(dev->dev, "Failed to boot firmware: %d\n", ret);
        release_firmware(fw);
        return ret;
    }

    // 7. Wait for device to enumerate with new firmware
    msleep(2000);

    dev->fw_info.fw = fw;
    dev->fw_info.loaded = true;
    dev->fw_info.version = *(uint32_t *)fw->data;

    dev_info(dev->dev, "Firmware loaded successfully: version 0x%08x\n",
             dev->fw_info.version);

    return 0;
}
```

**Required USB Control Requests:**
- `USB_REQ_ENTER_BOOTLOADER (0x01)` - Put device in bootloader mode
- `USB_REQ_WRITE_FIRMWARE (0x02)` - Write firmware chunk
- `USB_REQ_VERIFY_FIRMWARE (0x03)` - Verify firmware CRC
- `USB_REQ_BOOT_FIRMWARE (0x04)` - Boot new firmware
- `USB_REQ_GET_FW_VERSION (0x05)` - Query firmware version

**Note:** Actual USB request codes must be obtained from:
1. Intel Movidius hardware documentation
2. Reverse engineering existing drivers
3. Official Intel SDK source code

---

## Temperature and Performance Counter Reading

**Current Status:** Simulated values

**Required Implementation:**

### Temperature Reading
```c
static int read_temperature(struct movidius_x_vpu_dev *dev)
{
    int ret;
    struct {
        int32_t temperature;  // Temperature in millidegrees Celsius
        uint32_t timestamp;
    } temp_data;

    ret = usb_control_msg(dev->udev,
                         usb_rcvctrlpipe(dev->udev, 0),
                         USB_REQ_READ_REGISTER,  // 0x10
                         USB_DIR_IN | USB_TYPE_VENDOR,
                         MOVIDIUS_TEMP_SENSOR_REG,  // wValue - register address
                         0,  // wIndex
                         &temp_data,
                         sizeof(temp_data),
                         1000);

    if (ret == sizeof(temp_data)) {
        return temp_data.temperature / 1000;  // Convert to degrees C
    }

    dev_warn(dev->dev, "Failed to read temperature: %d\n", ret);
    return -1;
}
```

### Performance Counters
```c
static void read_hw_perf_counters(struct movidius_x_vpu_dev *dev)
{
    int ret;
    struct hw_perf_regs {
        uint64_t compute_cycles;
        uint64_t memory_read_bytes;
        uint64_t memory_write_bytes;
        uint64_t dma_transfers;
        uint32_t compute_utilization;  // Percentage * 100
        uint32_t memory_bandwidth;     // MB/s * 100
    } perf_regs;

    // Read performance counter block via USB
    ret = usb_control_msg(dev->udev,
                         usb_rcvctrlpipe(dev->udev, 0),
                         USB_REQ_READ_REGISTER,
                         USB_DIR_IN | USB_TYPE_VENDOR,
                         MOVIDIUS_PERF_COUNTER_BASE,
                         0,
                         &perf_regs,
                         sizeof(perf_regs),
                         1000);

    if (ret == sizeof(perf_regs)) {
        atomic64_set(&dev->hw_counters.compute_cycles, perf_regs.compute_cycles);
        atomic64_set(&dev->hw_counters.memory_read_bytes, perf_regs.memory_read_bytes);
        atomic64_set(&dev->hw_counters.memory_write_bytes, perf_regs.memory_write_bytes);
        atomic64_set(&dev->hw_counters.dma_transfers, perf_regs.dma_transfers);
        atomic64_set(&dev->hw_counters.compute_utilization, perf_regs.compute_utilization);
        atomic64_set(&dev->hw_counters.memory_bandwidth, perf_regs.memory_bandwidth);
    }
}
```

---

## Implementation Strategy

### Option 1: Userspace Compatibility Library (RECOMMENDED)

**Architecture:**
```
┌─────────────────────────────────────┐
│  User Application (NCAPI v2 API)   │
├─────────────────────────────────────┤
│  libmvnc_compat.so                  │
│  - Device/Graph/FIFO abstraction    │
│  - Graph file parsing               │
│  - Tensor descriptors               │
│  - State management                 │
│  - Error code mapping               │
├─────────────────────────────────────┤
│  /dev/movidius_x_vpu_0 (ioctl)     │
│  /dev/movidius_x_vpu_0 (io_uring)  │
├─────────────────────────────────────┤
│  movidius_x_vpu.ko (kernel driver)  │
│  - USB transport                    │
│  - DMA management                   │
│  - Performance optimization         │
└─────────────────────────────────────┘
```

**Benefits:**
- Maintains kernel driver performance optimizations
- Easier to update and maintain
- No kernel ABI changes required
- Can be distributed separately
- Userspace debugging is easier

**Implementation:**
```c
// libmvnc_compat.so

struct ncDeviceHandle_t {
    int fd;  // /dev/movidius_x_vpu_N
    struct io_uring ring;
    int state;
    struct device_info info;
    struct list_head graphs;
};

struct ncGraphHandle_t {
    struct ncDeviceHandle_t *device;
    char name[NC_MAX_NAME_SIZE];
    void *graph_buffer;
    size_t graph_size;
    int state;
    struct ncFifoHandle_t *input_fifos;
    struct ncFifoHandle_t *output_fifos;
    struct ncTensorDescriptor_t *input_descriptors;
    struct ncTensorDescriptor_t *output_descriptors;
};

struct ncFifoHandle_t {
    struct ncGraphHandle_t *graph;
    char name[NC_MAX_NAME_SIZE];
    ncFifoType_t type;
    ncFifoDataType_t data_type;
    int state;
    int capacity;
    int fill_level;
    struct ncTensorDescriptor_t tensor_desc;
    struct ring_buffer *queue;
};

ncStatus_t ncDeviceCreate(int index, struct ncDeviceHandle_t** handle) {
    char dev_path[32];
    snprintf(dev_path, sizeof(dev_path), "/dev/movidius_x_vpu_%d", index);

    int fd = open(dev_path, O_RDWR);
    if (fd < 0) {
        return NC_DEVICE_NOT_FOUND;
    }

    struct ncDeviceHandle_t *dev = calloc(1, sizeof(*dev));
    dev->fd = fd;
    dev->state = NC_DEVICE_CREATED;
    io_uring_queue_init(64, &dev->ring, 0);

    *handle = dev;
    return NC_OK;
}

// ... implement rest of NCAPI functions
```

---

### Option 2: Kernel-Level NCAPI Support

**Architecture:**
```
┌─────────────────────────────────────┐
│  User Application (NCAPI v2 API)   │
├─────────────────────────────────────┤
│  libmvnc.so (thin wrapper)          │
├─────────────────────────────────────┤
│  /dev/movidius_ncapi_0 (ioctl)     │
├─────────────────────────────────────┤
│  movidius_ncapi.ko                  │
│  - Device/Graph/FIFO management     │
│  - Graph parsing                    │
│  - State machines                   │
├─────────────────────────────────────┤
│  movidius_x_vpu.ko (transport)      │
└─────────────────────────────────────┘
```

**Benefits:**
- Complete kernel-level implementation
- Single API surface
- Kernel-enforced resource management

**Drawbacks:**
- More complex kernel code
- Harder to maintain and debug
- Kernel ABI stability concerns
- Graph parsing in kernel space

---

### Option 3: Hybrid Approach

Keep current high-performance driver for advanced users, add NCAPI compatibility layer for standard applications:

```
┌──────────────────────────────────────────────┐
│  Advanced Apps (direct io_uring)            │
│  ↓                                           │
│  /dev/movidius_x_vpu_0                       │
└──────────────────────────────────────────────┘

┌──────────────────────────────────────────────┐
│  NCAPI Apps (standard API)                   │
│  ↓                                           │
│  libmvnc_compat.so                           │
│  ↓                                           │
│  /dev/movidius_x_vpu_0                       │
└──────────────────────────────────────────────┘
```

---

## Specific Improvements from NCAPI Documentation

### 1. Add Blocking/Non-Blocking Modes

**NCAPI Feature:**
```c
// Set FIFO to non-blocking mode
int dont_block = 1;
ncFifoSetOption(fifo, NC_RW_FIFO_DONT_BLOCK, &dont_block, sizeof(dont_block));
```

**Current Driver:**
- Only async io_uring operations
- No blocking synchronous API

**Recommendation:**
Add synchronous read/write ioctls:
```c
#define MOVIDIUS_IOCTL_WRITE_SYNC _IOW('M', 10, struct sync_io_request)
#define MOVIDIUS_IOCTL_READ_SYNC _IOWR('M', 11, struct sync_io_response)

struct sync_io_request {
    uint32_t timeout_ms;  // 0 = blocking
    void *data;
    size_t len;
};
```

---

### 2. Add FP32 ↔ FP16 Conversion

**NCAPI Feature:**
- Host typically uses FP32
- Device uses FP16 for efficiency
- Automatic conversion in FIFO layer

**Current Driver:**
- No data type awareness
- No conversion support

**Recommendation:**
Add conversion helpers in userspace library:
```c
static void fp32_to_fp16(const float *src, uint16_t *dst, size_t count) {
    for (size_t i = 0; i < count; i++) {
        dst[i] = float_to_half(src[i]);
    }
}

static void fp16_to_fp32(const uint16_t *src, float *dst, size_t count) {
    for (size_t i = 0; i < count; i++) {
        dst[i] = half_to_float(src[i]);
    }
}
```

---

### 3. Add Graph Execution Time Reporting

**NCAPI Feature:**
```c
// Get per-layer execution times
float time_taken[100];
unsigned int time_taken_size = sizeof(time_taken);
ncGraphGetOption(graph, NC_RO_GRAPH_TIME_TAKEN, time_taken, &time_taken_size);
```

**Current Driver:**
- No per-layer timing
- Only total inference time

**Recommendation:**
Add detailed timing instrumentation:
```c
struct layer_timing {
    char layer_name[64];
    uint64_t start_ns;
    uint64_t end_ns;
    uint32_t compute_cycles;
};

#define MOVIDIUS_IOCTL_GET_LAYER_TIMINGS _IOR('M', 12, struct layer_timing[])
```

---

### 4. Add Multiple Executor Threads

**NCAPI Feature:**
```c
// Set number of executor threads for parallel execution
int num_executors = 4;
ncGraphSetOption(graph, NC_RW_GRAPH_EXECUTORS_NUM,
                &num_executors, sizeof(num_executors));
```

**Current Driver:**
- Single submission thread
- No parallel graph execution

**Recommendation:**
Add per-graph executor pool configuration:
```c
struct graph_config {
    uint32_t num_executors;
    uint32_t executor_affinity_mask;
};

#define MOVIDIUS_IOCTL_SET_GRAPH_CONFIG _IOW('M', 13, struct graph_config)
```

---

### 5. Add Device Memory Management

**NCAPI Feature:**
```c
// Query device memory usage
int current_mem, total_mem;
ncDeviceGetOption(dev, NC_RO_DEVICE_CURRENT_MEMORY_USED,
                 &current_mem, sizeof(current_mem));
ncDeviceGetOption(dev, NC_RO_DEVICE_MEMORY_SIZE,
                 &total_mem, sizeof(total_mem));
```

**Current Driver:**
- Fixed device info
- No dynamic memory tracking

**Recommendation:**
Add real-time memory tracking:
```c
struct memory_stats {
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
    uint32_t num_allocations;
    uint32_t fragmentation_pct;
};

#define MOVIDIUS_IOCTL_GET_MEMORY_STATS _IOR('M', 14, struct memory_stats)
```

---

## Priority Matrix

### Must Have (P0) - For NCAPI Compatibility
1. ✅ Userspace compatibility library (libmvnc_compat.so)
2. ✅ Device/Graph/FIFO handle abstraction
3. ✅ Tensor descriptor system
4. ✅ Graph file parsing support
5. ✅ State machine management
6. ✅ FP32/FP16 conversion
7. ⚠️ Real firmware loading (hardware-specific)

### Should Have (P1) - For Feature Parity
1. ⚠️ Real temperature reading (hardware-specific)
2. ⚠️ Real performance counters (hardware-specific)
3. ✅ Blocking/non-blocking modes
4. ✅ Error code compatibility
5. ✅ Global options support
6. ✅ Multiple executor threads

### Nice to Have (P2) - For Enhanced Experience
1. ✅ Per-layer timing
2. ✅ Memory usage tracking
3. ✅ Advanced debugging interfaces
4. ✅ NUMA awareness
5. ✅ QoS policies

**Legend:**
- ✅ = Can be implemented in software
- ⚠️ = Requires hardware documentation/access

---

## Testing Strategy

### Unit Tests
```python
# Test device enumeration
def test_device_create():
    for i in range(10):
        status, handle = ncDeviceCreate(i)
        if status == NC_DEVICE_NOT_FOUND:
            break
        assert status == NC_OK
        ncDeviceDestroy(handle)

# Test graph allocation
def test_graph_allocate():
    device = create_and_open_device()
    graph_buffer = load_graph_file("inception_v3.blob")

    status, graph = ncGraphCreate("test")
    assert status == NC_OK

    status = ncGraphAllocate(device, graph, graph_buffer, len(graph_buffer))
    assert status == NC_OK

    cleanup(graph, device)

# Test FIFO operations
def test_fifo_write_read():
    device, graph, input_fifo, output_fifo = setup_inference()

    # Write input
    input_data = np.random.randn(224, 224, 3).astype(np.float32)
    status = ncFifoWriteElem(input_fifo, input_data, len(input_data), None)
    assert status == NC_OK

    # Queue inference
    status = ncGraphQueueInference(graph, [input_fifo], [output_fifo])
    assert status == NC_OK

    # Read output
    output_data, user_param = ncFifoReadElem(output_fifo)
    assert len(output_data) > 0

    cleanup(output_fifo, input_fifo, graph, device)
```

### Integration Tests
```bash
# Test with official NCAPI applications
./test_official_app.sh

# Stress test
./stress_test.sh --duration=3600 --concurrent-inferences=100

# Performance benchmark
./benchmark.sh --compare-to-official
```

---

## Documentation Requirements

### 1. NCAPI Compatibility Guide
Document which NCAPI v2 features are supported:
- Function support matrix
- Known limitations
- Performance differences
- Migration guide from official SDK

### 2. API Reference
Complete API documentation matching NCAPI format:
- All function signatures
- Parameter descriptions
- Return value documentation
- Example code

### 3. Graph File Format
Document the expected graph file format:
- File structure
- Magic numbers
- Version compatibility
- Endianness

### 4. Hardware Requirements
Document required hardware features:
- Supported device models (MA2450, MA2480)
- Firmware version requirements
- USB endpoint requirements
- Power requirements

---

## Estimated Implementation Effort

### Phase 1: Core Compatibility (4-6 weeks)
- [ ] Userspace library skeleton
- [ ] Device handle management
- [ ] Graph handle management
- [ ] FIFO handle management
- [ ] State machine implementation
- [ ] Basic graph file parsing
- [ ] Tensor descriptor support
- [ ] Error code mapping

### Phase 2: Data Path (2-3 weeks)
- [ ] FIFO write implementation
- [ ] FIFO read implementation
- [ ] FP32/FP16 conversion
- [ ] Blocking/non-blocking modes
- [ ] Queue management
- [ ] Backpressure handling

### Phase 3: Advanced Features (2-3 weeks)
- [ ] Multiple graphs
- [ ] Executor thread management
- [ ] Per-layer timing
- [ ] Memory tracking
- [ ] Global options
- [ ] Advanced error handling

### Phase 4: Hardware Integration (2-4 weeks)
- [ ] Real firmware loading (depends on hardware docs)
- [ ] Temperature reading (depends on hardware docs)
- [ ] Performance counters (depends on hardware docs)
- [ ] Device capability detection

### Phase 5: Testing & Documentation (2-3 weeks)
- [ ] Unit test suite
- [ ] Integration tests
- [ ] Performance benchmarks
- [ ] API documentation
- [ ] Example applications
- [ ] Migration guide

**Total Estimated Effort: 12-19 weeks**

---

## Conclusion

The current driver is a **high-performance, modern Linux implementation** but lacks the **NCAPI v2 abstraction layer** required for compatibility with existing Movidius applications. The recommended approach is to implement a **userspace compatibility library (libmvnc_compat.so)** that provides NCAPI v2 semantics on top of the existing high-performance kernel driver.

**Key Takeaways:**

1. ✅ **Keep the existing kernel driver** - it has excellent performance optimizations
2. ✅ **Add userspace compatibility layer** - easier to maintain than kernel changes
3. ⚠️ **Hardware documentation needed** - for firmware loading, temperature, performance counters
4. ✅ **Hybrid approach** - support both NCAPI-compatible and high-performance direct access
5. ✅ **Well-defined scope** - clear implementation phases with realistic timeline

**Success Metrics:**

- Existing NCAPI v2 applications run without modification
- Performance within 10% of official SDK
- All NCAPI functions implemented (except hardware-specific features)
- Comprehensive test coverage
- Clear documentation

This analysis provides a complete roadmap for achieving NCAPI v2 compatibility while preserving the advanced features of the current driver implementation.
