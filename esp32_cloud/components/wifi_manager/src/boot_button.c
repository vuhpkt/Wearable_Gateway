/*
 * BOOT Button Driver (GPIO 0)
 * 2-Second Hold Detection FSM (Zero-Heap Static Task Allocation)
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef HOST_TEST
#include "gateway_host_port.h"
#include "driver/gpio.h"
#else
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#include "boot_button.h"

#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define SAMPLE_PERIOD_MS        50
#define LONG_PRESS_HOLD_MS      2000
#define REQUIRED_HOLD_SAMPLES   (LONG_PRESS_HOLD_MS / SAMPLE_PERIOD_MS) /* 40 samples */

static const char *TAG = "BOOT_BTN";
static boot_button_callback_t s_long_press_cb = NULL;

#define BOOT_BTN_TASK_STACK_SIZE 2048
static StaticTask_t s_boot_btn_tcb;
static StackType_t s_boot_btn_stack[BOOT_BTN_TASK_STACK_SIZE / sizeof(StackType_t)];
static TaskHandle_t s_boot_btn_task_handle = NULL;

static uint32_t s_consecutive_low = 0;
static bool s_triggered = false;

static void boot_button_process_sample(int level) {
    if (level == 0) { /* Active LOW */
        if (!s_triggered) {
            s_consecutive_low++;
            if (s_consecutive_low >= REQUIRED_HOLD_SAMPLES) {
                s_triggered = true;
                ESP_LOGI(TAG, "🔘 [BUTTON] BOOT button held for %d ms! Triggering callback...", LONG_PRESS_HOLD_MS);
                if (s_long_press_cb != NULL) {
                    s_long_press_cb();
                }
            }
        }
    } else { /* Button released */
        s_consecutive_low = 0;
        s_triggered = false;
    }
}

static void boot_button_task(void *pvParameters) {
    (void)pvParameters;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
        int level = gpio_get_level(BOOT_BUTTON_GPIO);
        boot_button_process_sample(level);
    }
}

esp_err_t boot_button_init(boot_button_callback_t long_press_cb) {
    s_long_press_cb = long_press_cb;

    if (s_boot_btn_task_handle != NULL) {
        /* Idempotent re-initialization: do not recreate static task */
        return ESP_OK;
    }

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

    s_boot_btn_task_handle = xTaskCreateStaticPinnedToCore(
        boot_button_task,
        "boot_btn_tsk",
        BOOT_BTN_TASK_STACK_SIZE,
        NULL,
        2, /* Priority 2 */
        s_boot_btn_stack,
        &s_boot_btn_tcb,
        0  /* Core 0 */
    );
    if (s_boot_btn_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create static boot button task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "BOOT button initialized on GPIO %d (2s hold detection, static task).", BOOT_BUTTON_GPIO);
    return ESP_OK;
}

#ifdef HOST_TEST
int g_host_mock_gpio_level = 1;

void boot_button_test_step(int level) {
    boot_button_process_sample(level);
}

TaskHandle_t boot_button_test_get_task_handle(void) {
    return s_boot_btn_task_handle;
}

void boot_button_test_reset(void) {
    s_boot_btn_task_handle = NULL;
    s_long_press_cb = NULL;
    s_consecutive_low = 0;
    s_triggered = false;
    g_host_mock_gpio_level = 1;
}
#endif
