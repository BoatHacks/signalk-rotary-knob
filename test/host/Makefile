# Host unit tests for the pure logic in components/knob_logic.
#
#   make test UNITY_DIR=/path/to/Unity/src
#
# Needs a host C compiler (gcc or clang) and Unity (unity.c, unity.h, unity_internals.h). Unity ships with ESP-IDF at
# $(IDF_PATH)/components/unity/unity/src and with PlatformIO in its tool-unity package. Not part of the firmware build.
# Last run 2026-11-27 on gcc 14 (aarch64 Debian): 52 tests, 0 failures.
CC ?= gcc
UNITY_DIR ?= $(IDF_PATH)/components/unity/unity/src
LOGIC := ../../components/knob_logic

test_knob_logic: test_knob_logic.c $(LOGIC)/knob_logic.c $(LOGIC)/knob_demo.c $(LOGIC)/knob_motor.c $(LOGIC)/knob_autodemo.c $(LOGIC)/knob_gauge.c $(UNITY_DIR)/unity.c
	$(CC) -std=c11 -Wall -Wextra -I$(LOGIC) -I$(UNITY_DIR) $^ -lm -o $@

.PHONY: test clean
test: test_knob_logic
	./test_knob_logic

clean:
	rm -f test_knob_logic test_knob_logic.exe
