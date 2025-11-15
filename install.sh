#!/bin/bash
# Movidius Myriad X VPU Driver - Dynamic Installer v2.2
# Automatically detects kernel capabilities and builds with optimal configuration

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Usage function
usage() {
    echo "Usage: $0 [install|uninstall|systemd]"
    echo ""
    echo "Commands:"
    echo "  (none)     - Build only (default)"
    echo "  install    - Build and install system-wide"
    echo "  uninstall  - Remove installed modules and files"
    echo "  systemd    - Install systemd service for auto-loading"
    echo ""
    exit 1
}

# Uninstall function
uninstall_driver() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}  Movidius Driver Uninstaller${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo ""

    if [[ $EUID -ne 0 ]]; then
        echo -e "${RED}✗ Error: Uninstallation requires root privileges${NC}"
        echo -e "  Run: sudo $0 uninstall"
        exit 1
    fi

    echo -e "${BLUE}[1/5] Unloading Kernel Modules${NC}"
    # Unload modules if loaded
    if lsmod | grep -q movidius_x_vpu; then
        rmmod movidius_x_vpu 2>/dev/null || true
        echo -e "  ${GREEN}✓ Unloaded movidius_x_vpu${NC}"
    else
        echo -e "  ${YELLOW}⚠ Module not loaded${NC}"
    fi

    if lsmod | grep -q vfio_movidius; then
        rmmod vfio_movidius 2>/dev/null || true
        echo -e "  ${GREEN}✓ Unloaded vfio_movidius${NC}"
    fi

    echo ""
    echo -e "${BLUE}[2/5] Removing Kernel Modules${NC}"
    KERNEL_VERSION=$(uname -r)
    rm -f "/lib/modules/${KERNEL_VERSION}/extra/movidius_x_vpu.ko"
    rm -f "/lib/modules/${KERNEL_VERSION}/extra/vfio_movidius.ko"
    depmod -a
    echo -e "  ${GREEN}✓ Removed kernel modules${NC}"

    echo ""
    echo -e "${BLUE}[3/5] Removing Benchmark Tool${NC}"
    if [ -f "/usr/local/bin/movidius-bench" ]; then
        rm -f /usr/local/bin/movidius-bench
        echo -e "  ${GREEN}✓ Removed movidius-bench${NC}"
    else
        echo -e "  ${YELLOW}⚠ movidius-bench not found${NC}"
    fi

    echo ""
    echo -e "${BLUE}[4/5] Removing Udev Rules${NC}"
    if [ -f "/etc/udev/rules.d/99-movidius.rules" ]; then
        rm -f /etc/udev/rules.d/99-movidius.rules
        udevadm control --reload-rules 2>/dev/null || true
        echo -e "  ${GREEN}✓ Removed udev rules${NC}"
    else
        echo -e "  ${YELLOW}⚠ Udev rules not found${NC}"
    fi

    echo ""
    echo -e "${BLUE}[5/5] Removing Systemd Service${NC}"
    if [ -f "/etc/systemd/system/movidius-vpu.service" ]; then
        systemctl stop movidius-vpu 2>/dev/null || true
        systemctl disable movidius-vpu 2>/dev/null || true
        rm -f /etc/systemd/system/movidius-vpu.service
        systemctl daemon-reload
        echo -e "  ${GREEN}✓ Removed systemd service${NC}"
    else
        echo -e "  ${YELLOW}⚠ Systemd service not found${NC}"
    fi

    echo ""
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}  Uninstallation Complete!${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo ""
    exit 0
}

# Systemd service installation
install_systemd_service() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}  Systemd Service Installer${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo ""

    if [[ $EUID -ne 0 ]]; then
        echo -e "${RED}✗ Error: Systemd installation requires root privileges${NC}"
        echo -e "  Run: sudo $0 systemd"
        exit 1
    fi

    echo -e "${BLUE}Creating systemd service...${NC}"

    cat > /etc/systemd/system/movidius-vpu.service << 'EOF'
[Unit]
Description=Movidius Myriad X VPU Driver
After=multi-user.target
DefaultDependencies=no

[Service]
Type=oneshot
ExecStart=/sbin/modprobe movidius_x_vpu
ExecStop=/sbin/rmmod movidius_x_vpu
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF

    systemctl daemon-reload
    systemctl enable movidius-vpu

    echo -e "  ${GREEN}✓ Created /etc/systemd/system/movidius-vpu.service${NC}"
    echo -e "  ${GREEN}✓ Enabled movidius-vpu service${NC}"
    echo ""
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}  Systemd Service Installed!${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo ""
    echo -e "The driver will now load automatically on boot."
    echo -e ""
    echo -e "Commands:"
    echo -e "  Start:   ${BLUE}sudo systemctl start movidius-vpu${NC}"
    echo -e "  Stop:    ${BLUE}sudo systemctl stop movidius-vpu${NC}"
    echo -e "  Status:  ${BLUE}sudo systemctl status movidius-vpu${NC}"
    echo -e "  Disable: ${BLUE}sudo systemctl disable movidius-vpu${NC}"
    echo ""
    exit 0
}

# Handle command-line arguments
if [[ "${1}" == "uninstall" ]]; then
    uninstall_driver
elif [[ "${1}" == "systemd" ]]; then
    install_systemd_service
elif [[ "${1}" == "help" ]] || [[ "${1}" == "--help" ]] || [[ "${1}" == "-h" ]]; then
    usage
fi

# Banner
echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}  Movidius Myriad X VPU Driver Installer${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""

# Check if running as root for installation
if [[ "${1}" == "install" ]] && [[ $EUID -ne 0 ]]; then
   echo -e "${RED}✗ Error: Installation requires root privileges${NC}"
   echo -e "  Run: sudo $0 install"
   exit 1
fi

# Detect kernel version
KERNEL_VERSION=$(uname -r)
KERNEL_MAJOR=$(echo "$KERNEL_VERSION" | cut -d. -f1)
KERNEL_MINOR=$(echo "$KERNEL_VERSION" | cut -d. -f2)

echo -e "${BLUE}[1/8] System Detection${NC}"
echo "  Kernel Version:    $KERNEL_VERSION"
echo "  Kernel Major.Minor: ${KERNEL_MAJOR}.${KERNEL_MINOR}"

# Check if kernel supports io_uring_cmd (requires >= 6.2)
ENABLE_IO_URING=0
if [[ $KERNEL_MAJOR -gt 6 ]] || [[ $KERNEL_MAJOR -eq 6 && $KERNEL_MINOR -ge 2 ]]; then
    # Check if CONFIG_IO_URING is enabled
    if [ -f "/boot/config-${KERNEL_VERSION}" ]; then
        if grep -q "CONFIG_IO_URING=y" "/boot/config-${KERNEL_VERSION}"; then
            ENABLE_IO_URING=1
            echo -e "  ${GREEN}✓ CONFIG_IO_URING enabled${NC}"
        else
            echo -e "  ${YELLOW}⚠ CONFIG_IO_URING not found in kernel config${NC}"
        fi
    else
        # Try alternative config locations
        if [ -f "/proc/config.gz" ]; then
            if zcat /proc/config.gz | grep -q "CONFIG_IO_URING=y"; then
                ENABLE_IO_URING=1
                echo -e "  ${GREEN}✓ CONFIG_IO_URING enabled${NC}"
            fi
        else
            # Assume enabled if headers present and kernel is new enough
            if [ -f "/usr/src/linux-headers-${KERNEL_VERSION}/include/linux/io_uring.h" ] || \
               [ -f "/lib/modules/${KERNEL_VERSION}/build/include/linux/io_uring.h" ]; then
                ENABLE_IO_URING=1
                echo -e "  ${GREEN}✓ io_uring headers detected${NC}"
            else
                echo -e "  ${YELLOW}⚠ Cannot verify CONFIG_IO_URING (assuming enabled)${NC}"
                ENABLE_IO_URING=1
            fi
        fi
    fi
else
    echo -e "  ${YELLOW}⚠ Kernel < 6.2: io_uring_cmd not supported${NC}"
fi

if [ $ENABLE_IO_URING -eq 1 ]; then
    echo -e "  ${GREEN}✓ io_uring support: ENABLED${NC}"
else
    echo -e "  ${YELLOW}⚠ io_uring support: DISABLED (fallback to ioctl)${NC}"
fi

# Check for required tools
echo ""
echo -e "${BLUE}[2/8] Checking Dependencies${NC}"

# Function to prompt for package installation
prompt_install() {
    local package=$1
    local description=$2

    if [[ $EUID -eq 0 ]]; then
        # Running as root, can auto-install
        read -p "  Install $description now? (y/n): " -n 1 -r
        echo
        if [[ $REPLY =~ ^[Yy]$ ]]; then
            # Detect package manager
            if command -v apt-get &> /dev/null; then
                apt-get update -qq && apt-get install -y $package
            elif command -v dnf &> /dev/null; then
                dnf install -y $package
            elif command -v yum &> /dev/null; then
                yum install -y $package
            else
                echo -e "  ${RED}✗ Unable to detect package manager${NC}"
                return 1
            fi
            return 0
        else
            return 1
        fi
    else
        # Not root, show manual install command
        return 1
    fi
}

# Check for kernel headers
if [ ! -d "/lib/modules/${KERNEL_VERSION}/build" ]; then
    echo -e "  ${RED}✗ Kernel headers not found${NC}"
    if ! prompt_install "linux-headers-${KERNEL_VERSION}" "kernel headers"; then
        echo -e "  ${YELLOW}  Manual install: sudo apt-get install linux-headers-${KERNEL_VERSION}${NC}"
        exit 1
    fi
fi
echo -e "  ${GREEN}✓ Kernel headers found${NC}"

# Check for build tools
NEED_BUILD_ESSENTIAL=0
if ! command -v make &> /dev/null; then
    echo -e "  ${RED}✗ make not found${NC}"
    NEED_BUILD_ESSENTIAL=1
fi

if ! command -v gcc &> /dev/null; then
    echo -e "  ${RED}✗ gcc not found${NC}"
    NEED_BUILD_ESSENTIAL=1
fi

if [ $NEED_BUILD_ESSENTIAL -eq 1 ]; then
    if ! prompt_install "build-essential" "build tools (gcc, make, etc.)"; then
        echo -e "  ${YELLOW}  Manual install: sudo apt-get install build-essential${NC}"
        exit 1
    fi
fi

if command -v make &> /dev/null; then
    echo -e "  ${GREEN}✓ make found${NC}"
fi

if command -v gcc &> /dev/null; then
    echo -e "  ${GREEN}✓ gcc found${NC}"
fi

# Check for liburing (only required if io_uring is enabled)
LIBURING_FOUND=0
if [ $ENABLE_IO_URING -eq 1 ]; then
    if pkg-config --exists liburing 2>/dev/null; then
        LIBURING_VERSION=$(pkg-config --modversion liburing)
        echo -e "  ${GREEN}✓ liburing found (version ${LIBURING_VERSION})${NC}"
        LIBURING_FOUND=1
    elif [ -f "/usr/include/liburing.h" ] || [ -f "/usr/local/include/liburing.h" ]; then
        echo -e "  ${GREEN}✓ liburing headers found${NC}"
        LIBURING_FOUND=1
    else
        echo -e "  ${YELLOW}⚠ liburing not found (required for full io_uring support)${NC}"
        if prompt_install "liburing-dev" "liburing (for io_uring benchmark tool)"; then
            LIBURING_FOUND=1
            echo -e "  ${GREEN}✓ liburing installed${NC}"
        else
            echo -e "  ${YELLOW}  Manual install: sudo apt-get install liburing-dev${NC}"
            echo -e "  ${YELLOW}  Continuing without benchmark tool...${NC}"
        fi
    fi
fi

# Clean previous build
echo ""
echo -e "${BLUE}[3/8] Cleaning Previous Build${NC}"
make clean > /dev/null 2>&1 || true
echo -e "  ${GREEN}✓ Clean complete${NC}"

# Build kernel modules
echo ""
echo -e "${BLUE}[4/8] Building Kernel Modules${NC}"
echo "  Building with ENABLE_IO_URING=${ENABLE_IO_URING}..."

if make ENABLE_IO_URING=${ENABLE_IO_URING} -j$(nproc) 2>&1 | tee /tmp/movidius_build.log; then
    echo -e "  ${GREEN}✓ Kernel modules built successfully${NC}"
else
    echo -e "  ${RED}✗ Build failed${NC}"
    echo -e "  Check /tmp/movidius_build.log for details"
    exit 1
fi

# Build benchmark tool
echo ""
echo -e "${BLUE}[5/8] Building Benchmark Tool${NC}"
if [ $ENABLE_IO_URING -eq 1 ] && [ $LIBURING_FOUND -eq 1 ]; then
    if make bench 2>&1 | tee -a /tmp/movidius_build.log; then
        echo -e "  ${GREEN}✓ movidius-bench built successfully${NC}"
    else
        echo -e "  ${YELLOW}⚠ Failed to build benchmark tool${NC}"
        echo -e "  ${YELLOW}  Modules will still work, but benchmarking unavailable${NC}"
    fi
else
    if [ $ENABLE_IO_URING -eq 0 ]; then
        echo -e "  ${YELLOW}⚠ Skipped (io_uring disabled)${NC}"
    else
        echo -e "  ${YELLOW}⚠ Skipped (liburing not available)${NC}"
    fi
fi

# Verify build artifacts
echo ""
echo -e "${BLUE}[6/8] Verifying Build Artifacts${NC}"

if [ -f "movidius_x_vpu.ko" ]; then
    SIZE=$(stat -c%s movidius_x_vpu.ko)
    echo -e "  ${GREEN}✓ movidius_x_vpu.ko (${SIZE} bytes)${NC}"
else
    echo -e "  ${RED}✗ movidius_x_vpu.ko not found${NC}"
    exit 1
fi

if [ -f "vfio_movidius.ko" ]; then
    SIZE=$(stat -c%s vfio_movidius.ko)
    echo -e "  ${GREEN}✓ vfio_movidius.ko (${SIZE} bytes)${NC}"
else
    echo -e "  ${YELLOW}⚠ vfio_movidius.ko not found${NC}"
fi

if [ -f "movidius-bench" ]; then
    SIZE=$(stat -c%s movidius-bench)
    echo -e "  ${GREEN}✓ movidius-bench (${SIZE} bytes)${NC}"
fi

# Installation
if [[ "${1}" == "install" ]]; then
    echo ""
    echo -e "${BLUE}[7/8] Installing Kernel Modules${NC}"

    # Install modules
    if make install 2>&1 | tee -a /tmp/movidius_build.log; then
        echo -e "  ${GREEN}✓ Modules installed to /lib/modules/${KERNEL_VERSION}/extra/${NC}"
    else
        echo -e "  ${RED}✗ Installation failed${NC}"
        exit 1
    fi

    # Create udev rule for device permissions
    echo ""
    echo -e "${BLUE}[8/8] Configuring Device Permissions${NC}"
    cat > /etc/udev/rules.d/99-movidius.rules << 'EOF'
# Movidius Myriad X VPU device permissions
SUBSYSTEM=="movidius_x_vpu", MODE="0666"
KERNEL=="movidius_x_vpu*", MODE="0666"
EOF
    echo -e "  ${GREEN}✓ Created /etc/udev/rules.d/99-movidius.rules${NC}"

    # Reload udev rules
    udevadm control --reload-rules 2>/dev/null || true
    echo -e "  ${GREEN}✓ Reloaded udev rules${NC}"

    # Install benchmark tool
    if [ -f "movidius-bench" ]; then
        cp movidius-bench /usr/local/bin/
        chmod +x /usr/local/bin/movidius-bench
        echo -e "  ${GREEN}✓ Installed movidius-bench to /usr/local/bin/${NC}"
    fi

    echo ""
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}  Installation Complete!${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo ""
    echo -e "Next steps:"
    echo -e "  1. Load the module:  ${BLUE}sudo modprobe movidius_x_vpu${NC}"
    echo -e "  2. Check devices:    ${BLUE}ls -la /dev/movidius*${NC}"
    echo -e "  3. View stats:       ${BLUE}cat /sys/class/movidius_x_vpu/*/movidius/*${NC}"
    if [ -f "/usr/local/bin/movidius-bench" ]; then
        echo -e "  4. Run benchmark:    ${BLUE}movidius-bench${NC}"
    fi
    echo ""
else
    echo ""
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}  Build Complete!${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo ""
    echo -e "To install the modules, run:"
    echo -e "  ${BLUE}sudo $0 install${NC}"
    echo ""
    echo -e "Or load modules directly:"
    echo -e "  ${BLUE}sudo insmod movidius_x_vpu.ko${NC}"
    echo ""
fi

# Summary
echo -e "${BLUE}Build Configuration Summary:${NC}"
echo -e "  io_uring:          $([ $ENABLE_IO_URING -eq 1 ] && echo -e '${GREEN}ENABLED${NC}' || echo -e '${YELLOW}DISABLED${NC}')"
echo -e "  Kernel Version:    ${KERNEL_VERSION}"
echo -e "  Build Log:         /tmp/movidius_build.log"
echo ""
