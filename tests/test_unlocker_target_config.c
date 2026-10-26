#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

/* Mock GPIO types for host testing */
typedef int gpio_num_t;
#define GPIO_NUM_1   1
#define GPIO_NUM_2   2
#define GPIO_NUM_4   4
#define GPIO_NUM_5   5
#define GPIO_NUM_18  18
#define GPIO_NUM_19  19
#define GPIO_NUM_20  20

/* -------------------------------------------------------------
 * TEST 1: ESP32-S3 Target Configuration & Native USB Protection
 * ------------------------------------------------------------- */
#define CONFIG_IDF_TARGET_ESP32S3 1
#undef CONFIG_IDF_TARGET_ESP32

#include "../tools/esp32_nrf_unlocker/main/target_config.h"

static void test_esp32s3_defaults(void) {
    printf("[RUN ] Test 1: ESP32-S3 default pinout and USB safety...\n");
    assert(TARGET_PIN_SWCLK == GPIO_NUM_4);
    assert(TARGET_PIN_SWDIO == GPIO_NUM_5);

    /* Critical Safety Check: Never use GPIO 19 (USB_D-) or GPIO 20 (USB_D+) on ESP32-S3 */
    assert(TARGET_PIN_SWCLK != GPIO_NUM_19);
    assert(TARGET_PIN_SWCLK != GPIO_NUM_20);
    assert(TARGET_PIN_SWDIO != GPIO_NUM_19);
    assert(TARGET_PIN_SWDIO != GPIO_NUM_20);

    assert(TARGET_IS_S3 == true);
    printf("[PASS] Test 1: ESP32-S3 maps to GPIO 4 (SWCLK), GPIO 5 (SWDIO), USB D-/D+ protected.\n");
}

/* -------------------------------------------------------------
 * TEST 2: ESP32 Classic Target Configuration
 * ------------------------------------------------------------- */
static void test_esp32_defaults(void) {
    printf("[RUN ] Test 2: ESP32 classic default pinout...\n");
    #undef CONFIG_IDF_TARGET_ESP32S3
    #define CONFIG_IDF_TARGET_ESP32 1
    #undef TARGET_CONFIG_H
    #undef TARGET_IS_S3
    #undef TARGET_CHIP_NAME
    #undef DEFAULT_PIN_SWCLK
    #undef DEFAULT_PIN_SWDIO
    #undef TARGET_PIN_SWCLK
    #undef TARGET_PIN_SWDIO
    #include "../tools/esp32_nrf_unlocker/main/target_config.h"

    assert(TARGET_PIN_SWCLK == GPIO_NUM_18);
    assert(TARGET_PIN_SWDIO == GPIO_NUM_19);
    assert(TARGET_IS_S3 == false);
    printf("[PASS] Test 2: ESP32 classic maps to GPIO 18 (SWCLK), GPIO 19 (SWDIO).\n");
}

/* -------------------------------------------------------------
 * TEST 3: User Custom Pin Override
 * ------------------------------------------------------------- */
static void test_custom_override(void) {
    printf("[RUN ] Test 3: User custom pin override capability...\n");
    #define UNLOCKER_OVERRIDE_SWCLK GPIO_NUM_1
    #define UNLOCKER_OVERRIDE_SWDIO GPIO_NUM_2
    #undef TARGET_CONFIG_H
    #undef TARGET_IS_S3
    #undef TARGET_CHIP_NAME
    #undef DEFAULT_PIN_SWCLK
    #undef DEFAULT_PIN_SWDIO
    #undef TARGET_PIN_SWCLK
    #undef TARGET_PIN_SWDIO
    #include "../tools/esp32_nrf_unlocker/main/target_config.h"

    assert(TARGET_PIN_SWCLK == GPIO_NUM_1);
    assert(TARGET_PIN_SWDIO == GPIO_NUM_2);
    printf("[PASS] Test 3: Custom pin overrides honored.\n");
}

int main(void) {
    printf("=== RUNNING UNLOCKER TARGET CONFIG UNIT TESTS ===\n");
    test_esp32s3_defaults();
    test_esp32_defaults();
    test_custom_override();
    printf("=== ALL UNLOCKER TARGET TESTS PASSED (3/3) ===\n");
    return 0;
}
