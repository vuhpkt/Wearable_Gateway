/**
 * @file test_provisioning_fsm.c
 * @brief Host-based Unit Tests for Wi-Fi Provisioning FSM (TDD)
 *
 * Verifies Architecture v2 Contract:
 * 1. Clean transitions between STA_IDLE, AP_STARTING, PORTAL_ACTIVE, CONNECTING_NEW_STA, REVERTING_STA.
 * 2. Repeated BOOT button holds while active are safely ignored (no duplicate side effects).
 * 3. Accepted credentials win race against timeout in same session.
 * 4. Stale timeouts from previous generations cannot close a newer provisioning session.
 * 5. Duplicate submits in same session do not invoke NVS write multiple times.
 * 6. Timeout cleanly reverts to previous NVS credentials without MCU reboot.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HOST_UNIT_TEST 1

#include "provisioning_fsm.h"

/* Mock Adapter Call Counters & Arguments */
static int s_pause_mqtt_calls = 0;
static int s_stop_sta_calls = 0;
static int s_start_ap_calls = 0;
static int s_stop_ap_calls = 0;
static int s_start_portal_calls = 0;
static int s_stop_portal_calls = 0;
static int s_save_nvs_calls = 0;
static int s_read_nvs_calls = 0;
static int s_connect_sta_calls = 0;

static char s_last_saved_ssid[33] = {0};
static char s_last_saved_pass[65] = {0};
static char s_last_connected_ssid[33] = {0};
static char s_mock_nvs_ssid[33] = "PREV_HOME_WIFI";
static char s_mock_nvs_pass[65] = "secret123";

static void mock_pause_mqtt(void) { s_pause_mqtt_calls++; }
static void mock_stop_sta(void) { s_stop_sta_calls++; }
static void mock_start_ap(const char *ssid, const char *pass) {
    (void)ssid; (void)pass;
    s_start_ap_calls++;
}
static void mock_stop_ap(void) { s_stop_ap_calls++; }
static void mock_start_portal(void) { s_start_portal_calls++; }
static void mock_stop_portal(void) { s_stop_portal_calls++; }
static void mock_save_nvs(const char *ssid, const char *pass) {
    s_save_nvs_calls++;
    strncpy(s_last_saved_ssid, ssid, sizeof(s_last_saved_ssid) - 1);
    strncpy(s_last_saved_pass, pass, sizeof(s_last_saved_pass) - 1);
}
static bool mock_read_nvs(char *ssid, size_t s_len, char *pass, size_t p_len) {
    s_read_nvs_calls++;
    strncpy(ssid, s_mock_nvs_ssid, s_len - 1);
    strncpy(pass, s_mock_nvs_pass, p_len - 1);
    return true;
}
static void mock_connect_sta(const char *ssid, const char *pass) {
    (void)pass;
    s_connect_sta_calls++;
    strncpy(s_last_connected_ssid, ssid, sizeof(s_last_connected_ssid) - 1);
}

static void reset_mocks(void) {
    s_pause_mqtt_calls = 0;
    s_stop_sta_calls = 0;
    s_start_ap_calls = 0;
    s_stop_ap_calls = 0;
    s_start_portal_calls = 0;
    s_stop_portal_calls = 0;
    s_save_nvs_calls = 0;
    s_read_nvs_calls = 0;
    s_connect_sta_calls = 0;
    memset(s_last_saved_ssid, 0, sizeof(s_last_saved_ssid));
    memset(s_last_saved_pass, 0, sizeof(s_last_saved_pass));
    memset(s_last_connected_ssid, 0, sizeof(s_last_connected_ssid));
}

static const prov_adapter_t s_adapter = {
    .pause_mqtt = mock_pause_mqtt,
    .resume_mqtt = NULL,
    .stop_sta = mock_stop_sta,
    .start_ap = mock_start_ap,
    .stop_ap = mock_stop_ap,
    .start_portal = mock_start_portal,
    .stop_portal = mock_stop_portal,
    .save_nvs_creds = mock_save_nvs,
    .read_nvs_creds = mock_read_nvs,
    .connect_sta = mock_connect_sta,
};

static void test_normal_provisioning_flow(void) {
    printf("[RUN] Test 1: Normal provisioning flow (Hold -> Portal -> Submit -> STA)...\n");
    reset_mocks();
    prov_fsm_t fsm;
    prov_fsm_init(&fsm, &s_adapter);
    assert(prov_fsm_get_state(&fsm) == PROV_STATE_STA_IDLE);

    /* 1. User holds BOOT button for 2 seconds */
    prov_event_t evt_boot = { .type = PROV_EVT_BOOT_HOLD_2S };
    prov_fsm_dispatch(&fsm, &evt_boot);

    assert(prov_fsm_get_state(&fsm) == PROV_STATE_PORTAL_ACTIVE);
    assert(prov_fsm_get_generation(&fsm) == 1);
    assert(s_pause_mqtt_calls == 1);
    assert(s_stop_sta_calls == 1);
    assert(s_start_ap_calls == 1);
    assert(s_start_portal_calls == 1);

    /* 2. User submits new credentials */
    prov_event_t evt_submit = {
        .type = PROV_EVT_CREDENTIALS_SUBMITTED,
        .generation = 1,
        .ssid = "NEW_OFFICE_WIFI",
        .pass = "my_password"
    };
    prov_fsm_dispatch(&fsm, &evt_submit);

    assert(prov_fsm_get_state(&fsm) == PROV_STATE_STA_IDLE);
    assert(s_save_nvs_calls == 1);
    assert(strcmp(s_last_saved_ssid, "NEW_OFFICE_WIFI") == 0);
    assert(s_stop_portal_calls == 1);
    assert(s_stop_ap_calls == 1);
    assert(s_connect_sta_calls == 1);
    assert(strcmp(s_last_connected_ssid, "NEW_OFFICE_WIFI") == 0);

    printf("      -> PASS: Transitioned cleanly through SoftAP to new Station mode\n");
}

static void test_timeout_reverts_to_previous_creds(void) {
    printf("[RUN] Test 2: Timeout without credentials reverts to previous STA...\n");
    reset_mocks();
    prov_fsm_t fsm;
    prov_fsm_init(&fsm, &s_adapter);

    /* Trigger SoftAP */
    prov_event_t evt_boot = { .type = PROV_EVT_BOOT_HOLD_2S };
    prov_fsm_dispatch(&fsm, &evt_boot);
    assert(prov_fsm_get_state(&fsm) == PROV_STATE_PORTAL_ACTIVE);

    /* 5-minute timeout fires */
    prov_event_t evt_timeout = {
        .type = PROV_EVT_TIMEOUT,
        .generation = 1
    };
    prov_fsm_dispatch(&fsm, &evt_timeout);

    assert(prov_fsm_get_state(&fsm) == PROV_STATE_STA_IDLE);
    assert(s_save_nvs_calls == 0); /* Did NOT overwrite NVS */
    assert(s_read_nvs_calls == 1); /* Read previous credentials */
    assert(s_stop_portal_calls == 1);
    assert(s_stop_ap_calls == 1);
    assert(s_connect_sta_calls == 1);
    assert(strcmp(s_last_connected_ssid, "PREV_HOME_WIFI") == 0);

    printf("      -> PASS: Timeout cleanly restored previous NVS station connection\n");
}

static void test_submit_wins_against_timeout_in_same_session(void) {
    printf("[RUN] Test 3: Accepted submit wins race against timeout in same session...\n");
    reset_mocks();
    prov_fsm_t fsm;
    prov_fsm_init(&fsm, &s_adapter);

    prov_event_t evt_boot = { .type = PROV_EVT_BOOT_HOLD_2S };
    prov_fsm_dispatch(&fsm, &evt_boot);

    /* Submit credentials */
    prov_event_t evt_submit = {
        .type = PROV_EVT_CREDENTIALS_SUBMITTED,
        .generation = 1,
        .ssid = "CAFE_WIFI",
        .pass = "cafe_pass"
    };
    prov_fsm_dispatch(&fsm, &evt_submit);
    assert(s_connect_sta_calls == 1);
    assert(strcmp(s_last_connected_ssid, "CAFE_WIFI") == 0);

    /* An in-flight timeout from this same session arrives shortly after submit */
    prov_event_t evt_timeout = {
        .type = PROV_EVT_TIMEOUT,
        .generation = 1
    };
    prov_fsm_dispatch(&fsm, &evt_timeout);

    /* Timeout MUST be ignored; state must stay at new credentials */
    assert(s_connect_sta_calls == 1);
    assert(strcmp(s_last_connected_ssid, "CAFE_WIFI") == 0);

    printf("      -> PASS: Timeout discarded after credentials already accepted\n");
}

static void test_stale_timeout_and_duplicate_submit(void) {
    printf("[RUN] Test 4: Stale timeout from old generation and duplicate submit...\n");
    reset_mocks();
    prov_fsm_t fsm;
    prov_fsm_init(&fsm, &s_adapter);

    /* Session 1 */
    prov_event_t evt_boot = { .type = PROV_EVT_BOOT_HOLD_2S };
    prov_fsm_dispatch(&fsm, &evt_boot);
    assert(prov_fsm_get_generation(&fsm) == 1);

    /* Close session 1 */
    prov_event_t evt_submit1 = {
        .type = PROV_EVT_CREDENTIALS_SUBMITTED,
        .generation = 1,
        .ssid = "WIFI_A",
        .pass = "pass_a"
    };
    prov_fsm_dispatch(&fsm, &evt_submit1);
    assert(prov_fsm_get_state(&fsm) == PROV_STATE_STA_IDLE);

    /* Session 2 opened later */
    prov_fsm_dispatch(&fsm, &evt_boot);
    assert(prov_fsm_get_generation(&fsm) == 2);
    assert(prov_fsm_get_state(&fsm) == PROV_STATE_PORTAL_ACTIVE);

    /* Stale timeout from Session 1 arrives */
    prov_event_t stale_timeout = {
        .type = PROV_EVT_TIMEOUT,
        .generation = 1 /* OLD! */
    };
    prov_fsm_dispatch(&fsm, &stale_timeout);

    /* Session 2 portal MUST still be active! */
    assert(prov_fsm_get_state(&fsm) == PROV_STATE_PORTAL_ACTIVE);

    /* Duplicate submits in Session 2 */
    reset_mocks();
    prov_event_t evt_submit2 = {
        .type = PROV_EVT_CREDENTIALS_SUBMITTED,
        .generation = 2,
        .ssid = "WIFI_B",
        .pass = "pass_b"
    };
    prov_fsm_dispatch(&fsm, &evt_submit2);
    assert(s_save_nvs_calls == 1);

    /* Second identical submit: ignored */
    prov_fsm_dispatch(&fsm, &evt_submit2);
    assert(s_save_nvs_calls == 1);

    printf("      -> PASS: Stale generation timeout rejected and duplicate submit guarded\n");
}

static void test_repeated_boot_hold_ignored(void) {
    printf("[RUN] Test 5: Repeated BOOT hold ignored while portal is active...\n");
    reset_mocks();
    prov_fsm_t fsm;
    prov_fsm_init(&fsm, &s_adapter);

    prov_event_t evt_boot = { .type = PROV_EVT_BOOT_HOLD_2S };
    prov_fsm_dispatch(&fsm, &evt_boot);
    assert(s_start_ap_calls == 1);

    /* Second button hold */
    prov_fsm_dispatch(&fsm, &evt_boot);
    assert(s_start_ap_calls == 1); /* Not called again */
    assert(prov_fsm_get_generation(&fsm) == 1);

    printf("      -> PASS: Repeated BOOT hold during active provisioning safely ignored\n");
}

int main(void) {
    printf("====================================================\n");
    printf("   RUNNING PROVISIONING FSM UNIT TESTS (TDD)        \n");
    printf("====================================================\n\n");

    test_normal_provisioning_flow();
    test_timeout_reverts_to_previous_creds();
    test_submit_wins_against_timeout_in_same_session();
    test_stale_timeout_and_duplicate_submit();
    test_repeated_boot_hold_ignored();

    printf("\n🎉 ALL PROVISIONING FSM TESTS PASSED!\n");
    return 0;
}
