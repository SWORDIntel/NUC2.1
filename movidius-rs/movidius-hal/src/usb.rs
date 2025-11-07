//! USB protocol handling

/// USB protocol constants
pub mod protocol {
    pub const USB_REQ_READ_REGISTER: u8 = 0x10;
    pub const USB_REQ_WRITE_REGISTER: u8 = 0x11;
    pub const USB_REQ_GET_FW_VERSION: u8 = 0x05;
}
