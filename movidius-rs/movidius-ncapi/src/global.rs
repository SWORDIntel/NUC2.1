//! Global options and state

use crate::error::Result;
use crate::status::Status;
use crate::types::{GlobalOption, LogLevel};
use parking_lot::RwLock;
use std::sync::OnceLock;

static GLOBAL_STATE: OnceLock<RwLock<GlobalState>> = OnceLock::new();

struct GlobalState {
    log_level: LogLevel,
}

impl Default for GlobalState {
    fn default() -> Self {
        Self {
            log_level: LogLevel::default(),
        }
    }
}

fn global_state() -> &'static RwLock<GlobalState> {
    GLOBAL_STATE.get_or_init(|| RwLock::new(GlobalState::default()))
}

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

pub fn global_set_option(option: GlobalOption, value: &[u8]) -> Result<()> {
    let mut state = global_state().write();
    match option {
        GlobalOption::LogLevel => {
            let val = i32::from_le_bytes(value.try_into().map_err(|_| {
                crate::Error::Status(Status::InvalidParameters)
            })?);
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
        GlobalOption::ApiVersion => {
            Err(crate::Error::Status(Status::Unauthorized))
        }
    }
}
