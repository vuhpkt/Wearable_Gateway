/*
 * BOOT Button Driver (GPIO 0)
 * 2-Second Hold Detection FSM
 */

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "boot_button.h"

#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define SAMPLE_PERIOD_MS        50
#define LONG_PRESS_HOLD_MS      2000
#define REQUIRED_HOLD_SAMPLES   (LONG_PRESS_HOLD_MS / SAMPLE_PERIOD_MS) /* 40 samples */

static const char *TAG = "BOOT_BTN";
static boot_button_callback_t s_long_press_cb = NULL;

static void boot_button_task(void *pvParameters) {
    (void)pvParameters;
    uint32_t consecutive_low = 0;
    bool triggered = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));

        int level = gpio_get_level(BOOT_BUTTON_GPIO);
        if (level == 0) { /* Active LOW */
            if (!triggered) {
                consecutive_low++;
                if (consecutive_low >= REQUIRED_HOLD_SAMPLES) {
                    triggered = true;
                    ESP_LOGI(TAG, "🔘 [BUTTON] BOOT button held for %d ms! Triggering callback...", LONG_PRESS_HOLD_MS);
                    if (s_long_press_cb != NULL) {
                        s_long_press_cb();
                    }
                }
            }
        } else { /* Button released */
            consecutive_low = 0;
            triggered = false;
        }
    }
}

esp_err_t boot_button_init(boot_button_callback_t long_press_cb) {
    s_long_press_cb = long_press_cb;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO %d: %s", BOOT_BUTTON_GPIO, esp_err_to_name(err));
        return err;
    }

    BaseType_t ret = xTaskCreate(boot_button_task, "boot_btn_tsk", 2048, NULL, 2, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create boot button task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "BOOT button initialized on GPIO %d (2s hold detection).", BOOT_BUTTON_GPIO);
    return ESP_OK;
}
