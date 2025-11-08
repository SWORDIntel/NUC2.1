//! Tensor descriptor with zero-copy and cache-alignment optimizations

use crate::fifo::FifoDataType;
use bytemuck::{Pod, Zeroable};
use crossbeam_utils::CachePadded;

/// Tensor descriptor describing shape and layout
///
/// This structure is cache-aligned and designed for zero-copy operations.
/// It's compatible with NCAPI v2's ncTensorDescriptor_t.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Pod, Zeroable)]
#[repr(C, align(64))] // Cache-line aligned for performance
pub struct TensorDescriptor {
    /// Batch count (currently always 1)
    pub n: u32,
    /// Channels per pixel
    pub c: u32,
    /// Width in pixels (1 for non-images)
    pub w: u32,
    /// Height in pixels
    pub h: u32,
    /// Total size in bytes
    pub total_size: u32,
    /// Channel stride in bytes
    pub c_stride: u32,
    /// Horizontal stride in bytes
    pub w_stride: u32,
    /// Vertical stride in bytes
    pub h_stride: u32,
    /// Data type
    pub data_type: FifoDataType,
    /// Padding to cache line (64 bytes total)
    _padding: [u8; 28],
}

unsafe impl Send for TensorDescriptor {}
unsafe impl Sync for TensorDescriptor {}

impl Default for TensorDescriptor {
    #[inline]
    fn default() -> Self {
        Self::new(1, 1, 1, 1, FifoDataType::FP32)
    }
}

impl TensorDescriptor {
    /// Create a new tensor descriptor with overflow checking
    #[inline]
    pub fn new(n: u32, c: u32, h: u32, w: u32, data_type: FifoDataType) -> Self {
        // Safety checks for overflow
        debug_assert!(n > 0, "Batch size must be > 0");
        debug_assert!(c > 0, "Channels must be > 0");
        debug_assert!(h > 0, "Height must be > 0");
        debug_assert!(w > 0, "Width must be > 0");

        let elem_size = data_type.size_bytes();
        let w_stride = elem_size;

        // Check for overflow in stride calculations
        let h_stride = match w.checked_mul(w_stride) {
            Some(v) => v,
            None => panic!("Overflow in h_stride calculation"),
        };

        let c_stride = match h.checked_mul(h_stride) {
            Some(v) => v,
            None => panic!("Overflow in c_stride calculation"),
        };

        // Check for overflow in total size calculation
        let total_size = match n
            .checked_mul(c)
            .and_then(|v| v.checked_mul(h))
            .and_then(|v| v.checked_mul(w))
            .and_then(|v| v.checked_mul(elem_size))
        {
            Some(v) => v,
            None => panic!("Overflow in total_size calculation"),
        };

        Self {
            n,
            c,
            w,
            h,
            total_size,
            c_stride,
            w_stride,
            h_stride,
            data_type,
            _padding: [0; 28],
        }
    }

    /// Create for an image tensor
    #[inline]
    pub fn image(height: u32, width: u32, channels: u32, data_type: FifoDataType) -> Self {
        Self::new(1, channels, height, width, data_type)
    }

    /// Create for a 1D vector
    #[inline]
    pub fn vector(length: u32, data_type: FifoDataType) -> Self {
        Self::new(1, 1, 1, length, data_type)
    }

    /// Validate buffer size matches descriptor
    #[inline]
    pub const fn validate_buffer_size(&self, buffer_len: usize) -> bool {
        buffer_len == self.total_size as usize
    }

    /// Get element count
    #[inline]
    pub const fn element_count(&self) -> usize {
        (self.n * self.c * self.h * self.w) as usize
    }

    /// Get size in bytes for specified data type
    #[inline]
    pub const fn size_for_type(&self, data_type: FifoDataType) -> u32 {
        self.element_count() as u32 * data_type.size_bytes()
    }

    /// Check if tensor is contiguous
    #[inline]
    pub const fn is_contiguous(&self) -> bool {
        let elem_size = self.data_type.size_bytes();
        self.w_stride == elem_size
            && self.h_stride == self.w * elem_size
            && self.c_stride == self.h * self.h_stride
    }
}

/// Cache-padded tensor descriptor for hot-path usage
pub type CachePaddedTensorDescriptor = CachePadded<TensorDescriptor>;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_tensor_descriptor_size() {
        // Ensure cache-line alignment
        assert_eq!(std::mem::size_of::<TensorDescriptor>(), 64);
        assert_eq!(std::mem::align_of::<TensorDescriptor>(), 64);
    }

    #[test]
    fn test_tensor_descriptor_image() {
        let desc = TensorDescriptor::image(224, 224, 3, FifoDataType::FP32);
        assert_eq!(desc.n, 1);
        assert_eq!(desc.c, 3);
        assert_eq!(desc.h, 224);
        assert_eq!(desc.w, 224);
        assert_eq!(desc.element_count(), 224 * 224 * 3);
        assert_eq!(desc.total_size, 224 * 224 * 3 * 4);
        assert!(desc.is_contiguous());
    }

    #[test]
    fn test_tensor_descriptor_vector() {
        let desc = TensorDescriptor::vector(1000, FifoDataType::FP16);
        assert_eq!(desc.element_count(), 1000);
        assert_eq!(desc.total_size, 1000 * 2);
    }

    #[test]
    fn test_validate_buffer() {
        let desc = TensorDescriptor::vector(100, FifoDataType::FP32);
        assert!(desc.validate_buffer_size(400));
        assert!(!desc.validate_buffer_size(399));
        assert!(!desc.validate_buffer_size(401));
    }

    #[test]
    fn test_pod_safety() {
        let desc = TensorDescriptor::default();
        let bytes = bytemuck::bytes_of(&desc);
        let reconstructed: &TensorDescriptor = bytemuck::from_bytes(bytes);
        assert_eq!(*reconstructed, desc);
    }
}
