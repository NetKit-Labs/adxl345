cmake_minimum_required(VERSION 3.25)
project(adxl345 VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(adxl345 STATIC src/codec.cpp src/driver.cpp)
target_include_directories(adxl345 PUBLIC include)
target_compile_options(adxl345 PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(adxl345 PRIVATE src/linux_i2c_bus.cpp)
  target_compile_definitions(adxl345 PUBLIC ADXL345_HAS_LINUX_I2C=1)

  add_executable(adxl345ctl src/cli.cpp)
  target_link_libraries(adxl345ctl PRIVATE adxl345)
  target_compile_options(adxl345ctl PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  set_target_properties(adxl345ctl PROPERTIES OUTPUT_NAME adxl345)

  add_executable(adxl345_hw tests/hardware_test.cpp)
  target_link_libraries(adxl345_hw PRIVATE adxl345)
  target_compile_options(adxl345_hw PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

add_executable(adxl345_tests
  tests/main.cpp
  tests/mock_bus.cpp
  tests/test_unit.cpp
  tests/test_regression.cpp
)
target_link_libraries(adxl345_tests PRIVATE adxl345)
target_include_directories(adxl345_tests PRIVATE tests)
target_compile_options(adxl345_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

enable_testing()
add_test(NAME unit COMMAND adxl345_tests unit)
add_test(NAME regression COMMAND adxl345_tests regression)
add_test(NAME all COMMAND adxl345_tests all)
