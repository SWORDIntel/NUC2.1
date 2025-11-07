//! USB protocol handling

/// USB protocol constants
pub mod protocol {
    /// USB request code for reading device registers
    pub const USB_REQ_READ_REGISTER: u8 = 0x10;
    /// USB request code for writing device registers
    pub const USB_REQ_WRITE_REGISTER: u8 = 0x11;
    /// USB request code for retrieving firmware version
    pub const USB_REQ_GET_FW_VERSION: u8 = 0x05;
}
