#!/bin/bash
# Code Validation Script for ESP32 RC Car
# Checks for common issues without requiring hardware

echo "=========================================="
echo "ESP32 RC Car Code Validation"
echo "=========================================="

ERRORS=0
WARNINGS=0

# Check 1: Verify all required header files exist
echo ""
echo "[1/8] Checking header files..."
REQUIRED_HEADERS=(
    "include/hardware.h"
    "include/mailbox.h"
    "include/messages.h"
)

for header in "${REQUIRED_HEADERS[@]}"; do
    if [ -f "$header" ]; then
        echo "  ✓ $header"
    else
        echo "  ✗ MISSING: $header"
        ((ERRORS++))
    fi
done

# Check 2: Verify all source files exist
echo ""
echo "[2/8] Checking source files..."
REQUIRED_SOURCES=(
    "src/main.cpp"
    "src/hardware.cpp"
    "src/mailbox.cpp"
    "src/motor_task.cpp"
    "src/steer_task.cpp"
    "src/lights_task.cpp"
    "src/link_rx_task.cpp"
    "src/link_tx_task.cpp"
    "src/supervisor_task.cpp"
)

for source in "${REQUIRED_SOURCES[@]}"; do
    if [ -f "$source" ]; then
        echo "  ✓ $source"
    else
        echo "  ✗ MISSING: $source"
        ((ERRORS++))
    fi
done

# Check 3: Verify function declarations match implementations
echo ""
echo "[3/8] Checking function declarations..."
if grep -q "void hardware_init(void)" src/hardware.cpp && grep -q "void hardware_init(void)" include/hardware.h; then
    echo "  ✓ hardware_init() declared and defined"
else
    echo "  ✗ hardware_init() mismatch"
    ((ERRORS++))
fi

# Check 4: Verify critical constants are defined
echo ""
echo "[4/8] Checking critical constants..."
if grep -q "MOTOR_SPEED_MAX" include/hardware.h; then
    echo "  ✓ MOTOR_SPEED_MAX defined"
else
    echo "  ✗ MOTOR_SPEED_MAX not found"
    ((ERRORS++))
fi

if grep -q "WATCHDOG_TIMEOUT_MS" include/hardware.h; then
    echo "  ✓ WATCHDOG_TIMEOUT_MS defined"
else
    echo "  ✗ WATCHDOG_TIMEOUT_MS not found"
    ((ERRORS++))
fi

# Check 5: Verify FreeRTOS task functions
echo ""
echo "[5/8] Checking FreeRTOS tasks..."
TASKS=(
    "motor_task"
    "steer_task"
    "lights_task"
    "link_rx_task"
    "link_tx_task"
    "supervisor_task"
)

for task in "${TASKS[@]}"; do
    if grep -q "void ${task}(void \*pvParameters)" src/${task}.cpp; then
        echo "  ✓ ${task}() defined"
    else
        echo "  ✗ ${task}() not found"
        ((ERRORS++))
    fi
done

# Check 6: Verify mailbox functions
echo ""
echo "[6/8] Checking mailbox functions..."
MAILBOX_FUNCS=(
    "mailbox_init"
    "mailbox_write"
    "mailbox_read"
    "mailbox_is_expired"
)

for func in "${MAILBOX_FUNCS[@]}"; do
    if grep -q "${func}" src/mailbox.cpp && grep -q "${func}" include/mailbox.h; then
        echo "  ✓ ${func}() declared and defined"
    else
        echo "  ✗ ${func}() mismatch"
        ((ERRORS++))
    fi
done

# Check 7: Verify all tasks include required headers
echo ""
echo "[7/8] Checking includes..."
if grep -q "#include.*hardware.h" src/motor_task.cpp && \
   grep -q "#include.*mailbox.h" src/motor_task.cpp && \
   grep -q "#include.*messages.h" src/motor_task.cpp; then
    echo "  ✓ motor_task.cpp includes required headers"
else
    echo "  ⚠ motor_task.cpp missing some includes"
    ((WARNINGS++))
fi

# Check 8: Verify platformio.ini configuration
echo ""
echo "[8/8] Checking platformio.ini..."
if grep -q "framework = arduino" platformio.ini; then
    echo "  ✓ Arduino framework configured"
else
    echo "  ✗ Arduino framework not configured"
    ((ERRORS++))
fi

if grep -q "ESP32Servo" platformio.ini; then
    echo "  ✓ ESP32Servo library configured"
else
    echo "  ✗ ESP32Servo library missing"
    ((ERRORS++))
fi

# Summary
echo ""
echo "=========================================="
echo "Validation Summary"
echo "=========================================="
echo "Errors:   $ERRORS"
echo "Warnings: $WARNINGS"

if [ $ERRORS -eq 0 ]; then
    echo ""
    echo "✓ Code structure looks good!"
    echo ""
    echo "Next steps:"
    echo "  1. Compile: pio run"
    echo "  2. Upload: pio run -t upload"
    echo "  3. Monitor: pio device monitor"
    echo "  4. Test: python3 test/test_uart_simulator.py -p /dev/ttyUSB0"
    exit 0
else
    echo ""
    echo "✗ Found $ERRORS error(s). Please fix before compiling."
    exit 1
fi

