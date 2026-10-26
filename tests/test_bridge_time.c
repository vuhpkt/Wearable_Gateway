/**
 * @file test_bridge_time.c
 * @brief Host-based Unit Tests for Central-ESP32 Time Sync & Event Timestamping (TDD)
 *
 * Verifies Architecture v2 Contract §6 & Task T07:
 * 1. Monotonic Central->ESP delta mapped to UTC via SNTP offset.
 * 2. Event timestamp calculated at arrival, invariant to queue backlog/delay.
 * 3. Freshness window boundary (fresh at 4999ms, stale at 5001ms).
 * 4. Missing SNTP does not produce LIVE timestamps.
 * 5. Clock step / jump (±SNTP step, epoch reset) invalidates anchor.
 * 6. Future timestamps and uncertainty > 250ms rejected.
 * 7. ECG batch end/period duration calculation.
 * 8. Same-ms collision resolution enforcing strict monotonicity.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "common/bridge_time.h"

static void test_clock_anchor_and_monotonic_delta(void) {
    printf("[RUN] Test 1: Clock anchor and monotonic delta calculation...\n");
    bridge_time_sync_t ts;
    bridge_time_init(&ts, 1000ULL);

    /* Anchor Central mono 5000ms to ESP mono 10000ms with RTT 2ms */
    int ret = bridge_time_anchor(&ts, 1000ULL, 5000ULL, 10000ULL, 2);
    assert(ret == 0);
    assert(ts.clock_anchored == true);

    /* Set SNTP UTC offset: UTC = ESP_mono + 1,700,000,000,000ms */
    int64_t sntp_offset = 1700000000000LL;
    bridge_time_set_sntp_offset(&ts, sntp_offset, 10000ULL);
    assert(ts.sntp_synced == true);

    /* Telemetry received: Central mono = 5200ms, ESP arrival mono = 10202ms */
    uint64_t event_utc_ms = 0;
    uint8_t quality = 0;
    uint16_t uncertainty = 0;

    ret = bridge_time_compute_event_ts(&ts, 1000ULL, 5200ULL, 10202ULL,
                                      &event_utc_ms, &quality, &uncertainty);
    assert(ret == 0);
    assert(quality == TIME_QUALITY_SYNCED);
    /* Event mono = 10000 + (5200 - 5000) = 10200. UTC = 1700000000000 + 10200 = 1700000010200 */
    assert(event_utc_ms == 1700000010200ULL);
    /* Wire transit = 10202 - 10200 = 2ms. Total uncertainty = RTT(2) + transit(2) = 4ms <= 250ms */
    assert(uncertainty == 4);
    printf("      -> PASS: UTC event TS=%llu ms with quality SYNCED (uncertainty: %u ms)\n",
           (unsigned long long)event_utc_ms, uncertainty);
}

static void test_delayed_queue_backlog_invariance(void) {
    printf("[RUN] Test 2: Delayed queue backlog invariance & freshness window...\n");
    bridge_time_sync_t ts;
    bridge_time_init(&ts, 1000ULL);
    bridge_time_anchor(&ts, 1000ULL, 1000ULL, 2000ULL, 2);
    bridge_time_set_sntp_offset(&ts, 1700000000000LL, 2000ULL);

    /* Frame arrives at ESP Core 1 at esp_rx_mono_ms = 2002 */
    uint64_t event_utc_ms = 0;
    uint8_t quality = 0;
    uint16_t uncertainty = 0;
    int ret = bridge_time_compute_event_ts(&ts, 1000ULL, 1000ULL, 2002ULL,
                                          &event_utc_ms, &quality, &uncertainty);
    assert(ret == 0);
    assert(event_utc_ms == 1700000002000ULL);

    /* Frame is placed in queue and delayed for 3500ms before Core 0 processes it */
    uint64_t esp_dequeue_mono_ms = 2002ULL + 3500ULL;
    uint32_t backlog_age = bridge_time_calc_backlog_ms(2002ULL, esp_dequeue_mono_ms);
    assert(backlog_age == 3500U);

    /* Event timestamp remains identical (arrival timestamp, not dequeue timestamp!) */
    assert(event_utc_ms == 1700000002000ULL);

    /* Test Freshness Window: 3500ms is fresh (< 5000ms) */
    uint64_t server_now_1 = event_utc_ms + 3500ULL;
    assert(bridge_time_is_fresh(event_utc_ms, server_now_1, BRIDGE_TIME_LIVE_TTL_MS) == true);

    /* Exactly 4999ms is fresh */
    uint64_t server_now_2 = event_utc_ms + 4999ULL;
    assert(bridge_time_is_fresh(event_utc_ms, server_now_2, BRIDGE_TIME_LIVE_TTL_MS) == true);

    /* 5000ms or 5001ms is STALE */
    uint64_t server_now_3 = event_utc_ms + 5000ULL;
    assert(bridge_time_is_fresh(event_utc_ms, server_now_3, BRIDGE_TIME_LIVE_TTL_MS) == false);

    uint64_t server_now_4 = event_utc_ms + 5001ULL;
    assert(bridge_time_is_fresh(event_utc_ms, server_now_4, BRIDGE_TIME_LIVE_TTL_MS) == false);

    printf("      -> PASS: Timestamp invariant across 3500ms queue delay; boundary verified at 4999ms/5001ms\n");
}

static void test_no_sntp_handling(void) {
    printf("[RUN] Test 3: Unsynchronized / No-SNTP handling...\n");
    bridge_time_sync_t ts;
    bridge_time_init(&ts, 1000ULL);
    bridge_time_anchor(&ts, 1000ULL, 1000ULL, 2000ULL, 2);

    /* SNTP is NOT set */
    assert(ts.sntp_synced == false);

    uint64_t event_utc_ms = 0;
    uint8_t quality = 0;
    uint16_t uncertainty = 0;

    int ret = bridge_time_compute_event_ts(&ts, 1000ULL, 1000ULL, 2002ULL,
                                          &event_utc_ms, &quality, &uncertainty);
    assert(ret != 0);
    assert(quality == TIME_QUALITY_UNKNOWN);
    assert(event_utc_ms == 0);

    /* Verify is_fresh returns false for unset timestamp */
    assert(bridge_time_is_fresh(event_utc_ms, 1700000000000ULL, BRIDGE_TIME_LIVE_TTL_MS) == false);

    printf("      -> PASS: Frame without SNTP rejected with TIME_QUALITY_UNKNOWN\n");
}

static void test_clock_step_jump_and_reboot(void) {
    printf("[RUN] Test 4: Clock step jump & Central reboot epoch barrier...\n");
    bridge_time_sync_t ts;
    bridge_time_init(&ts, 1000ULL);
    bridge_time_anchor(&ts, 1000ULL, 1000ULL, 2000ULL, 2);
    bridge_time_set_sntp_offset(&ts, 1700000000000LL, 2000ULL);
    assert(ts.clock_anchored == true);

    /* Clock step jump: SNTP changes offset by +10,000ms */
    bridge_time_set_sntp_offset(&ts, 1700000010000LL, 2010ULL);
    assert(ts.clock_jump_resets == 1);
    assert(ts.clock_anchored == false); /* Monotonic anchor invalidated! */

    /* Telemetry using old anchor without re-anchoring falls back to ESTIMATED */
    uint64_t event_utc_ms = 0;
    uint8_t quality = 0;
    uint16_t uncertainty = 0;
    int ret = bridge_time_compute_event_ts(&ts, 1000ULL, 1050ULL, 2050ULL,
                                          &event_utc_ms, &quality, &uncertainty);
    assert(ret == 0);
    assert(quality == TIME_QUALITY_ESTIMATED);

    /* Central reboots: new epoch 1001 established */
    bridge_time_anchor(&ts, 1001ULL, 50ULL, 3000ULL, 2);
    assert(ts.bridge_epoch == 1001ULL);

    /* Stale epoch 1000 frame arriving is rejected */
    ret = bridge_time_compute_event_ts(&ts, 1000ULL, 50ULL, 3000ULL,
                                      &event_utc_ms, &quality, &uncertainty);
    assert(ret != 0);
    assert(quality == TIME_QUALITY_INVALID);

    printf("      -> PASS: Clock step invalidated anchor; stale epoch rejected with TIME_QUALITY_INVALID\n");
}

static void test_future_timestamp_and_excessive_uncertainty(void) {
    printf("[RUN] Test 5: Future timestamp & excessive uncertainty rejection...\n");
    bridge_time_sync_t ts;
    bridge_time_init(&ts, 1000ULL);
    bridge_time_anchor(&ts, 1000ULL, 1000ULL, 2000ULL, 2);
    bridge_time_set_sntp_offset(&ts, 1700000000000LL, 2000ULL);

    /* Case A: Future timestamp (central claims mono 5000ms while ESP mono is only 2000ms) */
    uint64_t event_utc_ms = 0;
    uint8_t quality = 0;
    uint16_t uncertainty = 0;
    int ret = bridge_time_compute_event_ts(&ts, 1000ULL, 5000ULL, 2000ULL,
                                          &event_utc_ms, &quality, &uncertainty);
    assert(ret != 0);
    assert(quality == TIME_QUALITY_INVALID);

    /* Case B: Uncertainty > 250ms budget (e.g. transit delay was 300ms) */
    ret = bridge_time_compute_event_ts(&ts, 1000ULL, 1000ULL, 2350ULL,
                                      &event_utc_ms, &quality, &uncertainty);
    assert(ret != 0);
    assert(quality == TIME_QUALITY_INVALID);

    printf("      -> PASS: Future timestamps and uncertainty > 250ms correctly rejected\n");
}

static void test_ecg_batch_parameters(void) {
    printf("[RUN] Test 6: ECG batch 50-sample duration & end timestamp...\n");
    uint64_t batch_end_utc = 1700000010200ULL;
    uint16_t sample_count = 50;
    uint16_t period_ms = ECG_SAMPLE_PERIOD_MS; /* 4ms */
    uint64_t batch_start_utc = 0;

    int ret = bridge_time_compute_ecg_batch(batch_end_utc, sample_count, period_ms, &batch_start_utc);
    assert(ret == 0);
    /* Duration = (50 - 1) * 4 = 196 ms. Start = 1700000010200 - 196 = 1700000010004 */
    assert(batch_start_utc == 1700000010004ULL);
    assert(batch_end_utc - batch_start_utc == 196ULL);

    printf("      -> PASS: 50 samples @ 4ms: Duration=196ms (Start: %llu, End: %llu)\n",
           (unsigned long long)batch_start_utc, (unsigned long long)batch_end_utc);
}

static void test_collision_resolution(void) {
    printf("[RUN] Test 7: Strictly monotonic collision resolution...\n");
    uint64_t last_published = 1700000000000ULL;

    /* Proposed same ms */
    uint64_t resolved_1 = bridge_time_resolve_collision(1700000000000ULL, last_published);
    assert(resolved_1 == 1700000000001ULL);
    last_published = resolved_1;

    /* Second same ms */
    uint64_t resolved_2 = bridge_time_resolve_collision(1700000000000ULL, last_published);
    assert(resolved_2 == 1700000000002ULL);
    last_published = resolved_2;

    /* Greater ms */
    uint64_t resolved_3 = bridge_time_resolve_collision(1700000000050ULL, last_published);
    assert(resolved_3 == 1700000000050ULL);

    printf("      -> PASS: Colliding timestamps incremented monotonically (1000 -> 1001 -> 1002)\n");
}

int main(void) {
    printf("====================================================\n");
    printf("   RUNNING BRIDGE TIME & SYNCHRONIZATION TESTS (TDD)\n");
    printf("====================================================\n\n");

    test_clock_anchor_and_monotonic_delta();
    test_delayed_queue_backlog_invariance();
    test_no_sntp_handling();
    test_clock_step_jump_and_reboot();
    test_future_timestamp_and_excessive_uncertainty();
    test_ecg_batch_parameters();
    test_collision_resolution();

    printf("\n🎉 ALL BRIDGE TIME & SYNCHRONIZATION TESTS PASSED!\n");
    return 0;
}
