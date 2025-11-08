//! Device enumeration example
//!
//! This example enumerates all available Movidius devices and prints their status.

use movidius_ncapi::{Device, Status};

fn main() {
    println!("Movidius Device Enumeration");
    println!("===========================\n");

    let mut found_devices = 0;
    let max_devices = 8;

    for index in 0..max_devices {
        print!("Device {}: ", index);

        match Device::create(index) {
            Ok(device) => {
                found_devices += 1;
                println!("✓ Found");

                // Try to open the device
                match device.write().open() {
                    Ok(()) => {
                        println!("  Status: Opened successfully");

                        // Close the device
                        if let Err(e) = device.write().close() {
                            println!("  Warning: Failed to close - {:?}", e);
                        }
                    }
                    Err(e) => {
                        println!("  Warning: Failed to open - {:?}", e);
                    }
                }
            }
            Err(e) => {
                let status = e.to_status();
                if status == Status::DeviceNotFound {
                    // This is expected for indices without devices
                    println!("✗ Not found");
                } else {
                    println!("✗ Error: {:?}", e);
                }
            }
        }
    }

    println!("\n===========================");
    println!("Total devices found: {}", found_devices);

    if found_devices == 0 {
        println!("\nNo devices detected. Please check:");
        println!("  1. Movidius stick is plugged in");
        println!("  2. Kernel module is loaded (modprobe movidius_x_vpu)");
        println!("  3. Device files exist in /dev/movidius_x_vpu_*");
        println!("  4. You have permissions to access the devices");
        std::process::exit(1);
    } else {
        println!("\n✓ System ready for inference!");
    }
}
