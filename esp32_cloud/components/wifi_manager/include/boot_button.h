#pragma once

#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*boot_button_callback_t)(void);

/**
 * @brief Initialize the BOOT button (GPIO 0) and spawn the periodic 50ms polling task.
 *
 * When GPIO 0 is held LOW for >= 2000ms, long_press_cb is invoked.
 *
 * @param long_press_cb Callback function to invoke on 2-second hold.
 * @return ESP_OK on success, or appropriate error code.
 */
esp_err_t boot_button_init(boot_button_callback_t long_press_cb);

#ifdef __cplusplus
}
#endif
