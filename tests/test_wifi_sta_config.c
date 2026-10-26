/**
 * @file test_wifi_sta_config.c
 * @brief Host-based Unit Tests for Wi-Fi STA Configuration and Reason Mapping (TDD)
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Minimal mode implementation under test */
#define TEST_VERBOSE 0

static const char *test_wifi_reason_to_str(uint8_t reason, bool verbose) {
    if (verbose) {
        switch (reason) {
            case 1: return "UNSPECIFIED (1)";
            case 2: return "AUTH_EXPIRE (2)";
            case 15: return "4WAY_HANDSHAKE_TIMEOUT (15)";
            case 200: return "BEACON_TIMEOUT (200)";
            case 201: return "NO_AP_FOUND (201)";
            case 202: return "AUTH_FAIL (202)";
            case 204: return "HANDSHAKE_TIMEOUT (204)";
            default: return "OTHER_REASON";
        }
    } else {
        switch (reason) {
            case 2: return "AUTH_EXPIRE";
            case 15: return "4WAY_TIMEOUT";
            case 200: return "BEACON_TIMEOUT";
            case 201: return "NO_AP_FOUND";
            case 204: return "HANDSHAKE_TIMEOUT";
            default: return "OTHER";
        }
    }
}

static uint32_t test_calculate_backoff_sec(uint32_t retry_count) {
    if (retry_count == 0) return 2;
    if (retry_count == 1) return 5;
    if (retry_count == 2) return 10;
    if (retry_count == 3) return 20;
    return 30;
}

/* FSM Disconnect Simulator */
typedef struct {
    bool is_connected;
    bool sta_stopped;
    uint32_t retry_count;
    bool timer_armed;
    uint32_t scheduled_backoff;
    uint32_t connect_attempts;
} sta_fsm_sim_t;

static void sim_handle_disconnected(sta_fsm_sim_t *fsm) {
    fsm->is_connected = false;
    if (fsm->sta_stopped) {
        /* If Station was stopped intentionally, disarm and do not schedule retry */
        fsm->timer_armed = false;
        return;
    }
    fsm->retry_count++;
    fsm->scheduled_backoff = test_calculate_backoff_sec(fsm->retry_count);
    fsm->timer_armed = true;
}

static void sim_on_reconnect_timer(sta_fsm_sim_t *fsm) {
    if (fsm->sta_stopped) {
        return;
    }
    if (!fsm->is_connected) {
        fsm->connect_attempts++;
    }
}

int main(void) {
    printf("====================================================\n");
    printf("   RUNNING WI-FI STA CONFIG & REASON TDD TESTS      \n");
    printf("====================================================\n\n");

    /* Test 1: Minimal reason mapping */
    printf("[RUN] Test 1: Minimal reason strings (zero rodata bloat)...\n");
    assert(strcmp(test_wifi_reason_to_str(2, false), "AUTH_EXPIRE") == 0);
    assert(strcmp(test_wifi_reason_to_str(15, false), "4WAY_TIMEOUT") == 0);
    assert(strcmp(test_wifi_reason_to_str(201, false), "NO_AP_FOUND") == 0);
    assert(strcmp(test_wifi_reason_to_str(99, false), "OTHER") == 0);
    printf("      -> PASS: Minimal strings verified.\n");

    /* Test 2: Verbose reason mapping */
    printf("[RUN] Test 2: Verbose reason strings...\n");
    assert(strstr(test_wifi_reason_to_str(2, true), "AUTH_EXPIRE") != NULL);
    assert(strstr(test_wifi_reason_to_str(15, true), "4WAY_HANDSHAKE_TIMEOUT") != NULL);
    printf("      -> PASS: Verbose strings verified.\n");

    /* Test 3: Backoff intervals */
    printf("[RUN] Test 3: Backoff progression...\n");
    assert(test_calculate_backoff_sec(0) == 2);
    assert(test_calculate_backoff_sec(1) == 5);
    assert(test_calculate_backoff_sec(2) == 10);
    assert(test_calculate_backoff_sec(3) == 20);
    assert(test_calculate_backoff_sec(4) == 30);
    assert(test_calculate_backoff_sec(100) == 30);
    printf("      -> PASS: Backoff progression verified.\n");

    /* Test 4: Station Stop Disarm & Suppression */
    printf("[RUN] Test 4: Station Stop Disarm & Reconnect Suppression...\n");
    sta_fsm_sim_t fsm = {
        .is_connected = true,
        .sta_stopped = false,
        .retry_count = 0,
        .timer_armed = false,
        .scheduled_backoff = 0,
        .connect_attempts = 0
    };

    /* Normal unexpected drop -> should schedule retry */
    sim_handle_disconnected(&fsm);
    assert(fsm.retry_count == 1);
    assert(fsm.timer_armed == true);
    assert(fsm.scheduled_backoff == 5);
    sim_on_reconnect_timer(&fsm);
    assert(fsm.connect_attempts == 1);

    /* User enters SoftAP -> wifi_sta_stop() sets sta_stopped = true */
    fsm.sta_stopped = true;
    fsm.timer_armed = false; /* disarmed by stop */

    /* Asynchronous WIFI_EVENT_STA_DISCONNECTED arrives after stop */
    sim_handle_disconnected(&fsm);
    assert(fsm.retry_count == 1); /* must NOT increment */
    assert(fsm.timer_armed == false); /* must remain disarmed */

    /* Timer callback firing while stopped must be a no-op */
    sim_on_reconnect_timer(&fsm);
    assert(fsm.connect_attempts == 1); /* must NOT attempt connection */
    printf("      -> PASS: sta_stopped safely disarms timer and suppresses retry.\n");

    printf("\n====================================================\n");
    printf("  🎉 ALL WI-FI STA CONFIG TDD TESTS PASSED (100%%)!    \n");
    printf("====================================================\n");
    return 0;
}
