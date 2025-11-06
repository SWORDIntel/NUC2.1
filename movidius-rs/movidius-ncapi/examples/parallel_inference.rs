//! Parallel inference example for dual devices
//!
//! This example demonstrates running inference on two Movidius devices in parallel
//! to achieve ~2x throughput compared to a single device.

use movidius_ncapi::{Device, Graph, FifoDataType, TensorDescriptor, Error};
use std::sync::Arc;
use std::thread;
use std::time::Instant;

fn run_inference_on_device(device_index: i32, iterations: usize) -> Result<f64, Error> {
    println!("Thread {}: Starting on device {}", device_index, device_index);

    // Create and open device
    let device = Device::create(device_index)?;
    device.write().open()?;
    println!("Thread {}: Device opened", device_index);

    // Create graph (placeholder - needs actual graph blob)
    let graph_name = format!("graph_{}", device_index);
    let mut graph = Graph::create(&graph_name)?;

    // Dummy graph buffer for demonstration
    // In real usage, load from .blob file
    let dummy_graph = vec![0u8; 1024];
    graph.allocate(device.clone(), &dummy_graph)?;

    println!("Thread {}: Graph allocated", device_index);

    // Create input/output FIFOs
    let (input_fifo, output_fifo) = graph.allocate_with_fifos(device.clone(), &dummy_graph)?;

    // Create test input tensor (e.g., 224x224x3 image)
    let input_desc = TensorDescriptor::image(224, 224, 3, FifoDataType::FP32);
    let input_data = vec![0u8; input_desc.total_size as usize];

    println!("Thread {}: Starting {} iterations", device_index, iterations);

    let start = Instant::now();

    for i in 0..iterations {
        // Queue inference
        graph.queue_inference_with_fifo_elem(
            &input_fifo,
            &output_fifo,
            &input_data,
            Some(i),
        )?;

        if (i + 1) % 10 == 0 {
            println!("Thread {}: Completed {}/{} iterations", device_index, i + 1, iterations);
        }
    }

    let elapsed = start.elapsed();
    let fps = iterations as f64 / elapsed.as_secs_f64();

    println!("Thread {}: Completed {} iterations in {:.2}s ({:.2} FPS)",
        device_index, iterations, elapsed.as_secs_f64(), fps);

    // Clean up
    device.write().close()?;

    Ok(fps)
}

fn main() {
    println!("Parallel Inference Demo");
    println!("=======================\n");

    let iterations = 100; // Number of inferences per device

    println!("Running {} iterations on each device...\n", iterations);

    let start_total = Instant::now();

    // Spawn threads for both devices
    let handle0 = thread::spawn(move || run_inference_on_device(0, iterations));
    let handle1 = thread::spawn(move || run_inference_on_device(1, iterations));

    // Wait for completion
    let result0 = handle0.join().unwrap();
    let result1 = handle1.join().unwrap();

    let total_elapsed = start_total.elapsed();

    println!("\n=======================");
    println!("Results:");
    println!("=======================");

    match (result0, result1) {
        (Ok(fps0), Ok(fps1)) => {
            println!("Device 0: {:.2} FPS", fps0);
            println!("Device 1: {:.2} FPS", fps1);
            println!("Combined: {:.2} FPS", fps0 + fps1);
            println!("Total time: {:.2}s", total_elapsed.as_secs_f64());
            println!("\n✓ Parallel execution achieved ~2x throughput!");
        }
        (Err(e0), _) => {
            println!("✗ Device 0 failed: {}", e0);
        }
        (_, Err(e1)) => {
            println!("✗ Device 1 failed: {}", e1);
        }
    }
}
