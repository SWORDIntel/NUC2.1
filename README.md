# Movidius Myriad X VPU Linux Driver

This is a custom high-performance Linux kernel driver for the Intel Movidius Myriad X VPU, designed for low-latency, high-throughput deep learning inference.

## Features

This driver implements several advanced features to maximize the performance of the Myriad X VPU:

1.  **Zero-Copy Data Path**: The driver utilizes `pin_user_pages` and a custom `ioctl` (`MOVIDIUS_IOCTL_REGISTER_DMA_ARENA`) to map user-space buffers directly into the kernel address space. This allows the USB hardware to perform DMA directly from user memory, eliminating all intermediate copies (`memcpy`) between user and kernel space for inference data. This significantly reduces CPU overhead and per-inference latency.

2.  **`io_uring` Interface**: The driver exposes a modern, high-performance `io_uring` interface for submitting inference requests. This is a true asynchronous, low-latency queue that minimizes syscall overhead, context switches, and locks, dramatically improving requests-per-second (QPS) for small models.

3.  **Batch Submission & Adaptive Batching**: The driver supports batching multiple inference requests into a single `io_uring` command (`MOVIDIUS_URING_CMD_SUBMIT_BATCH`). Furthermore, it implements an adaptive batching strategy in the kernel. A timer (`batch_delay_ms`) and a queue depth threshold (`batch_high_watermark`) are used to dynamically decide when to dispatch a batch, balancing the tradeoff between latency (for small batches) and throughput (for large batches). This allows the driver to maximize device utilization and overall throughput without violating latency SLOs under varying loads.

4.  **Persistent URB Pool & Asynchronous Submission**: A pool of USB Request Blocks (URBs) is pre-allocated at driver initialization and reused for all data transfers. Requests are submitted asynchronously from a dedicated kernel thread, which fetches pending requests from the `io_uring` queue. This avoids the overhead of allocating/freeing URBs and making blocking submissions for every inference.

5.  **Multi-Device Coordination & Scheduling**: The driver can manage multiple Myriad X VPUs simultaneously. It creates a character device for each VPU found. A round-robin scheduling policy is used to distribute inference requests across all available devices, improving aggregate throughput.

6.  **NUMA/CPU Affinity & IRQ Balancing**: The driver's submission thread and the USB controller's interrupt requests (IRQs) can be pinned to specific CPU cores using module parameters (`submission_cpu_affinity`, `usb_irq_affinity`). This improves cache locality and reduces cross-socket memory traffic, leading to lower latency jitter and better performance under heavy load.

7.  **Sysfs Telemetry**: Key performance metrics and device status are exposed via `sysfs`. This includes queue depth, device temperature, and performance counters, allowing for real-time monitoring and integration with external schedulers or thermal management daemons.

## Building the Driver

**Prerequisites**:
*   Linux kernel headers for your running kernel version.
*   `liburing` development library.

```bash
# Build the kernel module
make

# Build the test application
make test
```

## Usage

1.  **Load the driver**:
    ```bash
    sudo insmod movidius_x_vpu.ko
    ```
    You can specify module parameters to customize behavior:
    ```bash
    sudo insmod movidius_x_vpu.ko vendor_id=0x03e7 product_id=0x2485 submission_cpu_affinity=4 usb_irq_affinity=5
    ```

2.  **Verify device creation**:
    Check for the presence of `/dev/movidius_x_vpu[0-9]` device nodes.
    ```bash
    ls /dev/movidius*
    ```

3.  **Run the test application**:
    The test application demonstrates how to register a DMA buffer and submit inference requests using `io_uring`.
    ```bash
    ./test_app
    ```

## `io_uring` Interface Details

*   **`MOVIDIUS_URING_CMD_SUBMIT_INFERENCE`**: Submits a single inference request. `addr` points to a `struct inference_request`.
*   **`MOVIDIUS_URING_CMD_SUBMIT_BATCH`**: Submits a batch of requests. `addr` points to a `struct batch_inference_request`.
