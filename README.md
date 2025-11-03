# High-Performance Linux Driver for Intel Movidius Myriad X VPU

This repository contains a custom, high-performance Linux kernel driver for the Intel Movidius Myriad X VPU. The driver is designed to provide a low-latency, high-throughput interface for submitting inference requests to the VPU, making it suitable for demanding machine learning applications.

## Features

*   **Zero-Copy Data Path:** The driver uses a `mmap`'d DMA buffer to share data between user-space and the kernel, eliminating the need for expensive memory copies.
*   **Asynchronous I/O with `io_uring`:** The driver uses the modern `io_uring` interface for submitting inference requests, providing a high-performance, low-latency alternative to traditional `ioctl` and `eventfd` mechanisms.
*   **Persistent URB Pool:** The driver pre-allocates and reuses a pool of USB Request Blocks (URBs) to minimize the overhead of USB communication.
*   **Concurrent Request Processing:** The driver can process multiple in-flight inference requests concurrently, keeping the VPU's pipeline full and maximizing throughput.
*   **Scatter-Gather (SG) DMA:** The driver supports scatter-gather DMA, allowing a single inference request to be composed of multiple, non-contiguous memory buffers.

## Building and Installing the Driver

### Prerequisites

*   A Linux kernel with `io_uring` support (5.1 or later).
*   The kernel headers for your running kernel.
*   `liburing-dev` installed.

### Building

To build the driver, simply run `make`:

```bash
make
```

This will produce a kernel module file named `movidius_x_vpu.ko`.

### Installing

To install the driver, run the following command:

```bash
sudo insmod movidius_x_vpu.ko
```

The driver will create a character device at `/dev/movidius_x_vpu`.

### Performance Tuning

The driver exposes two module parameters that can be used for performance tuning:

*   `submission_cpu`: The CPU to bind the submission thread to. Pinning the submission thread to a specific CPU can improve cache locality and reduce context switching.
*   `irq_cpu`: The CPU to affinitize USB interrupts to. Pinning USB interrupts to a specific CPU can reduce interrupt latency and improve throughput.
*   `batch_delay_ms`: The maximum time in ms to wait for a batch to fill up. Increasing this value can improve throughput at the cost of increased latency.

To use these parameters, specify them when loading the driver:

```bash
sudo insmod movidius_x_vpu.ko submission_cpu=2 irq_cpu=3
```

For best performance, it is recommended to bind the submission thread and USB interrupts to different CPUs on the same NUMA node as the USB host controller.

## Building and Running the Test Application

### Building

To build the test application, run `make` with the `Makefile.test` file:

```bash
make -f Makefile.test
```

This will produce an executable file named `test_app`.

### Running

To run the test application, simply execute the `test_app` binary:

```bash
./test_app
```

The test application will submit a batch of inference requests to the driver and print a message to the console when each request is complete.

To list the available devices, use the `--list` command-line option:

```bash
./test_app --list
```

### udev Rule

To allow non-root users to access the device, you can add the following `udev` rule to `/etc/udev/rules.d/99-movidius.rules`:

```
SUBSYSTEM=="usb", ATTR{idVendor}=="03e7", ATTR{idProduct}=="2485", MODE="0660", GROUP="plugdev", SYMLINK+="movidius_x_vpu%n"
```

## Scatter-Gather DMA

The driver supports scatter-gather DMA, which allows a single inference request to be composed of multiple, non-contiguous memory buffers. This is useful for complex neural networks where different inputs or layers may be prepared in separate memory regions.

### Data Structures

To use scatter-gather DMA, you need to populate the `inference_request` struct with an array of `movidius_sg_segment` structs.

```c
#define MAX_SG_SEGMENTS 16

struct movidius_sg_segment {
    __u32 offset;
    __u32 len;
};

struct inference_request {
    __u32 num_input_segs;
    __u32 num_output_segs;
    struct movidius_sg_segment input_segs[MAX_SG_SEGMENTS];
    struct movidius_sg_segment output_segs[MAX_SG_SEGMENTS];
    u64 user_data;
};
```

### Example

Here is an example of how to prepare and submit an inference request with two input segments and one output segment:

```c
/* Prepare a request with 2 input segments and 1 output segment */
req->num_input_segs = 2;
req->num_output_segs = 1;

/* First input segment */
strcpy(dma_buffer, "Hello from ");
req->input_segs[0].offset = 0;
req->input_segs[0].len = strlen("Hello from ");

/* Second input segment */
strcpy(dma_buffer + 1024, "user space!");
req->input_segs[1].offset = 1024;
req->input_segs[1].len = strlen("user space!");

/* Output segment */
req->output_segs[0].offset = 2048;
req->output_segs[0].len = 128;

req->user_data = 1;

/* Submit the request using io_uring */
```
