//! Basic inference example

use movidius_ncapi::*;

fn main() -> Result<()> {
    println!("Movidius NCAPI v2 Rust Implementation");
    println!("Version: {:?}", VERSION);

    // Create device
    println!("Creating device...");
    let device = Device::create(0)?;
    
    println!("Opening device...");
    device.write().open()?;
    
    println!("Device opened successfully!");
    println!("State: {:?}", device.read().state());

    // Create graph
    println!("\nCreating graph...");
    let mut graph = Graph::create("example_graph")?;
    
    // Simulate graph data
    let graph_data = vec![0u8; 1024];
    
    println!("Allocating graph with FIFOs...");
    let (input_fifo, output_fifo) = graph.allocate_with_fifos(
        device.clone(),
        &graph_data,
    )?;
    
    println!("Graph allocated successfully!");
    println!("State: {:?}", graph.state());

    // Prepare input data (224x224x3 RGB image)
    let input_size = 224 * 224 * 3;
    let input_data = vec![0.5f32; input_size];
    let input_bytes = bytemuck::cast_slice(&input_data);

    println!("\nSubmitting inference...");
    graph.queue_inference_with_fifo_elem(
        &input_fifo,
        &output_fifo,
        input_bytes,
        None,
    )?;
    
    println!("Inference queued!");

    // In a real implementation, we would read from output_fifo here
    // let (output, _user_data) = output_fifo.read().read_elem()?;
    // println!("Output size: {} bytes", output.len());

    // Cleanup
    println!("\nCleaning up...");
    drop(output_fifo);
    drop(input_fifo);
    drop(graph);
    
    device.write().close()?;
    
    println!("Done!");
    Ok(())
}
