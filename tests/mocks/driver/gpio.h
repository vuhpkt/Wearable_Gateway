#ifndef MOCK_DRIVER_GPIO_H
#define MOCK_DRIVER_GPIO_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GPIO_NUM_0              0
#define GPIO_MODE_INPUT         1
#define GPIO_PULLUP_ENABLE      1
#define GPIO_PULLDOWN_DISABLE   0
#define GPIO_INTR_DISABLE       0

typedef struct {
    uint64_t pin_bit_mask;
    int mode;
    int pull_up_en;
    int pull_down_en;
    int intr_type;
} gpio_config_t;

extern int g_host_mock_gpio_level;

static inline esp_err_t gpio_config(const gpio_config_t *pGPIOConfig) {
    (void)pGPIOConfig;
    return ESP_OK;
}

static inline int gpio_get_level(int gpio_num) {
    (void)gpio_num;
    return g_host_mock_gpio_level;
}

#ifdef __cplusplus
}
#endif

#endif /* MOCK_DRIVER_GPIO_H */
