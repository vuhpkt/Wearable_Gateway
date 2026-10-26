/**
 * @file test_boot_button_logic.c
 * @brief Host-based Unit Tests for 2-Second Long-Press Button State Machine (TDD)
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint32_t consecutive_low_samples;
    bool triggered;
    void (*on_long_press)(void);
} button_fsm_t;

#define SAMPLE_PERIOD_MS    50
#define LONG_PRESS_MS       2000
#define REQUIRED_SAMPLES    (LONG_PRESS_MS / SAMPLE_PERIOD_MS) /* 40 samples */

static void button_fsm_init(button_fsm_t *fsm, void (*cb)(void)) {
    fsm->consecutive_low_samples = 0;
    fsm->triggered = false;
    fsm->on_long_press = cb;
}

static void button_fsm_step(button_fsm_t *fsm, int pin_level) {
    if (pin_level == 0) { /* Active LOW */
        if (!fsm->triggered) {
            fsm->consecutive_low_samples++;
            if (fsm->consecutive_low_samples >= REQUIRED_SAMPLES) {
                fsm->triggered = true;
                if (fsm->on_long_press != NULL) {
                    fsm->on_long_press();
                }
            }
        }
    } else { /* Released (HIGH) */
        fsm->consecutive_low_samples = 0;
        fsm->triggered = false;
    }
}

static int s_callback_fire_count = 0;
static void dummy_cb(void) {
    s_callback_fire_count++;
}

int main(void) {
    printf("====================================================\n");
    printf("   RUNNING BOOT BUTTON (2s HOLD) TDD TESTS          \n");
    printf("====================================================\n\n");

    button_fsm_t fsm;
    button_fsm_init(&fsm, dummy_cb);

    /* Test 1: Short press (1.5s = 30 samples) does not fire */
    printf("[RUN] Test 1: Short press (1.5s) must not fire...\n");
    s_callback_fire_count = 0;
    for (int i = 0; i < 30; i++) {
        button_fsm_step(&fsm, 0);
    }
    button_fsm_step(&fsm, 1); /* Released */
    assert(s_callback_fire_count == 0);
    printf("      -> PASS: 1.5s hold did not trigger callback.\n");

    /* Test 2: Exact 2.0s hold (40 samples) fires exactly once */
    printf("[RUN] Test 2: Exact 2.0s hold (40 samples) fires exactly once...\n");
    s_callback_fire_count = 0;
    for (int i = 0; i < 40; i++) {
        button_fsm_step(&fsm, 0);
    }
    assert(s_callback_fire_count == 1);
    /* Extra hold samples should not re-trigger while held */
    for (int i = 0; i < 20; i++) {
        button_fsm_step(&fsm, 0);
    }
    assert(s_callback_fire_count == 1);
    printf("      -> PASS: Fired exactly once at 2.0s, no repeat while held.\n");

    /* Test 3: Release and press again for 2s fires second time */
    printf("[RUN] Test 3: Release and re-press for 2s fires again...\n");
    button_fsm_step(&fsm, 1); /* Released */
    for (int i = 0; i < 40; i++) {
        button_fsm_step(&fsm, 0);
    }
    assert(s_callback_fire_count == 2);
    printf("      -> PASS: Second press cycle fired callback.\n");

    printf("\n====================================================\n");
    printf("  🎉 ALL BOOT BUTTON TESTS PASSED (100%%)!           \n");
    printf("====================================================\n");
    return 0;
}
