/**
 * @file test_softap_timeout.c
 * @brief Host-based Unit Tests for SoftAP Provisioning Idle Timeout FSM (TDD)
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    bool is_active;
    uint32_t timeout_sec;
    uint32_t elapsed_sec;
    bool timer_armed;
    void (*on_timeout)(void);
} softap_fsm_t;

static void softap_fsm_init(softap_fsm_t *fsm, uint32_t timeout_sec, void (*cb)(void)) {
    fsm->is_active = false;
    fsm->timeout_sec = timeout_sec;
    fsm->elapsed_sec = 0;
    fsm->timer_armed = false;
    fsm->on_timeout = cb;
}

static void softap_fsm_start(softap_fsm_t *fsm) {
    fsm->is_active = true;
    fsm->elapsed_sec = 0;
    if (fsm->timeout_sec > 0) {
        fsm->timer_armed = true;
    }
}

static void softap_fsm_stop(softap_fsm_t *fsm) {
    fsm->is_active = false;
    fsm->timer_armed = false;
    fsm->elapsed_sec = 0;
}

static void softap_fsm_tick_1sec(softap_fsm_t *fsm) {
    if (fsm->is_active && fsm->timer_armed) {
        fsm->elapsed_sec++;
        if (fsm->elapsed_sec >= fsm->timeout_sec) {
            fsm->timer_armed = false;
            if (fsm->on_timeout != NULL) {
                fsm->on_timeout();
            }
        }
    }
}

static int s_timeout_fired_count = 0;
static void test_timeout_cb(void) {
    s_timeout_fired_count++;
}

int main(void) {
    printf("====================================================\n");
    printf("   RUNNING SOFTAP TIMEOUT UNIT TESTS (TDD)          \n");
    printf("====================================================\n\n");

    softap_fsm_t fsm;
    softap_fsm_init(&fsm, 300, test_timeout_cb); /* 5-minute timeout */

    /* Test 1: SoftAP started, advances 299s without firing */
    printf("[RUN] Test 1: Verify 299s does not trigger timeout...\n");
    s_timeout_fired_count = 0;
    softap_fsm_start(&fsm);
    for (int s = 0; s < 299; s++) {
        softap_fsm_tick_1sec(&fsm);
    }
    assert(s_timeout_fired_count == 0);
    assert(fsm.is_active == true);
    printf("      -> PASS: No timeout at 299s\n");

    /* Test 2: Exactly 300s triggers callback */
    printf("[RUN] Test 2: Exactly 300s triggers timeout callback...\n");
    softap_fsm_tick_1sec(&fsm);
    assert(s_timeout_fired_count == 1);
    printf("      -> PASS: Timeout triggered at 300s\n");

    /* Test 3: Credentials submitted at 60s cancels timer */
    printf("[RUN] Test 3: User submission at 60s disarms timer...\n");
    s_timeout_fired_count = 0;
    softap_fsm_start(&fsm);
    for (int s = 0; s < 60; s++) {
        softap_fsm_tick_1sec(&fsm);
    }
    softap_fsm_stop(&fsm); /* Submitted, SoftAP stopped */
    for (int s = 0; s < 300; s++) {
        softap_fsm_tick_1sec(&fsm);
    }
    assert(s_timeout_fired_count == 0);
    printf("      -> PASS: Stopped SoftAP never fires timeout\n");

    /* Test 4: Disabled timeout (0s) never fires */
    printf("[RUN] Test 4: Disabled timeout (timeout_sec=0) never fires...\n");
    s_timeout_fired_count = 0;
    softap_fsm_init(&fsm, 0, test_timeout_cb);
    softap_fsm_start(&fsm);
    for (int s = 0; s < 1000; s++) {
        softap_fsm_tick_1sec(&fsm);
    }
    assert(s_timeout_fired_count == 0);
    printf("      -> PASS: Zero timeout disables timer\n");

    printf("\n====================================================\n");
    printf("  🎉 ALL SOFTAP TIMEOUT TESTS PASSED (100%%)!         \n");
    printf("====================================================\n");
    return 0;
}
