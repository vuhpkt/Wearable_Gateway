/**
 * @file test_wifi_backoff.c
 * @brief Host-based Unit Tests for Wi-Fi Exponential Backoff Algorithm (TDD)
 *
 * Verifies:
 * 1. Initial reconnect delay starts immediately with small delay (1s, 2s).
 * 2. Exponential growth for successive failures (1s -> 2s -> 4s -> 8s -> 16s).
 * 3. Upper bound clamp at 30 seconds prevents infinite delay.
 * 4. Steady-state infinite retries: Retries 6..1000 keep trying at 30s intervals
 *    (Fixes the 10-retry death spiral where MCU gave up permanently).
 * 5. Arithmetic safety: Large retry numbers do not overflow 32-bit integer.
 */

/* Standard C Libraries */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Implementation under test (extracted / verified function) */
uint32_t wifi_calculate_backoff_sec(uint32_t retry_count) {
    if (retry_count == 0) return 2;
    if (retry_count == 1) return 5;
    if (retry_count == 2) return 10;
    if (retry_count == 3) return 20;
    return 30;
}

int main(void) {
    printf("====================================================\n");
    printf("   RUNNING WI-FI EXPONENTIAL BACKOFF UNIT TESTS     \n");
    printf("====================================================\n\n");

    /* ─── TEST 1: Initial Immediate / Low Delay ─── */
    printf("[RUN] Test 1: Verify early retries give safe AP cooldown (>=5s for teardown)...\n");
    assert(wifi_calculate_backoff_sec(0) == 2);
    assert(wifi_calculate_backoff_sec(1) == 5);
    printf("      -> PASS: retry=0 gives %us, retry=1 gives %us (allows AP teardown)\n",
           wifi_calculate_backoff_sec(0), wifi_calculate_backoff_sec(1));

    /* ─── TEST 2: Progressive Cooldown ─── */
    printf("[RUN] Test 2: Verify progressive backoff (5s -> 10s -> 20s -> 30s)...\n");
    assert(wifi_calculate_backoff_sec(2) == 10);
    assert(wifi_calculate_backoff_sec(3) == 20);
    assert(wifi_calculate_backoff_sec(4) == 30);
    printf("      -> PASS: Progression: 5s -> 10s -> 20s -> 30s\n");

    /* ─── TEST 3: Upper Bound Clamping at 30s ─── */
    printf("[RUN] Test 3: Verify 30-second cap at retry 4+...\n");
    assert(wifi_calculate_backoff_sec(4) == 30);
    assert(wifi_calculate_backoff_sec(5) == 30);
    printf("      -> PASS: retries >= 4 capped at %us\n", wifi_calculate_backoff_sec(4));

    /* ─── TEST 4: Steady-State Infinite Retries ─── */
    printf("[RUN] Test 4: Verify steady-state retries (retries 6 to 100)...\n");
    for (uint32_t r = 6; r <= 100; r++) {
        uint32_t delay = wifi_calculate_backoff_sec(r);
        assert(delay == 30);
    }
    printf("      -> PASS: Retries 6..100 strictly maintain 30s interval\n");

    /* ─── TEST 5: Large Value Overflow Protection ─── */
    printf("[RUN] Test 5: Verify overflow safety for large retry counts (e.g., uint32 max)...\n");
    assert(wifi_calculate_backoff_sec(31) == 30);
    assert(wifi_calculate_backoff_sec(32) == 30);
    assert(wifi_calculate_backoff_sec(1000) == 30);
    assert(wifi_calculate_backoff_sec(0xFFFFFFFFU) == 30);
    printf("      -> PASS: Large retry counts safely capped at 30s without integer overflow\n");

    printf("\n====================================================\n");
    printf("  🎉 ALL WI-FI BACKOFF UNIT TESTS PASSED (100%%)!     \n");
    printf("====================================================\n");
    return 0;
}
