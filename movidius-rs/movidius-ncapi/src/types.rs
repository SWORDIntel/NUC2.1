//! Common types and enumerations

/// Global options
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum GlobalOption {
    /// Logging level (read/write)
    LogLevel = 0,
    /// API version (read-only)
    ApiVersion = 1,
}

/// Logging levels
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
#[repr(i32)]
pub enum LogLevel {
    /// Fatal errors only
    Fatal = 0,
    /// Errors and above
    Error = 1,
    /// Warnings and above (default)
    Warn = 2,
    /// Info and above
    Info = 3,
    /// Debug and above (full verbosity)
    Debug = 4,
}

impl Default for LogLevel {
    fn default() -> Self {
        Self::Warn
    }
}

impl LogLevel {
    /// Convert to tracing level
    #[cfg(feature = "async")]
    pub fn to_tracing_level(self) -> tracing::Level {
        match self {
            Self::Fatal | Self::Error => tracing::Level::ERROR,
            Self::Warn => tracing::Level::WARN,
            Self::Info => tracing::Level::INFO,
            Self::Debug => tracing::Level::DEBUG,
        }
    }
}
