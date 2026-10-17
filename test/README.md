# Test Suite for ESP32 RC Car FreeRTOS System

This directory contains unit tests and integration tests for the RC car system.

> **Ojo: hoy no hay tests unitarios en el repo.** Los archivos `test_*.cpp` que se listan abajo
> no existen, así que `pio test` no tiene nada que correr. Esta sección describe la suite
> prevista. Lo que sí hay: el simulador `test/python/test_uart_simulator.py`, el monitor
> `test/python/telemetry_monitor.py`, `test/validate_code.sh` y las guías manuales
> (`manual_test_guide.md`, `QUICK_UART_TEST.md`, `TESTING_WITHOUT_JETSON.md`).
>
> Si se escriben, tener en cuenta que el firmware actual **no** es el que asume esta lista: el
> estado inicial depende de `-DBOOT_ARMED` (hoy arranca `ARMED`), el control es *latching* (un
> comando de control vencido no frena) y el watchdog es de 300 ms en AUTO y 1000 ms en MANUAL.

## Test Files

- **test_mailbox.cpp**: Tests for mailbox functionality (read/write, expiration, last-writer-wins)
- **test_messages.cpp**: Tests for message protocol enums and constants
- **test_uart_parser.cpp**: Tests for UART message parsing
- **test_state_machine.cpp**: Tests for state machine transitions
- **test_integration.cpp**: Integration tests for end-to-end command flows

## Running Tests

### Using PlatformIO

```bash
# Run all tests
pio test

# Run specific test file
pio test --filter test_mailbox

# Run with verbose output
pio test -v

# Run tests on specific environment
pio test -e esp32doit-devkit-v1
```

### Test Environment Setup

The tests use Unity test framework (included with PlatformIO). Each test file:
1. Includes `unity.h` for test assertions
2. Uses `setup()` for test initialization
3. Uses `loop()` (can be empty)
4. Calls `UNITY_BEGIN()` and `UNITY_END()` to wrap tests

## Test Coverage

### Mailbox Tests
- ✅ Initialization
- ✅ Write and read operations
- ✅ Expiration checking
- ✅ No-expiration mode (TTL=0)
- ✅ Last-writer-wins behavior
- ✅ Null pointer handling

### Message Protocol Tests
- ✅ Command type enums
- ✅ Topic enums
- ✅ Mode enums (MANUAL/AUTO)
- ✅ State enums (DISARMED/ARMED/RUNNING/FAULT)
- ✅ Channel constants

### UART Parser Tests
- ✅ Emergency message parsing (`E:BRAKE_NOW`)
- ✅ Control messages with values (`C:SET_SPEED:128`)
- ✅ Management messages (`M:SYS_ARM`)
- ✅ Newline/carriage return handling
- ✅ Invalid message handling
- ✅ Negative value parsing

### State Machine Tests
- ✅ Initial state (DISARMED sin `-DBOOT_ARMED`, ARMED con él)
- ✅ DISARMED → ARMED transition
- ✅ ARMED → DISARMED transition
- ✅ ARMED → RUNNING transition (with heartbeat)
- ✅ Heartbeat timeout → FAULT (300 ms AUTO / 1000 ms MANUAL)
- ✅ Mode switching (MANUAL ↔ AUTO)
- ✅ E-STOP → FAULT transition
- ✅ Invalid transition prevention

### Integration Tests
- ✅ Motor command flow
- ✅ Steering command flow
- ✅ Emergency brake flow
- ✅ System mode switching
- ✅ Multiple mailbox independence
- ✅ Command priority (emergency overwrites)

## Writing New Tests

To add a new test:

1. Create a new file `test/test_<component>.cpp`
2. Include necessary headers and Unity:
   ```cpp
   #include <Arduino.h>
   #include <unity.h>
   #include "your_header.h"
   ```
3. Implement test functions:
   ```cpp
   void test_something(void) {
       // Test code
       TEST_ASSERT_EQUAL(expected, actual);
   }
   ```
4. Register tests in `setup()`:
   ```cpp
   void setup() {
       delay(2000);
       UNITY_BEGIN();
       RUN_TEST(test_something);
       UNITY_END();
   }
   ```

## Test Assertions

Common Unity assertions:
- `TEST_ASSERT_TRUE(condition)` - Assert condition is true
- `TEST_ASSERT_FALSE(condition)` - Assert condition is false
- `TEST_ASSERT_EQUAL(expected, actual)` - Assert equality
- `TEST_ASSERT_NOT_EQUAL(expected, actual)` - Assert inequality
- `TEST_ASSERT_EQUAL_STRING(expected, actual)` - Assert string equality
- `TEST_ASSERT_NULL(pointer)` - Assert pointer is NULL
- `TEST_ASSERT_NOT_NULL(pointer)` - Assert pointer is not NULL

## Notes

- Tests run on the actual ESP32 hardware (not simulation)
- Some tests require delays to test timing-dependent behavior
- Hardware-dependent tests (GPIO, PWM) may need mocking for unit tests
- Integration tests verify the interaction between components

