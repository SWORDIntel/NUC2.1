# Repository Guidelines

## Project Structure & Module Organization
- Kernel driver (C): `movidius_x_vpu.c` and `vfio_movidius.c` in repo root, built via `Makefile`; helper tools in `movidius-bench.c`.
- Rust NCAPI stack: workspace in `movidius-rs/` with crates `movidius-ncapi`, `movidius-hal`, and the TUI benchmark `movidius-bench/`; shared headers live in `movidius-rs/include/`.
- Automation and tooling: `scripts/` contains integration tests and helper utilities; `install.sh` wraps build/install flows; Docker builds are defined by `Dockerfile*` and `docker-compose.yml`.
- Documentation: top-level `README.md`, kernel notes in `KERNEL_INTEGRATION.md`, and archived design docs in `docs/archive/`.

## Build, Test, and Development Commands
- Kernel modules: `make` (defaults to io_uring enabled), `make ENABLE_IO_URING=0` (ioctl-only), `make clean`, `sudo make install`, `sudo make uninstall`.
- Bench tool: `make bench` auto-detects `liburing`; artifacts `movidius-bench` and `.ko` files land in repo root.
- Integration tests: `make test` (wraps `scripts/run-tests.sh`); requires built artifacts and root privileges for module load checks.
- Rust workspace: `cd movidius-rs && cargo build --release`, `cargo test --release --verbose`, `cargo fmt --all -- --check`, `cargo clippy --all-targets --all-features -- -D warnings`, `./scripts/benchmark.sh` for the TUI runner.
- Containers: `docker-compose up builder` (Ubuntu) or `docker-compose up builder-debian` to produce artifacts under `./artifacts*/`.

## Coding Style & Naming Conventions
- C driver code follows Linux kernel style (tabs, 80-100 col soft limit, descriptive `snake_case` symbols matching kernel conventions).
- Rust code must stay `rustfmt`/`clippy` clean; prefer explicit types on public surfaces and `snake_case` for modules/funcs, `CamelCase` for types.
- Keep commit messages in conventional-commit form (`feat:`, `fix:`, etc.) as in existing history; reference hardware behavior clearly in the subject.
- Prefer descriptive file/function names that mirror hardware concepts (e.g., `dma_arena`, `io_uring_path`); avoid abbreviations not used elsewhere in the repo.

## Testing Guidelines
- Run `make test` after building modules; expect root-only steps for load/unload and device probing.
- For Rust crates, run `cargo test --release` in `movidius-rs/`; add targeted tests under each crate’s `tests/` folder or `mod tests` blocks with clear scenario names.
- When hardware is unavailable, document skips/failures in PR notes; include relevant `dmesg` snippets for driver changes.
- Aim to keep TUI benchmark behavior covered by Rust tests and manual runs via `./scripts/benchmark.sh` before publishing performance results.

## Commit & Pull Request Guidelines
- Use concise conventional commits; group related driver + Rust changes together when feasible.
- PRs should include: summary of behavior change, kernel/Rust commands run (`make`, `cargo test`, etc.), and whether tests were on real hardware or simulated.
- Link issues or discuss rationale for ABI or sysfs changes; include screenshots or JSON excerpts when altering benchmark/TUI output.
- Ensure CI parity: local runs of `cargo fmt`, `cargo clippy`, `cargo test`, and `make` should pass to mirror `.github/workflows/build.yml`.
