#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROV_STATE_STA_IDLE = 0,         /* Normal Station mode */
    PROV_STATE_AP_STARTING,          /* Transitioning to SoftAP */
    PROV_STATE_PORTAL_ACTIVE,        /* SoftAP active, Web Portal serving */
    PROV_STATE_CONNECTING_NEW_STA,   /* Applying new credentials, connecting to STA */
    PROV_STATE_REVERTING_STA,        /* Timeout expired, reverting to previous STA credentials */
} prov_state_t;

typedef enum {
    PROV_EVT_BOOT_HOLD_2S = 1,       /* Boot button 2s hold detected */
    PROV_EVT_AP_STARTED,             /* SoftAP started */
    PROV_EVT_CREDENTIALS_SUBMITTED,  /* User submitted new SSID/Pass via portal */
    PROV_EVT_TIMEOUT,                /* Idle timeout expired */
} prov_event_type_t;

typedef struct {
    prov_event_type_t type;
    uint32_t          generation;    /* Generation token to reject stale events */
    char              ssid[33];
    char              pass[65];
} prov_event_t;

/* Dependency Injection Adapter for Hardware / System Side Effects */
typedef struct {
    void (*pause_mqtt)(void);
    void (*resume_mqtt)(void);
    void (*stop_sta)(void);
    void (*start_ap)(const char *ssid, const char *password);
    void (*stop_ap)(void);
    void (*start_portal)(void);
    void (*stop_portal)(void);
    void (*save_nvs_creds)(const char *ssid, const char *password);
    bool (*read_nvs_creds)(char *ssid, size_t ssid_len, char *password, size_t pass_len);
    void (*connect_sta)(const char *ssid, const char *password);
} prov_adapter_t;

typedef struct {
    prov_state_t         state;
    uint32_t             generation;       /* Increments on every transition to SoftAP */
    prov_adapter_t       adapter;
    char                 submitted_ssid[33];
    char                 submitted_pass[65];
    bool                 has_submitted_creds;
} prov_fsm_t;

void prov_fsm_init(prov_fsm_t *fsm, const prov_adapter_t *adapter);
void prov_fsm_dispatch(prov_fsm_t *fsm, const prov_event_t *evt);
prov_state_t prov_fsm_get_state(const prov_fsm_t *fsm);
uint32_t prov_fsm_get_generation(const prov_fsm_t *fsm);

#ifdef __cplusplus
}
#endif
