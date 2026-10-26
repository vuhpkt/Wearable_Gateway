#ifndef GATEWAY_HOST_PORT_H
#define GATEWAY_HOST_PORT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP-IDF Basic Types & Errors */
typedef int esp_err_t;
#define ESP_OK                  0
#define ESP_FAIL                (-1)
#define ESP_ERR_NO_MEM          (-2)
#define ESP_ERR_INVALID_STATE   (-3)

/* FreeRTOS Types & Constants */
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE                  1
#define pdFALSE                 0
#define pdPASS                  1
#define pdFAIL                  0
#define pdMS_TO_TICKS(ms)       ((TickType_t)(ms))
#define portMAX_DELAY           ((TickType_t)0xffffffffUL)

typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
typedef uint32_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0

/* Critical section mocks for single-threaded host test */
static inline void portENTER_CRITICAL(portMUX_TYPE *mux) { (void)mux; }
static inline void portEXIT_CRITICAL(portMUX_TYPE *mux) { (void)mux; }

/* Notification tracking for test assertions */
extern uint32_t g_host_task_notify_count;
static inline void xTaskNotifyGive(TaskHandle_t task) {
    if (task != NULL) {
        g_host_task_notify_count++;
    }
}
static inline uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t wait_ticks) {
    (void)wait_ticks;
    uint32_t count = g_host_task_notify_count;
    if (clear_on_exit) {
        g_host_task_notify_count = 0;
    }
    return count;
}

static inline void vTaskDelay(TickType_t ticks) { (void)ticks; }

/* Mock Task Creation */
typedef uint32_t StackType_t;
typedef struct {
    uint32_t dummy[16];
} StaticTask_t;

typedef void (*TaskFunction_t)(void *);

static inline TaskHandle_t xTaskCreateStaticPinnedToCore(
    TaskFunction_t pxTaskCode,
    const char * const pcName,
    const uint32_t ulStackDepth,
    void * const pvParameters,
    UBaseType_t uxPriority,
    StackType_t * const puxStackBuffer,
    StaticTask_t * const pxTaskBuffer,
    const BaseType_t xCoreID)
{
    (void)pxTaskCode; (void)pcName; (void)ulStackDepth; (void)pvParameters;
    (void)uxPriority; (void)puxStackBuffer; (void)xCoreID;
    static int s_dummy_static_task = 42;
    if (pxTaskBuffer != NULL) {
        return (TaskHandle_t)&s_dummy_static_task;
    }
    return NULL;
}

static inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t pvTaskCode,
                                                const char * const pcName,
                                                const uint32_t usStackDepth,
                                                void * const pvParameters,
                                                UBaseType_t uxPriority,
                                                TaskHandle_t * const pvCreatedTask,
                                                const BaseType_t xCoreID) {
    (void)pvTaskCode; (void)pcName; (void)usStackDepth; (void)pvParameters;
    (void)uxPriority; (void)xCoreID;
    static int s_dummy_task = 42;
    if (pvCreatedTask != NULL) {
        *pvCreatedTask = (TaskHandle_t)&s_dummy_task;
    }
    return pdPASS;
}

/* Logging Mocks */
#ifndef ESP_LOGI
#define ESP_LOGI(tag, fmt, ...) do {} while(0)
#endif
#ifndef ESP_LOGW
#define ESP_LOGW(tag, fmt, ...) do {} while(0)
#endif
#ifndef ESP_LOGE
#define ESP_LOGE(tag, fmt, ...) do {} while(0)
#endif
#ifndef ESP_LOGD
#define ESP_LOGD(tag, fmt, ...) do {} while(0)
#endif

/* Mock Network State */
extern bool g_host_wifi_connected;
static inline bool wifi_is_connected(void) {
    return g_host_wifi_connected;
}
static inline bool wifi_sta_is_connected(void) {
    return g_host_wifi_connected;
}

/* Mock HTTP Server for Wi-Fi Portal Testing */
typedef void *httpd_handle_t;
typedef struct httpd_req {
    int content_len;
    const char *mock_body;
    int mock_read_offset;
    int last_status_sent;
    char last_err_sent[128];
} httpd_req_t;

typedef esp_err_t (*httpd_uri_func_t)(httpd_req_t *req);
typedef struct {
    const char *uri;
    int method;
    httpd_uri_func_t handler;
    void *user_ctx;
} httpd_uri_t;

typedef struct {
    unsigned max_uri_handlers;
    unsigned stack_size;
} httpd_config_t;

#define HTTP_GET 1
#define HTTP_POST 2
#define HTTPD_DEFAULT_CONFIG() { .max_uri_handlers = 8, .stack_size = 4096 }
#define HTTPD_RESP_USE_STRLEN (-1)
#define HTTPD_400_BAD_REQUEST "400 Bad Request"
#define HTTPD_SOCK_ERR_TIMEOUT (-2)

static inline esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config) {
    (void)config;
    static int s_dummy_server = 1;
    if (handle) *handle = (httpd_handle_t)&s_dummy_server;
    return ESP_OK;
}
static inline esp_err_t httpd_stop(httpd_handle_t handle) { (void)handle; return ESP_OK; }
static inline esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri) {
    (void)handle; (void)uri; return ESP_OK;
}
static inline esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type) {
    (void)req; (void)type; return ESP_OK;
}
static inline esp_err_t httpd_resp_send(httpd_req_t *req, const char *buf, size_t buf_len) {
    (void)req; (void)buf; (void)buf_len;
    if (req) req->last_status_sent = 200;
    return ESP_OK;
}
static inline esp_err_t httpd_resp_send_err(httpd_req_t *req, const char *status, const char *msg) {
    (void)status;
    if (req) {
        req->last_status_sent = 400;
        if (msg) strncpy(req->last_err_sent, msg, sizeof(req->last_err_sent) - 1);
    }
    return ESP_OK;
}
static inline esp_err_t httpd_resp_send_408(httpd_req_t *req) {
    if (req) req->last_status_sent = 408;
    return ESP_OK;
}
static inline int httpd_req_recv(httpd_req_t *req, char *buf, size_t buf_len) {
    if (!req || !buf || !req->mock_body) return 0;
    int remaining = req->content_len - req->mock_read_offset;
    if (remaining <= 0) return 0;
    int to_copy = (remaining < (int)buf_len) ? remaining : (int)buf_len;
    memcpy(buf, req->mock_body + req->mock_read_offset, to_copy);
    req->mock_read_offset += to_copy;
    return to_copy;
}

static inline void wifi_ap_cancel_timeout(void) {}

#ifdef __cplusplus
}
#endif

#endif /* GATEWAY_HOST_PORT_H */
