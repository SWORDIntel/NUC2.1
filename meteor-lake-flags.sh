#!/bin/bash
# ============================================================================
# METEOR LAKE OPTIMIZATION FLAGS - MOVIDIUS VPU PROJECT
# Optimized for Intel Core Ultra 7 165H (Meteor Lake)
# ============================================================================
# This file provides optimized compiler flags specifically selected for the
# Movidius VPU driver project, balancing performance and compatibility.
# ============================================================================

# ============================================================================
# USER-SPACE C/C++ FLAGS (for movidius-bench and other tools)
# ============================================================================

# Optimal flags for user-space applications
# Selected from Meteor Lake capabilities, excluding unsafe kernel flags
export CFLAGS_USERSPACE="\
-O3 \
-pipe \
-fomit-frame-pointer \
-funroll-loops \
-fstrict-aliasing \
-fno-plt \
-fdata-sections \
-ffunction-sections \
-flto=auto \
-fuse-linker-plugin \
-march=meteorlake \
-mtune=meteorlake \
-msse4.2 \
-mpopcnt \
-mavx \
-mavx2 \
-mfma \
-mf16c \
-mbmi \
-mbmi2 \
-mlzcnt \
-mmovbe \
-mavxvnni \
-mavxvnniint8 \
-mavxifma \
-mavxneconvert \
-maes \
-mvaes \
-mpclmul \
-mvpclmulqdq \
-msha \
-mgfni \
-mkl \
-mwidekl \
-madx \
-mclflushopt \
-mclwb \
-mcldemote \
-mmovdiri \
-mmovdir64b \
-mwaitpkg \
-mserialize \
-mtsxldtrk \
-muintr \
-mprefetchw \
-mprfchw \
-mprefetchi \
-mrdrnd \
-mrdseed \
-mfsgsbase \
-mfxsr \
-mxsave \
-mxsaveopt \
-mxsavec \
-mxsaves \
-mhreset \
-mpku \
-mptwrite \
-mrdpid \
-mpconfig \
-menqcmd \
-mcmpccxadd \
-mraoint \
-mshstk \
-mibt \
-minvpcid \
-mlahf-sa \
-ftree-vectorize \
-ftree-slp-vectorize \
-fipa-pta \
-fipa-cp-clone \
-fdevirtualize-speculatively \
-fdevirtualize-at-ltrans \
-fipa-ra \
-fipa-sra \
-fipa-vrp \
-fsched-pressure \
-fsched-spec-load \
-fmodulo-sched \
-fmodulo-sched-allow-regmoves \
-ftree-loop-im \
-ftree-loop-distribution \
-ftree-loop-distribute-patterns \
-ftree-loop-vectorize \
-floop-nest-optimize \
-fgcse-after-reload \
-fpredictive-commoning \
-ftree-partial-pre \
-ftracer \
-fsplit-paths \
-fprefetch-loop-arrays \
--param l1-cache-size=48 \
--param l1-cache-line-size=64 \
--param l2-cache-size=2048 \
--param prefetch-latency=300 \
--param simultaneous-prefetches=6 \
--param prefetch-min-insn-to-mem-ratio=3"

# Linker flags for user-space
export LDFLAGS_USERSPACE="\
-Wl,--as-needed \
-Wl,--gc-sections \
-Wl,-O2 \
-Wl,--hash-style=gnu \
-Wl,--sort-common \
-Wl,--enable-new-dtags \
-flto=auto \
-fuse-linker-plugin \
-Wl,-flto \
-flto-partition=balanced"

# ============================================================================
# KERNEL MODULE FLAGS (restricted - kernel-safe only)
# ============================================================================
# Note: Kernel modules cannot use -fPIC, -fPIE, or many user-space optimizations
# These flags are safe for kernel module compilation

export KCFLAGS_OPTIMAL="\
-O3 \
-pipe \
-march=meteorlake \
-mtune=meteorlake \
-msse4.2 \
-mpopcnt \
-mavx \
-mavx2 \
-mfma \
-mavxvnni \
-mavxvnniint8 \
-maes \
-mvaes \
-mpclmul \
-mvpclmulqdq \
-msha \
-mgfni \
-falign-functions=64 \
-falign-jumps=64 \
-falign-loops=64 \
-falign-labels=64 \
--param l1-cache-size=48 \
--param l1-cache-line-size=64 \
--param l2-cache-size=2048"

# ============================================================================
# RUST TARGET FEATURES (for .cargo/config.toml)
# ============================================================================

export RUST_TARGET_CPU="meteorlake"
export RUST_TARGET_FEATURES="\
+avx2 \
+fma \
+aes \
+vaes \
+pclmul \
+vpclmulqdq \
+sha \
+gfni \
+avxvnni \
+avxvnniint8 \
+avxifma \
+avxneconvert \
+bmi1 \
+bmi2 \
+lzcnt \
+popcnt \
+movbe \
+rdrand \
+rdseed \
+fsgsbase \
+xsave \
+xsaveopt \
+xsavec \
+xsaves \
+invpcid \
+sha \
+gfni \
+vaes \
+vpclmulqdq \
+kl \
+widekl \
+shstk \
+ibt"

# ============================================================================
# VERIFICATION FUNCTIONS
# ============================================================================

test_flags() {
    echo "Testing user-space CFLAGS..."
    if echo 'int main(){return 0;}' | gcc -xc $CFLAGS_USERSPACE - -o /tmp/test_flags 2>&1; then
        echo "✓ User-space flags verified"
        rm -f /tmp/test_flags
    else
        echo "✗ User-space flags failed - check compiler version"
        return 1
    fi
}

show_config() {
    echo "╔══════════════════════════════════════════════════════════════════════════╗"
    echo "║  METEOR LAKE OPTIMIZATION - MOVIDIUS VPU PROJECT                        ║"
    echo "║  CPU: Intel Core Ultra 7 165H | Meteor Lake                             ║"
    echo "╚══════════════════════════════════════════════════════════════════════════╝"
    echo ""
    echo "User-space CFLAGS: $(echo $CFLAGS_USERSPACE | wc -w) flags"
    echo "Kernel KCFLAGS: $(echo $KCFLAGS_OPTIMAL | wc -w) flags"
    echo "Rust target: $RUST_TARGET_CPU"
    echo ""
    echo "To use:"
    echo "  source meteor-lake-flags.sh"
    echo "  make bench  # Uses CFLAGS_USERSPACE"
    echo "  make       # Kernel modules use KCFLAGS_OPTIMAL"
}

# Auto-show config on source
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    # Script is being executed directly
    show_config
else
    # Script is being sourced
    echo "Meteor Lake optimization flags loaded. Use: show_config, test_flags"
fi
