# High-Performance Linux Driver for Intel Movidius Myriad X VPU

This repository contains a custom, high-performance Linux kernel driver for the Intel Movidius Myriad X VPU. The driver is designed to provide a low-latency, high-throughput interface for submitting inference requests to the VPU, making it suitable for demanding machine learning applications.

## Features

*   **Zero-Copy Data Path:** The driver uses a `mmap`'d DMA buffer to share data between user-space and the kernel, eliminating the need for expensive memory copies.
*   **Asynchronous I/O with `io_uring`:** The driver uses the modern `io_uring` interface for submitting inference requests, providing a high-performance, low-latency alternative to traditional `ioctl` and `eventfd` mechanisms.
*   **Persistent URB Pool:** The driver pre-allocates and reuses a pool of USB Request Blocks (URBs) to minimize the overhead of USB communication.
*   **Concurrent Request Processing:** The driver can process multiple in-flight inference requests concurrently, keeping the VPU's pipeline full and maximizing throughput.

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
