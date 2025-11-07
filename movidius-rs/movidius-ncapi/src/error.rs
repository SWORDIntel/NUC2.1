//! Error types and Result alias

use crate::status::Status;
use thiserror::Error;

/// Error type for NCAPI operations
#[derive(Error, Debug)]
pub enum Error {
    /// NCAPI status error
    #[error("NCAPI error: {0}")]
    Status(#[from] Status),

    /// IO error
    #[error("IO error: {0}")]
    Io(#[from] std::io::Error),

    /// Invalid state transition
    #[error("Invalid state transition: {0}")]
    InvalidState(String),

    /// Invalid argument
    #[error("Invalid argument: {0}")]
    InvalidArgument(String),

    /// Resource exhausted
    #[error("Resource exhausted: {0}")]
    ResourceExhausted(String),

    /// Hardware error
    #[error("Hardware error: {0}")]
    Hardware(String),

    /// Device error
    #[error("Device error: {0}")]
    DeviceError(String),

    /// Other error
    #[error("{0}")]
    Other(String),
}

impl Error {
    /// Convert to Status code
    pub fn to_status(&self) -> Status {
        match self {
            Self::Status(s) => *s,
            Self::Io(_) => Status::Error,
            Self::InvalidState(_) => Status::InvalidParameters,
            Self::InvalidArgument(_) => Status::InvalidParameters,
            Self::ResourceExhausted(_) => Status::OutOfMemory,
            Self::Hardware(_) => Status::MyriadError,
            Self::DeviceError(_) => Status::DeviceNotFound,
            Self::Other(_) => Status::Error,
        }
    }
}

/// Result type for NCAPI operations
pub type Result<T> = std::result::Result<T, Error>;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_error_conversion() {
        let err = Error::from(Status::DeviceNotFound);
        assert_eq!(err.to_status(), Status::DeviceNotFound);
    }
}
