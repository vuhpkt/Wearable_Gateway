#include <string.h>
#include "provisioning_fsm.h"

void prov_fsm_init(prov_fsm_t *fsm, const prov_adapter_t *adapter) {
    if (!fsm) return;
    memset(fsm, 0, sizeof(*fsm));
    fsm->state = PROV_STATE_STA_IDLE;
    fsm->generation = 0;
    if (adapter) {
        fsm->adapter = *adapter;
    }
}

prov_state_t prov_fsm_get_state(const prov_fsm_t *fsm) {
    if (!fsm) return PROV_STATE_STA_IDLE;
    return fsm->state;
}

uint32_t prov_fsm_get_generation(const prov_fsm_t *fsm) {
    if (!fsm) return 0;
    return fsm->generation;
}

void prov_fsm_dispatch(prov_fsm_t *fsm, const prov_event_t *evt) {
    if (!fsm || !evt) return;

    switch (evt->type) {
    case PROV_EVT_BOOT_HOLD_2S: {
        /* Ignore trigger if already running provisioning */
        if (fsm->state != PROV_STATE_STA_IDLE) {
            return;
        }

        fsm->generation++;
        fsm->has_submitted_creds = false;
        fsm->state = PROV_STATE_AP_STARTING;

        if (fsm->adapter.pause_mqtt) {
            fsm->adapter.pause_mqtt();
        }
        if (fsm->adapter.stop_sta) {
            fsm->adapter.stop_sta();
        }
        if (fsm->adapter.start_ap) {
            fsm->adapter.start_ap("ESP32_HEALTH_PROV", NULL);
        }
        if (fsm->adapter.start_portal) {
            fsm->adapter.start_portal();
        }

        fsm->state = PROV_STATE_PORTAL_ACTIVE;
        break;
    }

    case PROV_EVT_CREDENTIALS_SUBMITTED: {
        if (fsm->state != PROV_STATE_PORTAL_ACTIVE) {
            return;
        }
        /* Check generation: reject stale submit */
        if (evt->generation != 0 && evt->generation != fsm->generation) {
            return;
        }
        /* Guard against duplicate submission in the same session */
        if (fsm->has_submitted_creds) {
            return;
        }
        fsm->has_submitted_creds = true;
        strncpy(fsm->submitted_ssid, evt->ssid, sizeof(fsm->submitted_ssid) - 1);
        strncpy(fsm->submitted_pass, evt->pass, sizeof(fsm->submitted_pass) - 1);

        fsm->state = PROV_STATE_CONNECTING_NEW_STA;

        if (fsm->adapter.save_nvs_creds) {
            fsm->adapter.save_nvs_creds(evt->ssid, evt->pass);
        }
        if (fsm->adapter.stop_portal) {
            fsm->adapter.stop_portal();
        }
        if (fsm->adapter.stop_ap) {
            fsm->adapter.stop_ap();
        }
        if (fsm->adapter.connect_sta) {
            fsm->adapter.connect_sta(evt->ssid, evt->pass);
        }

        fsm->state = PROV_STATE_STA_IDLE;
        break;
    }

    case PROV_EVT_TIMEOUT: {
        if (fsm->state != PROV_STATE_PORTAL_ACTIVE) {
            return;
        }
        /* Check generation: stale timeout cannot cancel a newer session */
        if (evt->generation != 0 && evt->generation != fsm->generation) {
            return;
        }
        /* Submitted credentials win race against timeout */
        if (fsm->has_submitted_creds) {
            return;
        }

        fsm->state = PROV_STATE_REVERTING_STA;

        if (fsm->adapter.stop_portal) {
            fsm->adapter.stop_portal();
        }
        if (fsm->adapter.stop_ap) {
            fsm->adapter.stop_ap();
        }

        char prev_ssid[33] = {0};
        char prev_pass[65] = {0};
        if (fsm->adapter.read_nvs_creds &&
            fsm->adapter.read_nvs_creds(prev_ssid, sizeof(prev_ssid), prev_pass, sizeof(prev_pass))) {
            if (fsm->adapter.connect_sta && strlen(prev_ssid) > 0) {
                fsm->adapter.connect_sta(prev_ssid, prev_pass);
            }
        }

        fsm->state = PROV_STATE_STA_IDLE;
        break;
    }

    default:
        break;
    }
}
