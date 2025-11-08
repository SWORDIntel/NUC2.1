//! Production-ready C FFI bindings with proper lifetime management

use crate::*;
use parking_lot::RwLock;
use std::ffi::{c_char, c_int, c_uint, c_void, CStr};
use std::ptr;
use std::sync::Arc;

// Opaque handle types for C
/// Opaque handle for a Movidius Neural Compute device.
///
/// This is an opaque C-compatible type used in the FFI layer to represent a device handle.
/// It should only be created and manipulated through the nc* FFI functions.
#[repr(C)]
pub struct ncDeviceHandle_t {
    _private: [u8; 0],
}

/// Opaque handle for a neural network graph.
///
/// This is an opaque C-compatible type used in the FFI layer to represent a graph handle.
/// Graphs contain the neural network model that can be allocated on a device and executed.
/// It should only be created and manipulated through the nc* FFI functions.
#[repr(C)]
pub struct ncGraphHandle_t {
    _private: [u8; 0],
}

/// Opaque handle for a FIFO (First-In-First-Out) queue.
///
/// This is an opaque C-compatible type used in the FFI layer to represent a FIFO handle.
/// FIFOs are used to pass input data to graphs and receive output data from them.
/// They can be either host-readable (for outputs) or host-writable (for inputs).
/// It should only be created and manipulated through the nc* FFI functions.
#[repr(C)]
pub struct ncFifoHandle_t {
    _private: [u8; 0],
}

// Internal handle wrapper with ref counting
struct DeviceHandleWrapper {
    device: Arc<RwLock<Device>>,
    ref_count: std::sync::atomic::AtomicUsize,
}

struct GraphHandleWrapper {
    graph: Arc<RwLock<Graph>>,
    _ref_count: std::sync::atomic::AtomicUsize,
}

struct FifoHandleWrapper {
    fifo: Arc<RwLock<Fifo>>,
    _ref_count: std::sync::atomic::AtomicUsize,
}

// Safe conversion functions
#[inline]
fn device_to_c(dev: Arc<RwLock<Device>>) -> *mut ncDeviceHandle_t {
    Box::into_raw(Box::new(DeviceHandleWrapper {
        device: dev,
        ref_count: std::sync::atomic::AtomicUsize::new(1),
    })) as *mut ncDeviceHandle_t
}

#[inline]
unsafe fn c_to_device(ptr: *mut ncDeviceHandle_t) -> Result<Arc<RwLock<Device>>> {
    if ptr.is_null() {
        return Err(Error::Status(Status::InvalidHandle));
    }
    let wrapper = unsafe { &*(ptr as *const DeviceHandleWrapper) };
    Ok(wrapper.device.clone())
}

#[inline]
fn graph_to_c(graph: Arc<RwLock<Graph>>) -> *mut ncGraphHandle_t {
    Box::into_raw(Box::new(GraphHandleWrapper {
        graph,
        _ref_count: std::sync::atomic::AtomicUsize::new(1),
    })) as *mut ncGraphHandle_t
}

#[inline]
unsafe fn c_to_graph(ptr: *mut ncGraphHandle_t) -> Result<Arc<RwLock<Graph>>> {
    if ptr.is_null() {
        return Err(Error::Status(Status::InvalidHandle));
    }
    let wrapper = unsafe { &*(ptr as *const GraphHandleWrapper) };
    Ok(wrapper.graph.clone())
}

#[inline]
fn fifo_to_c(fifo: Arc<RwLock<Fifo>>) -> *mut ncFifoHandle_t {
    Box::into_raw(Box::new(FifoHandleWrapper {
        fifo,
        _ref_count: std::sync::atomic::AtomicUsize::new(1),
    })) as *mut ncFifoHandle_t
}

#[inline]
unsafe fn c_to_fifo(ptr: *mut ncFifoHandle_t) -> Result<Arc<RwLock<Fifo>>> {
    if ptr.is_null() {
        return Err(Error::Status(Status::InvalidHandle));
    }
    let wrapper = unsafe { &*(ptr as *const FifoHandleWrapper) };
    Ok(wrapper.fifo.clone())
}

// ========== Device Functions ==========

/// Create a device handle
///
/// # Safety
/// - device_handle must be a valid pointer
#[no_mangle]
pub unsafe extern "C" fn ncDeviceCreate(
    index: c_int,
    device_handle: *mut *mut ncDeviceHandle_t,
) -> c_int {
    if device_handle.is_null() {
        return Status::InvalidParameters.to_i32();
    }

    match Device::create(index) {
        Ok(dev) => {
            unsafe { *device_handle = device_to_c(dev) };
            Status::Ok.to_i32()
        }
        Err(e) => e.to_status().to_i32(),
    }
}

/// Open a device
///
/// # Safety
/// - device_handle must be a valid handle from ncDeviceCreate
#[no_mangle]
pub unsafe extern "C" fn ncDeviceOpen(device_handle: *mut ncDeviceHandle_t) -> c_int {
    match unsafe { c_to_device(device_handle) } {
        Ok(dev) => match dev.write().open() {
            Ok(()) => Status::Ok.to_i32(),
            Err(e) => e.to_status().to_i32(),
        },
        Err(e) => e.to_status().to_i32(),
    }
}

/// Close a device
///
/// # Safety
/// - device_handle must be a valid handle from ncDeviceCreate
#[no_mangle]
pub unsafe extern "C" fn ncDeviceClose(device_handle: *mut ncDeviceHandle_t) -> c_int {
    match unsafe { c_to_device(device_handle) } {
        Ok(dev) => match dev.write().close() {
            Ok(()) => Status::Ok.to_i32(),
            Err(e) => e.to_status().to_i32(),
        },
        Err(e) => e.to_status().to_i32(),
    }
}

/// Destroy a device handle
///
/// # Safety
/// - device_handle must be a valid pointer to a handle
/// - The handle must not be used after this call
#[no_mangle]
pub unsafe extern "C" fn ncDeviceDestroy(device_handle: *mut *mut ncDeviceHandle_t) -> c_int {
    if device_handle.is_null() || unsafe { (*device_handle).is_null() } {
        return Status::InvalidHandle.to_i32();
    }

    let ptr = unsafe { *device_handle as *mut DeviceHandleWrapper };
    let wrapper = unsafe { Box::from_raw(ptr) };

    // Check ref count
    if wrapper.ref_count.load(std::sync::atomic::Ordering::Acquire) != 1 {
        // Still has references, don't drop
        Box::leak(wrapper);
        return Status::Error.to_i32();
    }

    // Drop the wrapper (and inner Arc)
    drop(wrapper);
    unsafe { *device_handle = ptr::null_mut() };

    Status::Ok.to_i32()
}

/// Get device option
///
/// # Safety
/// - All pointers must be valid
#[no_mangle]
pub unsafe extern "C" fn ncDeviceGetOption(
    device_handle: *mut ncDeviceHandle_t,
    _option: c_int,
    _data: *mut c_void,
    data_length: *mut c_uint,
) -> c_int {
    if data_length.is_null() {
        return Status::InvalidParameters.to_i32();
    }

    match unsafe { c_to_device(device_handle) } {
        Ok(_dev) => {
            // TODO: Implement actual option getting
            // For now, return not implemented
            Status::UnsupportedFeature.to_i32()
        }
        Err(e) => e.to_status().to_i32(),
    }
}

// ========== Graph Functions ==========

/// Create a graph handle
///
/// # Safety
/// - name must be a valid null-terminated C string
/// - graph_handle must be a valid pointer
#[no_mangle]
pub unsafe extern "C" fn ncGraphCreate(
    name: *const c_char,
    graph_handle: *mut *mut ncGraphHandle_t,
) -> c_int {
    if name.is_null() || graph_handle.is_null() {
        return Status::InvalidParameters.to_i32();
    }

    let name_str = match unsafe { CStr::from_ptr(name) }.to_str() {
        Ok(s) => s,
        Err(_) => return Status::InvalidParameters.to_i32(),
    };

    match Graph::create(name_str) {
        Ok(graph) => {
            unsafe { *graph_handle = graph_to_c(Arc::new(RwLock::new(graph))) };
            Status::Ok.to_i32()
        }
        Err(e) => e.to_status().to_i32(),
    }
}

/// Allocate graph
///
/// # Safety
/// - All handles must be valid
/// - graph_buffer must point to valid memory of graph_buffer_length bytes
#[no_mangle]
pub unsafe extern "C" fn ncGraphAllocate(
    device_handle: *mut ncDeviceHandle_t,
    graph_handle: *mut ncGraphHandle_t,
    graph_buffer: *const c_void,
    graph_buffer_length: c_uint,
) -> c_int {
    if graph_buffer.is_null() || graph_buffer_length == 0 {
        return Status::InvalidParameters.to_i32();
    }

    match (unsafe { c_to_device(device_handle) }, unsafe {
        c_to_graph(graph_handle)
    }) {
        (Ok(device), Ok(graph)) => {
            let buffer = unsafe {
                std::slice::from_raw_parts(graph_buffer as *const u8, graph_buffer_length as usize)
            };

            match graph.write().allocate(device, buffer) {
                Ok(()) => Status::Ok.to_i32(),
                Err(e) => e.to_status().to_i32(),
            }
        }
        (Err(e), _) | (_, Err(e)) => e.to_status().to_i32(),
    }
}

/// Allocate graph with FIFOs
///
/// # Safety
/// - All pointers must be valid
#[no_mangle]
pub unsafe extern "C" fn ncGraphAllocateWithFifos(
    device_handle: *mut ncDeviceHandle_t,
    graph_handle: *mut ncGraphHandle_t,
    graph_buffer: *const c_void,
    graph_buffer_length: c_uint,
    in_fifo_handle: *mut *mut ncFifoHandle_t,
    out_fifo_handle: *mut *mut ncFifoHandle_t,
) -> c_int {
    if graph_buffer.is_null() || in_fifo_handle.is_null() || out_fifo_handle.is_null() {
        return Status::InvalidParameters.to_i32();
    }

    match (unsafe { c_to_device(device_handle) }, unsafe {
        c_to_graph(graph_handle)
    }) {
        (Ok(device), Ok(graph)) => {
            let buffer = unsafe {
                std::slice::from_raw_parts(graph_buffer as *const u8, graph_buffer_length as usize)
            };

            match graph.write().allocate_with_fifos(device, buffer) {
                Ok((in_fifo, out_fifo)) => {
                    unsafe {
                        *in_fifo_handle = fifo_to_c(in_fifo);
                        *out_fifo_handle = fifo_to_c(out_fifo);
                    }
                    Status::Ok.to_i32()
                }
                Err(e) => e.to_status().to_i32(),
            }
        }
        (Err(e), _) | (_, Err(e)) => e.to_status().to_i32(),
    }
}

/// Destroy graph
///
/// # Safety
/// - graph_handle must be a valid pointer to a handle
#[no_mangle]
pub unsafe extern "C" fn ncGraphDestroy(graph_handle: *mut *mut ncGraphHandle_t) -> c_int {
    if graph_handle.is_null() || unsafe { (*graph_handle).is_null() } {
        return Status::InvalidHandle.to_i32();
    }

    let ptr = unsafe { *graph_handle as *mut GraphHandleWrapper };
    let wrapper = unsafe { Box::from_raw(ptr) };
    drop(wrapper);
    unsafe { *graph_handle = ptr::null_mut() };

    Status::Ok.to_i32()
}

// ========== FIFO Functions ==========

/// Create FIFO
///
/// # Safety
/// - name must be valid C string
/// - fifo_handle must be valid pointer
#[no_mangle]
pub unsafe extern "C" fn ncFifoCreate(
    name: *const c_char,
    fifo_type: c_int,
    fifo_handle: *mut *mut ncFifoHandle_t,
) -> c_int {
    if name.is_null() || fifo_handle.is_null() {
        return Status::InvalidParameters.to_i32();
    }

    let name_str = match unsafe { CStr::from_ptr(name) }.to_str() {
        Ok(s) => s,
        Err(_) => return Status::InvalidParameters.to_i32(),
    };

    let ftype = match fifo_type {
        0 => FifoType::HostRo,
        1 => FifoType::HostWo,
        _ => return Status::InvalidParameters.to_i32(),
    };

    match Fifo::create(name_str, ftype) {
        Ok(fifo) => {
            unsafe { *fifo_handle = fifo_to_c(Arc::new(RwLock::new(fifo))) };
            Status::Ok.to_i32()
        }
        Err(e) => e.to_status().to_i32(),
    }
}

/// Write element to FIFO
///
/// # Safety
/// - All pointers must be valid
/// - input_tensor must point to valid memory
#[no_mangle]
pub unsafe extern "C" fn ncFifoWriteElem(
    fifo_handle: *mut ncFifoHandle_t,
    input_tensor: *const c_void,
    input_tensor_length: *mut c_uint,
    user_param: *mut c_void,
) -> c_int {
    if input_tensor.is_null() || input_tensor_length.is_null() {
        return Status::InvalidParameters.to_i32();
    }

    match unsafe { c_to_fifo(fifo_handle) } {
        Ok(fifo) => {
            let len = unsafe { *input_tensor_length as usize };
            let data = unsafe { std::slice::from_raw_parts(input_tensor as *const u8, len) };
            let user_data = if user_param.is_null() {
                None
            } else {
                Some(user_param as usize)
            };

            match fifo.read().write_elem(data, user_data) {
                Ok(()) => Status::Ok.to_i32(),
                Err(e) => e.to_status().to_i32(),
            }
        }
        Err(e) => e.to_status().to_i32(),
    }
}

/// Read element from FIFO
///
/// # Safety
/// - All pointers must be valid
/// - output_data must have space for the full element
#[no_mangle]
pub unsafe extern "C" fn ncFifoReadElem(
    fifo_handle: *mut ncFifoHandle_t,
    output_data: *mut c_void,
    output_data_len: *mut c_uint,
    user_param: *mut *mut c_void,
) -> c_int {
    if output_data.is_null() || output_data_len.is_null() {
        return Status::InvalidParameters.to_i32();
    }

    match unsafe { c_to_fifo(fifo_handle) } {
        Ok(fifo) => match fifo.read().read_elem() {
            Ok((data, user_data)) => {
                let out_len = unsafe { *output_data_len as usize };
                if data.len() > out_len {
                    unsafe { *output_data_len = data.len() as c_uint };
                    return Status::InvalidDataLength.to_i32();
                }

                unsafe {
                    ptr::copy_nonoverlapping(data.as_ptr(), output_data as *mut u8, data.len());
                    *output_data_len = data.len() as c_uint;

                    if !user_param.is_null() {
                        *user_param = user_data.unwrap_or(0) as *mut c_void;
                    }
                }

                Status::Ok.to_i32()
            }
            Err(e) => e.to_status().to_i32(),
        },
        Err(e) => e.to_status().to_i32(),
    }
}

/// Destroy FIFO
///
/// # Safety
/// - fifo_handle must be valid pointer to handle
#[no_mangle]
pub unsafe extern "C" fn ncFifoDestroy(fifo_handle: *mut *mut ncFifoHandle_t) -> c_int {
    if fifo_handle.is_null() || unsafe { (*fifo_handle).is_null() } {
        return Status::InvalidHandle.to_i32();
    }

    let ptr = unsafe { *fifo_handle as *mut FifoHandleWrapper };
    let wrapper = unsafe { Box::from_raw(ptr) };
    drop(wrapper);
    unsafe { *fifo_handle = ptr::null_mut() };

    Status::Ok.to_i32()
}

// ========== Global Functions ==========

/// Get global option
///
/// # Safety
/// - All pointers must be valid
#[no_mangle]
pub unsafe extern "C" fn ncGlobalGetOption(
    option: c_int,
    data: *mut c_void,
    data_length: *mut c_uint,
) -> c_int {
    if data_length.is_null() {
        return Status::InvalidParameters.to_i32();
    }

    let global_option = match option {
        0 => GlobalOption::LogLevel,
        1 => GlobalOption::ApiVersion,
        _ => return Status::InvalidParameters.to_i32(),
    };

    match global::global_get_option(global_option) {
        Ok(value) => {
            let req_len = value.len();
            if data.is_null() {
                unsafe { *data_length = req_len as c_uint };
                return Status::Ok.to_i32();
            }

            let out_len = unsafe { *data_length as usize };
            if req_len > out_len {
                unsafe { *data_length = req_len as c_uint };
                return Status::InvalidDataLength.to_i32();
            }

            unsafe {
                ptr::copy_nonoverlapping(value.as_ptr(), data as *mut u8, req_len);
                *data_length = req_len as c_uint;
            }
            Status::Ok.to_i32()
        }
        Err(e) => e.to_status().to_i32(),
    }
}

/// Set global option
///
/// # Safety
/// - All pointers must be valid
#[no_mangle]
pub unsafe extern "C" fn ncGlobalSetOption(
    option: c_int,
    data: *const c_void,
    data_length: c_uint,
) -> c_int {
    if data.is_null() || data_length == 0 {
        return Status::InvalidParameters.to_i32();
    }

    let global_option = match option {
        0 => GlobalOption::LogLevel,
        1 => GlobalOption::ApiVersion,
        _ => return Status::InvalidParameters.to_i32(),
    };

    let value = unsafe { std::slice::from_raw_parts(data as *const u8, data_length as usize) };

    match global::global_set_option(global_option, value) {
        Ok(()) => Status::Ok.to_i32(),
        Err(e) => e.to_status().to_i32(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    #[ignore = "Requires actual hardware device"]
    fn test_handle_conversions() {
        let device = Device::create(0).unwrap();
        let c_handle = device_to_c(device.clone());

        unsafe {
            let back = c_to_device(c_handle).unwrap();
            assert!(Arc::ptr_eq(&device, &back));

            // Cleanup
            let mut handle_ptr = c_handle;
            ncDeviceDestroy(&mut handle_ptr as *mut *mut _);
        }
    }
}
