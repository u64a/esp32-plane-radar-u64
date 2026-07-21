#pragma once

// Headless native-gfx shim for ESP-IDF's <driver/gpio.h>.
//
// include/config.h needs only the gpio_num_t type and the GPIO_NUM_* constants
// for the display/BOOT pin constexprs; the native-gfx render harness never
// touches a real GPIO. This shim is selected ONLY by the [env:native-gfx]
// -iquote/-I include path (see platformio.ini); firmware builds keep the real
// ESP-IDF header. Values mirror the ESP-IDF enumeration so config.h's numeric
// pin assignments are identical to the firmware build.

#include <cstdint>

typedef enum {
  GPIO_NUM_NC = -1,
  GPIO_NUM_0 = 0,
  GPIO_NUM_1 = 1,
  GPIO_NUM_2 = 2,
  GPIO_NUM_3 = 3,
  GPIO_NUM_4 = 4,
  GPIO_NUM_5 = 5,
  GPIO_NUM_6 = 6,
  GPIO_NUM_7 = 7,
  GPIO_NUM_8 = 8,
  GPIO_NUM_9 = 9,
  GPIO_NUM_10 = 10,
  GPIO_NUM_11 = 11,
  GPIO_NUM_12 = 12,
  GPIO_NUM_13 = 13,
  GPIO_NUM_14 = 14,
  GPIO_NUM_15 = 15,
  GPIO_NUM_16 = 16,
  GPIO_NUM_17 = 17,
  GPIO_NUM_18 = 18,
  GPIO_NUM_19 = 19,
  GPIO_NUM_20 = 20,
  GPIO_NUM_21 = 21,
} gpio_num_t;
