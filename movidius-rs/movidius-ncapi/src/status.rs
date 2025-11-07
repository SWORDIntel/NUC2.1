//! Status codes compatible with NCAPI v2

use std::fmt;

/// NCAPI v2 compatible status codes
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[repr(i32)]
pub enum Status {
    /// Success
    Ok = 0,
    /// Device busy, retry later  
    Busy = -1,
    /// Unexpected error
    Error = -2,
    /// Host out of memory
    OutOfMemory = -3,
    /// No device found
    DeviceNotFound = -4,
    /// Invalid parameters
    InvalidParameters = -5,
    /// Communication timeout
    Timeout = -6,
    /// Boot file not found
    MvcmdNotFound = -7,
    /// Not allocated
    NotAllocated = -8,
    /// Unauthorized operation
    Unauthorized = -9,
    /// Unsupported graph file
    UnsupportedGraphFile = -10,
    /// Unsupported configuration file
    UnsupportedConfigurationFile = -11,
    /// Unsupported feature
    UnsupportedFeature = -12,
    /// VPU error
    MyriadError = -13,
    /// Invalid data length
    InvalidDataLength = -14,
    /// Invalid handle
    InvalidHandle = -15,
}

impl Status {
    /// Convert to i32 for C FFI
    #[inline]
    pub const fn to_i32(self) -> i32 {
        self as i32
    }

    /// Convert from i32 for C FFI
    #[inline]
    pub const fn from_i32(code: i32) -> Option<Self> {
        match code {
            0 => Some(Self::Ok),
            -1 => Some(Self::Busy),
            -2 => Some(Self::Error),
            -3 => Some(Self::OutOfMemory),
            -4 => Some(Self::DeviceNotFound),
            -5 => Some(Self::InvalidParameters),
            -6 => Some(Self::Timeout),
            -7 => Some(Self::MvcmdNotFound),
            -8 => Some(Self::NotAllocated),
            -9 => Some(Self::Unauthorized),
            -10 => Some(Self::UnsupportedGraphFile),
            -11 => Some(Self::UnsupportedConfigurationFile),
            -12 => Some(Self::UnsupportedFeature),
            -13 => Some(Self::MyriadError),
            -14 => Some(Self::InvalidDataLength),
            -15 => Some(Self::InvalidHandle),
            _ => None,
        }
    }

    /// Check if status represents success
    #[inline]
    pub const fn is_ok(self) -> bool {
        matches!(self, Self::Ok)
    }

    /// Check if status represents an error
    #[inline]
    pub const fn is_err(self) -> bool {
        !self.is_ok()
    }
}

impl fmt::Display for Status {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Ok => write!(f, "Success"),
            Self::Busy => write!(f, "Device busy"),
            Self::Error => write!(f, "Unexpected error"),
            Self::OutOfMemory => write!(f, "Out of memory"),
            Self::DeviceNotFound => write!(f, "Device not found"),
            Self::InvalidParameters => write!(f, "Invalid parameters"),
            Self::Timeout => write!(f, "Timeout"),
            Self::MvcmdNotFound => write!(f, "Boot file not found"),
            Self::NotAllocated => write!(f, "Not allocated"),
            Self::Unauthorized => write!(f, "Unauthorized"),
            Self::UnsupportedGraphFile => write!(f, "Unsupported graph file"),
            Self::UnsupportedConfigurationFile => write!(f, "Unsupported config file"),
            Self::UnsupportedFeature => write!(f, "Unsupported feature"),
            Self::MyriadError => write!(f, "VPU error"),
            Self::InvalidDataLength => write!(f, "Invalid data length"),
            Self::InvalidHandle => write!(f, "Invalid handle"),
        }
    }
}

impl std::error::Error for Status {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_status_roundtrip() {
        for code in -15..=0 {
            if let Some(status) = Status::from_i32(code) {
                assert_eq!(status.to_i32(), code);
            }
        }
    }

    #[test]
    fn test_status_is_ok() {
        assert!(Status::Ok.is_ok());
        assert!(!Status::Error.is_ok());
    }
}
