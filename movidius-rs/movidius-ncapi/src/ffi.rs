//! C FFI bindings for NCAPI v2 compatibility

use crate::*;
use std::ffi::{c_char, c_int, c_uint, c_void, CStr};
use std::ptr;
use std::sync::Arc;
use parking_lot::RwLock;

// Opaque handle types for C
#[repr(C)]
pub struct ncDeviceHandle_t {
    _private: [u8; 0],
}

#[repr(C)]
pub struct ncGraphHandle_t {
    _private: [u8; 0],
}

#[repr(C)]
pub struct ncFifoHandle_t {
    _private: [u8; 0],
}

// Convert Rust types to C opaque pointers
#[inline]
fn device_to_c(dev: Arc<RwLock<Device>>) -> *mut ncDeviceHandle_t {
    Box::into_raw(Box::new(dev)) as *mut ncDeviceHandle_t
}

#[inline]
unsafe fn c_to_device(ptr: *mut ncDeviceHandle_t) -> Arc<RwLock<Device>> {
    let boxed = Box::from_raw(ptr as *mut Arc<RwLock<Device>>);
    let handle = (*boxed).clone();
    Box::leak(boxed);
    handle
}

#[inline]
fn graph_to_c(graph: Graph) -> *mut ncGraphHandle_t {
    Box::into_raw(Box::new(Arc::new(RwLock::new(graph)))) as *mut ncGraphHandle_t
}

/// Create a device handle
#[no_mangle]
pub extern "C" fn ncDeviceCreate(
    index: c_int,
    device_handle: *mut *mut ncDeviceHandle_t,
) -> c_int {
    if device_handle.is_null() {
        return Status::InvalidParameters.to_i32();
    }

    match Device::create(index) {
        Ok(dev) => {
            unsafe {
                *device_handle = device_to_c(dev);
            }
            Status::Ok.to_i32()
        }
        Err(e) => e.to_status().to_i32(),
    }
}

/// Open a device
#[no_mangle]
pub extern "C" fn ncDeviceOpen(device_handle: *mut ncDeviceHandle_t) -> c_int {
    if device_handle.is_null() {
        return Status::InvalidHandle.to_i32();
    }

    let dev = unsafe { c_to_device(device_handle) };
    match dev.write().open() {
        Ok(()) => Status::Ok.to_i32(),
        Err(e) => e.to_status().to_i32(),
    }
}

/// Close a device
#[no_mangle]
pub extern "C" fn ncDeviceClose(device_handle: *mut ncDeviceHandle_t) -> c_int {
    if device_handle.is_null() {
        return Status::InvalidHandle.to_i32();
    }

    let dev = unsafe { c_to_device(device_handle) };
    match dev.write().close() {
        Ok(()) => Status::Ok.to_i32(),
        Err(e) => e.to_status().to_i32(),
    }
}

/// Destroy a device handle
#[no_mangle]
pub extern "C" fn ncDeviceDestroy(device_handle: *mut *mut ncDeviceHandle_t) -> c_int {
    if device_handle.is_null() || unsafe { (*device_handle).is_null() } {
        return Status::InvalidHandle.to_i32();
    }

    unsafe {
        let ptr = *device_handle as *mut Arc<RwLock<Device>>;
        let _boxed = Box::from_raw(ptr);
        *device_handle = ptr::null_mut();
    }

    Status::Ok.to_i32()
}

/// Create a graph handle
#[no_mangle]
pub extern "C" fn ncGraphCreate(
    name: *const c_char,
    graph_handle: *mut *mut ncGraphHandle_t,
) -> c_int {
    if name.is_null() || graph_handle.is_null() {
        return Status::InvalidParameters.to_i32();
    }

    let name_str = unsafe {
        match CStr::from_ptr(name).to_str() {
            Ok(s) => s,
            Err(_) => return Status::InvalidParameters.to_i32(),
        }
    };

    match Graph::create(name_str) {
        Ok(graph) => {
            unsafe {
                *graph_handle = graph_to_c(graph);
            }
            Status::Ok.to_i32()
        }
        Err(e) => e.to_status().to_i32(),
    }
}

// Add remaining FFI functions as needed...
