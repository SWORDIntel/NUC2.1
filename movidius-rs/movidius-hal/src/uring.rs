//! High-performance io_uring submission and completion

use super::{Error, Result};
use io_uring::{opcode, IoUring};
use std::os::unix::io::RawFd;

/// io_uring submitter for maximum performance
pub struct UringSubmitter {
    ring: IoUring,
}

impl UringSubmitter {
    /// Create new submitter with specified queue depth
    pub fn new(entries: usize) -> Result<Self> {
        let ring = IoUring::new(entries as u32)?;
        Ok(Self { ring })
    }

    /// Submit inference request (placeholder - actual implementation would use proper ops)
    #[inline]
    pub fn submit_inference(&mut self, _fd: RawFd, _cmd_op: u32, user_data: u64) -> Result<()> {
        // Use NOP for now as UringCmd is not in this version
        let entry = opcode::Nop::new().build().user_data(user_data);

        unsafe {
            self.ring
                .submission()
                .push(&entry)
                .map_err(|_| Error::Uring("Queue full".to_string()))?;
        }

        self.ring.submit()?;
        Ok(())
    }

    /// Poll for completions (non-blocking)
    #[inline]
    pub fn poll_completion(&mut self) -> Result<Option<(u64, i32)>> {
        if let Some(cqe) = self.ring.completion().next() {
            Ok(Some((cqe.user_data(), cqe.result())))
        } else {
            Ok(None)
        }
    }

    /// Wait for completion (blocking)
    #[inline]
    pub fn wait_completion(&mut self) -> Result<(u64, i32)> {
        let cqe = self
            .ring
            .completion()
            .next()
            .ok_or_else(|| Error::Uring("No completion".to_string()))?;
        Ok((cqe.user_data(), cqe.result()))
    }

    /// Batch submit
    #[inline]
    pub fn submit_batch(&mut self) -> Result<usize> {
        Ok(self.ring.submit()?)
    }
}
