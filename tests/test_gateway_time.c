/**
 * @file test_gateway_time.c
 * @brief Host-based Unit Tests for Gateway Time Engine & SNTP Mailbox (TDD)
 *
 * Verifies Architecture v2 Contract:
 * 1. Pre-SNTP frames rejected with TIME_QUALITY_UNKNOWN (no live publish).
 * 2. Static Mailbox thread-safe transfer across cores without dynamic memory.
 * 3. Deterministic UTC mapping with Central->ESP monotonic delta.
 * 4. Epoch mismatch returns TIME_QUALITY_INVALID.
 * 5. Future timestamp rejection and uncertainty budget enforcement (> 250ms).
 * 6. Errors do NOT corrupt or force-reset anchor (no fake timestamps).
 * 7. Clock step (> 1000ms) invalidates anchor cleanly.
 * 8. Epoch reset preserves SNTP synchronization state.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HOST_UNIT_TEST 1

#include "gateway_time.h"

static void test_pre_sntp_rejection(void) {
    printf("[RUN] Test 1: Pre-SNTP frame rejection (no fake live timestamps)...\n");
    gateway_time_t gt;
    gateway_time_init(&gt, 100ULL);

    uint64_t out_utc_ms = 999;
    uint8_t out_quality = 99;
    uint16_t out_uncertainty = 99;

    int rc = gateway_time_map_tuple(&gt, 100ULL, 500ULL, 1000ULL,
                                    &out_utc_ms, &out_quality, &out_uncertainty);
    assert(rc == -3);
    assert(out_utc_ms == 0);
    assert(out_quality == TIME_QUALITY_UNKNOWN);
    assert(out_uncertainty == 0xFFFF);
    assert(gt.no_sntp_drops == 1);
    assert(gt.total_mapped == 0);

    printf("      -> PASS: Unsynchronized SNTP safely yields UNKNOWN (drops=%u)\n", gt.no_sntp_drops);
}

static void test_mailbox_and_deterministic_mapping(void) {
    printf("[RUN] Test 2: Mailbox posting and deterministic UTC mapping...\n");
    gateway_time_t gt;
    gateway_time_mailbox_t mb;

    gateway_time_init(&gt, 100ULL);
    gateway_time_mailbox_init(&mb);

    /* Post SNTP observation from Core 0: UTC = 1,700,000,000,000 at ESP mono 10,000 ms */
    gateway_time_mailbox_post(&mb, 1700000000000LL, 10000ULL);

    /* Core 1 applies mailbox */
    gateway_time_poll_and_apply_mailbox(&gt, &mb);
    assert(gt.sync.sntp_synced == true);
    assert(gt.last_applied_seq == 1);

    /* Subsequent poll has nothing pending */
    gateway_time_obs_t dummy;
    assert(gateway_time_mailbox_poll(&mb, &dummy) == false);

    /* Map a frame: Central mono = 5000 ms, ESP arrival mono = 10002 ms */
    uint64_t out_utc_ms = 0;
    uint8_t out_quality = 0;
    uint16_t out_uncertainty = 0;

    int rc = gateway_time_map_tuple(&gt, 100ULL, 5000ULL, 10002ULL,
                                    &out_utc_ms, &out_quality, &out_uncertainty);
    assert(rc == 0);
    assert(out_quality == TIME_QUALITY_SYNCED);
    /* Anchor set at (central=5000, esp=10002). UTC = 1,700,000,000,000 - 10,000 + 10,002 = 1,700,000,000,002 */
    assert(out_utc_ms == 1700000000002ULL);
    assert(gt.total_mapped == 1);

    /* Map next frame 200ms later: Central mono = 5200 ms, ESP mono = 10204 ms */
    rc = gateway_time_map_tuple(&gt, 100ULL, 5200ULL, 10204ULL,
                                &out_utc_ms, &out_quality, &out_uncertainty);
    assert(rc == 0);
    assert(out_quality == TIME_QUALITY_SYNCED);
    /* Delta = 200ms -> ESP mono = 10002 + 200 = 10202. Wire transit = 10204 - 10202 = 2ms */
    assert(out_utc_ms == 1700000000202ULL);
    assert(out_uncertainty == 2 + 2); /* RTT(2) + transit(2) = 4ms */
    assert(gt.total_mapped == 2);

    printf("      -> PASS: Frame mapped to %llu ms (uncertainty %u ms)\n",
           (unsigned long long)out_utc_ms, out_uncertainty);
}

static void test_epoch_mismatch_and_future_timestamp(void) {
    printf("[RUN] Test 3: Epoch mismatch and future timestamp guards...\n");
    gateway_time_t gt;
    gateway_time_init(&gt, 100ULL);

    gateway_time_obs_t obs = { .utc_ms = 1700000000000LL, .mono_ms = 10000ULL, .sequence = 1 };
    gateway_time_apply_obs(&gt, &obs);

    /* Initial anchor */
    uint64_t out_utc_ms = 0;
    uint8_t out_quality = 0;
    uint16_t out_uncertainty = 0;
    gateway_time_map_tuple(&gt, 100ULL, 5000ULL, 10000ULL, &out_utc_ms, &out_quality, &out_uncertainty);

    /* 1. Mismatched epoch 99 != 100 */
    int rc = gateway_time_map_tuple(&gt, 99ULL, 5100ULL, 10100ULL, &out_utc_ms, &out_quality, &out_uncertainty);
    assert(rc == -2);
    assert(out_quality == TIME_QUALITY_INVALID);
    assert(gt.invalid_epoch_drops == 1);

    /* 2. Future timestamp (Central mono says 6000ms, but ESP arrival mono is only 10100ms -> event in future by 900ms > 100ms slack) */
    rc = gateway_time_map_tuple(&gt, 100ULL, 6000ULL, 10100ULL, &out_utc_ms, &out_quality, &out_uncertainty);
    assert(rc == -4);
    assert(out_quality == TIME_QUALITY_INVALID);
    assert(gt.future_ts_drops == 1);

    printf("      -> PASS: Epoch mismatch & future timestamp rejected cleanly\n");
}

static void test_uncertainty_budget_and_no_forced_reanchor(void) {
    printf("[RUN] Test 4: Uncertainty budget (>250ms) and anchor integrity...\n");
    gateway_time_t gt;
    gateway_time_init(&gt, 100ULL);

    gateway_time_obs_t obs = { .utc_ms = 1700000000000LL, .mono_ms = 10000ULL, .sequence = 1 };
    gateway_time_apply_obs(&gt, &obs);

    uint64_t out_utc_ms = 0;
    uint8_t out_quality = 0;
    uint16_t out_uncertainty = 0;

    /* Anchor */
    gateway_time_map_tuple(&gt, 100ULL, 5000ULL, 10000ULL, &out_utc_ms, &out_quality, &out_uncertainty);

    /* Frame delayed by 300ms wire transit (Central delta = 0, but ESP arrival is 10300ms) */
    int rc = gateway_time_map_tuple(&gt, 100ULL, 5000ULL, 10300ULL, &out_utc_ms, &out_quality, &out_uncertainty);
    assert(rc == -5);
    assert(out_quality == TIME_QUALITY_INVALID);
    assert(gt.uncertainty_drops == 1);

    /* Anchor must still be intact and valid for subsequent normal frames */
    rc = gateway_time_map_tuple(&gt, 100ULL, 5010ULL, 10012ULL, &out_utc_ms, &out_quality, &out_uncertainty);
    assert(rc == 0);
    assert(out_quality == TIME_QUALITY_SYNCED);

    printf("      -> PASS: Uncertainty drop did NOT corrupt anchor (anchor preserved)\n");
}

static void test_clock_jump_and_epoch_reset(void) {
    printf("[RUN] Test 5: Clock jump (>1s) and epoch reset preserving SNTP offset...\n");
    gateway_time_t gt;
    gateway_time_init(&gt, 100ULL);

    gateway_time_obs_t obs1 = { .utc_ms = 1700000000000LL, .mono_ms = 10000ULL, .sequence = 1 };
    gateway_time_apply_obs(&gt, &obs1);

    /* Anchor */
    uint64_t out_utc_ms = 0;
    uint8_t out_quality = 0;
    uint16_t out_uncertainty = 0;
    gateway_time_map_tuple(&gt, 100ULL, 5000ULL, 10000ULL, &out_utc_ms, &out_quality, &out_uncertainty);
    assert(gt.sync.clock_anchored == true);

    /* 1. SNTP correction jumps by +5000ms */
    gateway_time_obs_t obs2 = { .utc_ms = 1700000015000LL, .mono_ms = 10000ULL, .sequence = 2 };
    gateway_time_apply_obs(&gt, &obs2);
    /* Clock jump should invalidate anchor */
    assert(gt.sync.clock_anchored == false);
    assert(gt.sync.clock_jump_resets == 1);
    assert(gt.sync.sntp_synced == true);

    /* Next frame smoothly re-anchors */
    int rc = gateway_time_map_tuple(&gt, 100ULL, 5100ULL, 10100ULL, &out_utc_ms, &out_quality, &out_uncertainty);
    assert(rc == 0);
    assert(gt.sync.clock_anchored == true);

    /* 2. Epoch change (Central reboots -> Epoch 101) */
    gateway_time_set_epoch(&gt, 101ULL);
    assert(gt.sync.bridge_epoch == 101ULL);
    assert(gt.sync.clock_anchored == false);
    /* CRITICAL CONTRACT: SNTP must NOT be lost on Central reboot! */
    assert(gt.sync.sntp_synced == true);

    rc = gateway_time_map_tuple(&gt, 101ULL, 100ULL, 20000ULL, &out_utc_ms, &out_quality, &out_uncertainty);
    assert(rc == 0);
    assert(gt.sync.clock_anchored == true);

    printf("      -> PASS: Clock jump and Central reboot handled with SNTP persistence\n");
}

int main(void) {
    printf("====================================================\n");
    printf("     RUNNING GATEWAY TIME UNIT TESTS (TDD)          \n");
    printf("====================================================\n\n");

    test_pre_sntp_rejection();
    test_mailbox_and_deterministic_mapping();
    test_epoch_mismatch_and_future_timestamp();
    test_uncertainty_budget_and_no_forced_reanchor();
    test_clock_jump_and_epoch_reset();

    printf("\n🎉 ALL GATEWAY TIME TESTS PASSED!\n");
    return 0;
}
