# NUC2.1 licence migration and kernel compatibility

**Effective for distributions adopting this revision:** 2026-10-17.

## Licence scope

The original SWORD-owned NUC2.1 code, including its out-of-tree Movidius/VFIO drivers and Rust NCAPI workspace, is prospectively offered under the [SWORD Restricted Source Licence v1.1](../LICENSE), identified as `LicenseRef-SRSL-1.1`. The full canonical text is also provided at [LICENSES/LicenseRef-SRSL-1.1.txt](../LICENSES/LicenseRef-SRSL-1.1.txt). This is source-available, **not OSI-approved open-source**, licensing.

The previous [SRSL-1.0](../LICENSES/SRSL-1.0.txt) text is archived verbatim. Before this change, the README specifically described the two kernel modules as GPL-2.0 and the Rust workspace as `MIT OR Apache-2.0`; the Rust Cargo manifest made the same declaration. Those earlier permissions **cannot be retrospectively withdrawn** for recipients who acquired valid rights in already distributed copies. This change is for new copies and updates only; historical files in `docs/archive/` and `movidius-rs/README.md.old` remain historical reference material, not licensing notices for the current release.

## Kernel-module compatibility and licensing

Out-of-tree source files `movidius_x_vpu.c` and `vfio_movidius.c` now carry `SPDX-License-Identifier: LicenseRef-SRSL-1.1` and identify with `MODULE_LICENSE("Proprietary")` (the kernel's recognised non-GPL2-compatible module marker, not the precise licence text). This corrects previously inconsistent `MODULE_LICENSE("GPL")` declarations for **newly built copies**, but has operational consequences:

- Loading taints the kernel with `P` (proprietary module).
- The kernel refuses to bind the module to `EXPORT_SYMBOL_GPL`-only symbols, including possible transitive dependencies. A successful compile does **not** prove successful module load.
- An SRSL-only module is **not** suitable for direct upstream Linux-kernel inclusion under GPL-2.0. `KERNEL_INTEGRATION.md` describes a historical design route, not a ready-to-merge licence grant.

Before deploying, run `make` against your target headers, `modinfo -F license movidius_x_vpu.ko` and `modinfo -F license vfio_movidius.ko` (both should report `Proprietary`), then inspect `dmesg`/kernel logs on a **non-production** test host. Also inspect the `modpost` output for GPL-only symbol references. Do **not** conceal licensing information to bypass kernel export restrictions.

If kernel integration breaks, **do not falsify `MODULE_LICENSE`**: use an older, validly GPL-licensed release of the drivers with the separate earlier terms, pursue an independently authorised GPL-compatible driver build, or redesign to avoid GPL-only dependencies. Reverting just the metadata string while retaining SRSL-only source is not a lawful or correct repair.

## Rust workspace and third parties

The Rust workspace uses `license-file = "../LICENSE"` (relative to its root), inherited via `license-file.workspace = true` by its three member crates. This follows Cargo's supported nonstandard-licence mechanism and ensures the full SRSL-1.1 text is included in packaged crates. The SPDX source-code identifier remains `LicenseRef-SRSL-1.1`. All ordinary dependencies retain their independent original terms; no third-party software or firmware is relicensed here. `movidius-rs/README.md` and `movidius-rs/movidius-ncapi/README.md` reflect the same version.

## Commercial use and model training

The SRSL-1.1 commercial exclusions and AI source-training restriction apply only to protected SWORD-owned portions, subject to mandatory statutory exceptions. Fifty percent profit participation is an opening offer for a separately executed Commercial Agreement, **not** an automatic royalty or infringement debt.

## Scope and validation

This migration changes legal text, documentation, Cargo metadata and kernel `MODULE_LICENSE`/SPDX declarations. It does not add activation checks, accounts, telemetry, DRM, or change driver algorithms. **Kernel loadability must be revalidated** on target hardware. Legal enforceability and chain of title remain subject to the facts and relevant law.
