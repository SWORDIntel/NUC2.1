# Docker Build Guide

Docker-based compilation environment for Movidius NUC2.1 project. Builds both kernel driver and Rust components in isolated containers.

Supports both **Ubuntu 22.04** and **Debian Bookworm (12)** with kernel 6.1.x/6.17+ support.

## Quick Start

### Using Docker Compose (Recommended)

**Ubuntu Build:**
```bash
# Build and extract artifacts
docker-compose up builder

# Artifacts will be in ./artifacts/
ls -la artifacts/kernel/*.ko
ls -la artifacts/bin/movidius-bench
```

**Debian Build (6.1.x/6.17+ kernel):**
```bash
# Build with Debian Bookworm base
docker-compose up builder-debian

# Artifacts will be in ./artifacts-debian/
ls -la artifacts-debian/kernel/*.ko
ls -la artifacts-debian/bin/movidius-bench
```

### Using Docker Directly

**Ubuntu Build:**
```bash
# Build image
docker build -t movidius-nuc21:latest .

# Run and view build info
docker run --rm movidius-nuc21:latest

# Extract artifacts
docker run --rm -v $(pwd)/artifacts:/out movidius-nuc21:latest \
  sh -c "cp -r /opt/movidius/* /out/"
```

**Debian Build:**
```bash
# Build Debian image
docker build -f Dockerfile.debian -t movidius-nuc21:debian .

# Run and view build info
docker run --rm movidius-nuc21:debian

# Extract artifacts
docker run --rm -v $(pwd)/artifacts-debian:/out movidius-nuc21:debian \
  sh -c "cp -r /opt/movidius/* /out/"
```

## Multi-Stage Build

Both Dockerfiles (`Dockerfile` for Ubuntu, `Dockerfile.debian` for Debian) use a 3-stage build:

### Stage 1: Kernel Builder
- Base: `ubuntu:22.04` or `debian:bookworm`
- Installs kernel headers and build tools for the running kernel
- Compiles `movidius_x_vpu.ko` and `vfio_movidius.ko`

### Stage 2: Rust Builder
- Base: `rust:1.75-slim` or `rust:1.75-slim-bookworm`
- Builds entire Rust workspace in release mode
- Runs tests
- Produces optimized binaries

### Stage 3: Final Image
- Base: `ubuntu:22.04` or `debian:bookworm-slim` (minimal)
- Contains only compiled artifacts:
  - `/opt/movidius/kernel/*.ko` - Kernel modules
  - `/opt/movidius/bin/movidius-bench` - Benchmark tool
  - `/opt/movidius/scripts/*.sh` - Helper scripts

## GitHub Actions Integration

The project includes automated CI/CD via GitHub Actions (`.github/workflows/build.yml`):

### Workflow Jobs

1. **build-kernel** - Compiles kernel modules
2. **build-rust** - Builds Rust components (stable, beta, nightly)
3. **build-docker** - Creates Docker image and pushes to GHCR
4. **integration-test** - Tests compiled artifacts
5. **benchmark** - Runs performance benchmarks (main/develop only)

### Triggers

- Push to `main`, `develop`, or `claude/**` branches
- Pull requests to `main` or `develop`
- Manual dispatch via GitHub UI

### Artifacts

All jobs upload artifacts for 30 days:
- `kernel-modules` - Compiled .ko files
- `rust-binaries` - Release binaries
- `benchmark-results` - Criterion benchmark data

### Docker Registry

Images are automatically pushed to GitHub Container Registry:
```bash
# Pull pre-built image
docker pull ghcr.io/swordintel/nuc2.1:latest

# Or pull specific commit
docker pull ghcr.io/swordintel/nuc2.1:main-abc1234
```

## Local Development

### Interactive Development Container

```bash
# Start development container
docker-compose run --rm dev

# Inside container:
cargo build --release
cargo test
cargo bench
```

### Watch Mode

```bash
# Install cargo-watch in dev container
docker-compose run --rm dev cargo install cargo-watch

# Run with auto-rebuild
docker-compose run --rm dev cargo watch -x build
```

## Advanced Usage

### Build Specific Stage

```bash
# Build only kernel stage
docker build --target kernel-builder -t movidius-kernel:latest .

# Build only Rust stage
docker build --target rust-builder -t movidius-rust:latest .
```

### Custom Build Args

```bash
# Use specific Rust version
docker build --build-arg RUST_VERSION=1.74 -t movidius-nuc21:rust174 .

# With build cache
docker build --cache-from movidius-nuc21:latest -t movidius-nuc21:latest .
```

### Extract Specific Artifacts

```bash
# Extract only kernel modules
docker run --rm -v $(pwd):/out movidius-nuc21:latest \
  cp /opt/movidius/kernel/movidius_x_vpu.ko /out/

# Extract only benchmark binary
docker run --rm -v $(pwd):/out movidius-nuc21:latest \
  cp /opt/movidius/bin/movidius-bench /out/
```

## Troubleshooting

### Build Fails on Kernel Stage

**Problem:** `linux-headers-generic` not found

**Solution:** Update package lists
```bash
docker build --no-cache -t movidius-nuc21:latest .
```

### Build Fails on Rust Stage

**Problem:** Out of memory during compilation

**Solution:** Increase Docker memory limit
```bash
# In Docker Desktop: Settings > Resources > Memory > 8GB+
# Or use incremental compilation
docker build --build-arg CARGO_INCREMENTAL=1 -t movidius-nuc21:latest .
```

### Slow Build Times

**Problem:** Rebuilding from scratch every time

**Solution:** Use BuildKit caching
```bash
# Enable BuildKit
export DOCKER_BUILDKIT=1

# Build with cache
docker build --cache-from movidius-nuc21:latest -t movidius-nuc21:latest .
```

### Permission Issues with Artifacts

**Problem:** Cannot write to `./artifacts/`

**Solution:** Fix permissions
```bash
sudo chown -R $(id -u):$(id -g) artifacts/
chmod -R 755 artifacts/
```

## CI/CD Best Practices

### GitHub Actions Secrets

For private repositories, ensure:
- `GITHUB_TOKEN` is available (automatically provided)
- Package write permissions enabled in repo settings

### Caching Strategy

The workflow uses multiple cache layers:
1. **Cargo registry** - Downloaded crates
2. **Cargo git** - Git dependencies
3. **Build target** - Compiled artifacts
4. **Docker layers** - BuildKit cache (GHA)

### Artifact Retention

- Default: 30 days
- Main/develop: Consider longer retention
- PR builds: 7 days sufficient

## Performance Metrics

### Build Times (Approximate)

| Stage | Cold Build | Cached Build |
|-------|------------|--------------|
| Kernel | ~2 min | ~30 sec |
| Rust | ~5-8 min | ~1-2 min |
| Docker | ~10 min | ~3 min |

### Image Sizes

| Stage | Size |
|-------|------|
| kernel-builder | ~800 MB |
| rust-builder | ~2.5 GB |
| final | ~150 MB |

## References

- [Dockerfile best practices](https://docs.docker.com/develop/develop-images/dockerfile_best-practices/)
- [GitHub Actions Docker](https://docs.github.com/en/actions/publishing-packages/publishing-docker-images)
- [Multi-stage builds](https://docs.docker.com/build/building/multi-stage/)

---

**Last Updated:** 2025-11-07
**Docker Version:** >= 20.10
**Compose Version:** >= 2.0
