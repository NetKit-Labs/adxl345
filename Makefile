CXX ?= g++
CXXFLAGS ?= -std=c++23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Iinclude -Itests
BUILD := build

UNAME_S := $(shell uname -s)

LIB_OBJS := $(BUILD)/codec.o $(BUILD)/driver.o
BINS := $(BUILD)/adxl345_tests

ifeq ($(UNAME_S),Linux)
  LIB_OBJS += $(BUILD)/linux_i2c_bus.o
  CXXFLAGS += -DADXL345_HAS_LINUX_I2C
  BINS += $(BUILD)/adxl345 $(BUILD)/adxl345_hw $(BUILD)/adxl345_compare
endif

.PHONY: all test clean

all: $(BINS)

test: all
	$(BUILD)/adxl345_tests all

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/adxl345_tests: tests/main.cpp tests/mock_bus.cpp tests/test_unit.cpp tests/test_regression.cpp $(LIB_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/adxl345: src/cli.cpp $(LIB_OBJS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/adxl345_hw: tests/hardware_test.cpp $(LIB_OBJS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/adxl345_compare: tests/compare_kernel_drivers.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -pthread $< -o $@

clean:
	rm -rf $(BUILD)
