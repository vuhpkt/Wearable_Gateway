#ifndef ESP_ERR_H
#define ESP_ERR_H

#include "gateway_host_port.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline const char *esp_err_to_name(esp_err_t err) {
    if (err == ESP_OK) return "ESP_OK";
    if (err == ESP_FAIL) return "ESP_FAIL";
    if (err == ESP_ERR_NO_MEM) return "ESP_ERR_NO_MEM";
    if (err == ESP_ERR_INVALID_STATE) return "ESP_ERR_INVALID_STATE";
    return "ESP_ERR_UNKNOWN";
}

#ifdef __cplusplus
}
#endif

#endif /* ESP_ERR_H */
