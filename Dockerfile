# Multi-stage Dockerfile for Movidius NUC2.1 compilation
# Builds both kernel driver and Rust components

# Stage 1: Kernel driver build environment
FROM ubuntu:22.04 AS kernel-builder

# Install kernel build dependencies
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential \
    linux-headers-generic \
    kmod \
    libelf-dev \
    bc \
    flex \
    bison \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Prepare kernel headers for module builds
# Use arch-specific headers (e.g., linux-headers-*-generic), NOT -common
RUN HEADERS_DIR=$(find /usr/src -maxdepth 1 -type d -name "linux-headers-*-generic" | head -n1) && \
    if [ -z "$HEADERS_DIR" ]; then \
        echo "ERROR: No arch-specific kernel headers found" && \
        ls -la /usr/src && \
        exit 1; \
    fi && \
    echo "Using headers: $HEADERS_DIR" && \
    cd "$HEADERS_DIR" && \
    if [ ! -f "include/config/auto.conf" ]; then \
        echo "Preparing kernel headers..." && \
        make oldconfig && \
        make modules_prepare; \
    fi && \
    KERNEL_VERSION=$(uname -r) && \
    mkdir -p /lib/modules/$KERNEL_VERSION && \
    ln -sf "$HEADERS_DIR" /lib/modules/$KERNEL_VERSION/build && \
    echo "Headers prepared at: $HEADERS_DIR"

# Copy kernel driver source
WORKDIR /build/kernel
COPY movidius_x_vpu.c .
COPY vfio_movidius.c .
COPY Makefile .

# Build kernel modules
RUN make clean && make

# Stage 2: Rust build environment
FROM rust:1.75-slim AS rust-builder

# Install system dependencies for Rust build
RUN apt-get update && apt-get install -y \
    pkg-config \
    libssl-dev \
    liburing-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy Rust workspace
WORKDIR /build/rust
COPY movidius-rs/Cargo.toml movidius-rs/Cargo.lock ./
COPY movidius-rs/movidius-ncapi ./movidius-ncapi/
COPY movidius-rs/movidius-hal ./movidius-hal/
COPY movidius-rs/movidius-bench ./movidius-bench/

# Build Rust components in release mode with optimizations
RUN cargo build --release

# Run tests
RUN cargo test --release

# Stage 3: Final image with artifacts
FROM ubuntu:22.04 AS final

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    kmod \
    usbutils \
    && rm -rf /var/lib/apt/lists/*

# Create directories
RUN mkdir -p /opt/movidius/kernel /opt/movidius/bin /opt/movidius/scripts

# Copy kernel modules from kernel-builder
COPY --from=kernel-builder /build/kernel/*.ko /opt/movidius/kernel/

# Copy Rust binaries from rust-builder
COPY --from=rust-builder /build/rust/target/release/movidius-bench /opt/movidius/bin/

# Copy scripts
COPY movidius-rs/scripts/*.sh /opt/movidius/scripts/
RUN chmod +x /opt/movidius/scripts/*.sh

# Set working directory
WORKDIR /opt/movidius

# Add info
RUN echo "Movidius NUC2.1 - Docker Build" > /opt/movidius/BUILD_INFO && \
    echo "Build Date: $(date)" >> /opt/movidius/BUILD_INFO && \
    echo "Kernel Modules: /opt/movidius/kernel/" >> /opt/movidius/BUILD_INFO && \
    echo "Binaries: /opt/movidius/bin/" >> /opt/movidius/BUILD_INFO && \
    echo "Scripts: /opt/movidius/scripts/" >> /opt/movidius/BUILD_INFO

# Display build info on container start
CMD ["cat", "/opt/movidius/BUILD_INFO"]

# Metadata
LABEL org.opencontainers.image.title="Movidius NUC2.1"
LABEL org.opencontainers.image.description="High-performance Movidius Myriad X VPU driver and Rust NCAPI"
LABEL org.opencontainers.image.version="2.1"
LABEL org.opencontainers.image.vendor="SWORDIntel"
