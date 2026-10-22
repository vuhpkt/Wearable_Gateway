#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifndef HOST_UNIT_TEST
#include <esp_err.h>
#endif

#include "thingsboard_config.h"
#include "thingsboard_mqtt.h"
#include "data_synthesizer.h"
#include "gateway_metrics.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef HOST_UNIT_TEST
/**
 * @brief Initialize ThingsBoard MQTT client and metrics
 */
esp_err_t tb_client_init(void);
#endif

#ifdef __cplusplus
}
#endif
