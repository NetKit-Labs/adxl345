#pragma once

#include "adxl345/codec.hpp"
#include "adxl345/driver.hpp"
#include "adxl345/i2c_bus.hpp"
#include "adxl345/registers.hpp"
#include "adxl345/types.hpp"

#ifdef __linux__
#include "adxl345/linux_i2c_bus.hpp"
#endif
