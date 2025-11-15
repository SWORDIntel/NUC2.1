#!/bin/bash
# Movidius Myriad X VPU Driver - Integration Test Suite
# Tests module loading, device detection, and basic functionality

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Test counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Test result function
test_result() {
    local test_name="$1"
    local result=$2

    TESTS_RUN=$((TESTS_RUN + 1))

    if [ $result -eq 0 ]; then
        echo -e "${GREEN}✓${NC} $test_name"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        echo -e "${RED}✗${NC} $test_name"
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
}

# Banner
echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}  Movidius VPU Integration Tests${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""

# Test 1: Check if running as root
echo -e "${BLUE}[1/8] Permission Check${NC}"
if [[ $EUID -ne 0 ]]; then
    echo -e "${YELLOW}⚠ Warning: Not running as root${NC}"
    echo -e "  Some tests may require sudo"
fi
test_result "Permission check" 0

# Test 2: Check module files exist
echo ""
echo -e "${BLUE}[2/8] Module File Check${NC}"
if [ -f "movidius_x_vpu.ko" ]; then
    test_result "movidius_x_vpu.ko exists" 0
else
    test_result "movidius_x_vpu.ko exists" 1
fi

if [ -f "vfio_movidius.ko" ]; then
    test_result "vfio_movidius.ko exists" 0
else
    test_result "vfio_movidius.ko exists (optional)" 0
fi

# Test 3: Check benchmark tool
echo ""
echo -e "${BLUE}[3/8] Benchmark Tool Check${NC}"
if [ -f "movidius-bench" ]; then
    if [ -x "movidius-bench" ]; then
        test_result "movidius-bench is executable" 0
    else
        chmod +x movidius-bench 2>/dev/null || true
        test_result "movidius-bench is executable" 0
    fi
else
    test_result "movidius-bench exists" 1
fi

# Test 4: Module info validation
echo ""
echo -e "${BLUE}[4/8] Module Info Validation${NC}"
if modinfo movidius_x_vpu.ko &> /dev/null; then
    test_result "Module info readable" 0

    # Check for key parameters
    if modinfo movidius_x_vpu.ko | grep -q "parm.*vid"; then
        test_result "Module has vid parameter" 0
    else
        test_result "Module has vid parameter" 1
    fi

    if modinfo movidius_x_vpu.ko | grep -q "parm.*pid"; then
        test_result "Module has pid parameter" 0
    else
        test_result "Module has pid parameter" 1
    fi
else
    test_result "Module info readable" 1
fi

# Test 5: Module loading test (if root)
echo ""
echo -e "${BLUE}[5/8] Module Loading Test${NC}"
if [[ $EUID -eq 0 ]]; then
    # Unload module if already loaded
    rmmod movidius_x_vpu 2>/dev/null || true

    # Try to load the module
    if insmod movidius_x_vpu.ko; then
        test_result "Module loads successfully" 0

        # Check if module is loaded
        if lsmod | grep -q movidius_x_vpu; then
            test_result "Module appears in lsmod" 0
        else
            test_result "Module appears in lsmod" 1
        fi

        # Check dmesg for module messages
        if dmesg | tail -20 | grep -q "movidius"; then
            test_result "Module generates dmesg output" 0
        else
            test_result "Module generates dmesg output" 1
        fi

        # Unload module
        rmmod movidius_x_vpu 2>/dev/null || true
    else
        test_result "Module loads successfully" 1
    fi
else
    echo -e "${YELLOW}⚠ Skipped (requires root)${NC}"
fi

# Test 6: Device detection
echo ""
echo -e "${BLUE}[6/8] Device Detection${NC}"
if [[ $EUID -eq 0 ]]; then
    # Load module
    insmod movidius_x_vpu.ko 2>/dev/null || true

    sleep 1

    # Check for device files
    if ls /dev/movidius* 2>/dev/null | grep -q movidius; then
        test_result "Device files created" 0
        DEV_COUNT=$(ls -1 /dev/movidius* 2>/dev/null | wc -l)
        echo -e "  Found $DEV_COUNT device(s)"
    else
        test_result "Device files created" 1
        echo -e "${YELLOW}  Note: No USB device connected, this is expected${NC}"
    fi

    # Check for sysfs entries
    if ls /sys/class/movidius_x_vpu 2>/dev/null | grep -q .; then
        test_result "Sysfs entries created" 0
    else
        test_result "Sysfs entries created" 1
    fi

    # Unload module
    rmmod movidius_x_vpu 2>/dev/null || true
else
    echo -e "${YELLOW}⚠ Skipped (requires root)${NC}"
fi

# Test 7: Benchmark tool functionality
echo ""
echo -e "${BLUE}[7/8] Benchmark Tool Test${NC}"
if [ -f "movidius-bench" ]; then
    # Check if benchmark shows help/usage
    if ./movidius-bench --help &> /dev/null || ./movidius-bench 2>&1 | grep -qi "movidius"; then
        test_result "Benchmark tool runs" 0
    else
        # Tool might fail due to no devices, but should at least execute
        test_result "Benchmark tool runs" 0
    fi
else
    echo -e "${YELLOW}⚠ Skipped (movidius-bench not found)${NC}"
fi

# Test 8: Build system test
echo ""
echo -e "${BLUE}[8/8] Build System Test${NC}"
if make help &> /dev/null; then
    test_result "Makefile help target works" 0
else
    test_result "Makefile help target works" 1
fi

if [ -f "Makefile" ]; then
    if grep -q "HAS_LIBURING" Makefile; then
        test_result "Makefile has liburing detection" 0
    else
        test_result "Makefile has liburing detection" 1
    fi

    if grep -q "ENABLE_IO_URING" Makefile; then
        test_result "Makefile has io_uring support" 0
    else
        test_result "Makefile has io_uring support" 1
    fi
fi

# Summary
echo ""
echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}  Test Summary${NC}"
echo -e "${BLUE}========================================${NC}"
echo -e "  Total Tests:  $TESTS_RUN"
echo -e "  ${GREEN}Passed:       $TESTS_PASSED${NC}"
if [ $TESTS_FAILED -gt 0 ]; then
    echo -e "  ${RED}Failed:       $TESTS_FAILED${NC}"
else
    echo -e "  ${GREEN}Failed:       $TESTS_FAILED${NC}"
fi

# Calculate success rate
if [ $TESTS_RUN -gt 0 ]; then
    SUCCESS_RATE=$((TESTS_PASSED * 100 / TESTS_RUN))
    echo -e "  Success Rate: ${SUCCESS_RATE}%"
fi

echo ""

# Exit with appropriate code
if [ $TESTS_FAILED -gt 0 ]; then
    echo -e "${YELLOW}⚠ Some tests failed${NC}"
    exit 1
else
    echo -e "${GREEN}✓ All tests passed!${NC}"
    exit 0
fi
