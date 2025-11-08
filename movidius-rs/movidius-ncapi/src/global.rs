//! Global options and state

use crate::error::Result;
use crate::status::Status;
use crate::types::{GlobalOption, LogLevel};
use parking_lot::RwLock;
use std::sync::OnceLock;

static GLOBAL_STATE: OnceLock<RwLock<GlobalState>> = OnceLock::new();

#[derive(Default)]
struct GlobalState {
    #[allow(dead_code)]
    log_level: LogLevel,
}

fn global_state() -> &'static RwLock<GlobalState> {
    GLOBAL_STATE.get_or_init(|| RwLock::new(GlobalState::default()))
}

/// Gets a global option value.
///
/// Retrieves the current value of a global option, such as the log level or API version.
///
/// # Arguments
///
/// * `option` - The global option to retrieve
///
/// # Returns
///
/// Returns the option value as a byte vector. For `LogLevel`, this is a 4-byte little-endian
/// integer. For `ApiVersion`, this is a 16-byte array containing four 4-byte little-endian integers
/// representing the version components.
///
/// # Errors
///
/// Returns an error if the option cannot be retrieved.
pub fn global_get_option(option: GlobalOption) -> Result<Vec<u8>> {
    let state = global_state().read();
    match option {
        GlobalOption::LogLevel => Ok((state.log_level as i32).to_le_bytes().to_vec()),
        GlobalOption::ApiVersion => {
            let version = crate::VERSION;
            let mut bytes = Vec::with_capacity(16);
            bytes.extend_from_slice(&version.0.to_le_bytes());
            bytes.extend_from_slice(&version.1.to_le_bytes());
            bytes.extend_from_slice(&version.2.to_le_bytes());
            bytes.extend_from_slice(&version.3.to_le_bytes());
            Ok(bytes)
        }
    }
}

/// Sets a global option value.
///
/// Configures a global option, such as the log level. Some options like `ApiVersion` are
/// read-only and cannot be set.
///
/// # Arguments
///
/// * `option` - The global option to set
/// * `value` - The new value as a byte slice (must be a 4-byte little-endian integer for `LogLevel`)
///
/// # Returns
///
/// Returns `Ok(())` if the option was successfully set.
///
/// # Errors
///
/// Returns an error if:
/// - The value has an invalid format or length
/// - The option value is out of valid range (e.g., invalid log level)
/// - The option is read-only (e.g., attempting to set `ApiVersion`)
pub fn global_set_option(option: GlobalOption, value: &[u8]) -> Result<()> {
    let mut state = global_state().write();
    match option {
        GlobalOption::LogLevel => {
            let val = i32::from_le_bytes(
                value
                    .try_into()
                    .map_err(|_| crate::Error::Status(Status::InvalidParameters))?,
            );
            state.log_level = match val {
                0 => LogLevel::Fatal,
                1 => LogLevel::Error,
                2 => LogLevel::Warn,
                3 => LogLevel::Info,
                4 => LogLevel::Debug,
                _ => return Err(crate::Error::Status(Status::InvalidParameters)),
            };
            Ok(())
        }
        GlobalOption::ApiVersion => Err(crate::Error::Status(Status::Unauthorized)),
    }
}
