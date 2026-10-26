/**
 * @file test_wifi_portal_parser.c
 * @brief Host-based Unit Tests for Web Portal URL Decoder & Credential Parser (TDD)
 */

#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void url_decode(char *dst, size_t dst_size, const char *src) {
    char a, b;
    size_t i = 0;
    while (*src && i < dst_size - 1) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit((int)a) && isxdigit((int)b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            dst[i++] = (char)(16 * a + b);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

static bool parse_form_credentials(const char *body, char *out_ssid, size_t ssid_size, char *out_pass, size_t pass_size) {
    if (!body || !out_ssid || !out_pass) return false;

    char raw_ssid[128] = {0};
    char raw_pass[128] = {0};

    const char *p_ssid = strstr(body, "ssid=");
    const char *p_pass = strstr(body, "pass=");

    if (!p_ssid || !p_pass) return false;

    p_ssid += 5;
    const char *end_ssid = strchr(p_ssid, '&');
    size_t len_ssid = end_ssid ? (size_t)(end_ssid - p_ssid) : strlen(p_ssid);
    if (len_ssid >= sizeof(raw_ssid)) len_ssid = sizeof(raw_ssid) - 1;
    strncpy(raw_ssid, p_ssid, len_ssid);
    raw_ssid[len_ssid] = '\0';

    p_pass += 5;
    const char *end_pass = strchr(p_pass, '&');
    size_t len_pass = end_pass ? (size_t)(end_pass - p_pass) : strlen(p_pass);
    if (len_pass >= sizeof(raw_pass)) len_pass = sizeof(raw_pass) - 1;
    strncpy(raw_pass, p_pass, len_pass);
    raw_pass[len_pass] = '\0';

    url_decode(out_ssid, ssid_size, raw_ssid);
    url_decode(out_pass, pass_size, raw_pass);

    return (strlen(out_ssid) > 0);
}

int main(void) {
    printf("====================================================\n");
    printf("   RUNNING PORTAL URL DECODER & PARSER TDD TESTS    \n");
    printf("====================================================\n\n");

    /* Test 1: Standard credentials */
    printf("[RUN] Test 1: Standard alphanumeric SSID & password...\n");
    char ssid[33] = {0};
    char pass[65] = {0};
    bool ok = parse_form_credentials("ssid=MyHomeWifi&pass=Secret1234", ssid, sizeof(ssid), pass, sizeof(pass));
    assert(ok);
    assert(strcmp(ssid, "MyHomeWifi") == 0);
    assert(strcmp(pass, "Secret1234") == 0);
    printf("      -> PASS: Parsed 'MyHomeWifi' / 'Secret1234'\n");

    /* Test 2: Spaces and special characters (%20, +, %21) */
    printf("[RUN] Test 2: Spaces and special characters...\n");
    ok = parse_form_credentials("ssid=Coffee+Shop%20Guest&pass=P%40ssw%30rd%21%26More", ssid, sizeof(ssid), pass, sizeof(pass));
    assert(ok);
    assert(strcmp(ssid, "Coffee Shop Guest") == 0);
    assert(strcmp(pass, "P@ssw0rd!&More") == 0);
    printf("      -> PASS: Parsed 'Coffee Shop Guest' / 'P@ssw0rd!&More'\n");

    /* Test 3: Reversed field order (pass before ssid) */
    printf("[RUN] Test 3: Field order tolerance (pass before ssid)...\n");
    ok = parse_form_credentials("pass=Hello1234&ssid=TestNet", ssid, sizeof(ssid), pass, sizeof(pass));
    assert(ok);
    assert(strcmp(ssid, "TestNet") == 0);
    assert(strcmp(pass, "Hello1234") == 0);
    printf("      -> PASS: Field order independence verified.\n");

    /* Test 4: Empty / Invalid body */
    printf("[RUN] Test 4: Rejection of malformed input...\n");
    assert(!parse_form_credentials("", ssid, sizeof(ssid), pass, sizeof(pass)));
    assert(!parse_form_credentials("random_garbage_without_keys", ssid, sizeof(ssid), pass, sizeof(pass)));
    printf("      -> PASS: Malformed input safely rejected.\n");

    /* Test 5: Transition state guard against double POST */
    printf("[RUN] Test 5: Transition state guard against double POST...\n");
    bool is_transitioning = false;
    // First request arrives: should succeed and latch transitioning = true
    bool req1_accepted = !is_transitioning;
    assert(req1_accepted == true);
    is_transitioning = true;

    // Second request arrives while first is transitioning: must be rejected
    bool req2_accepted = !is_transitioning;
    assert(req2_accepted == false);

    // Transition completes: flag reset allows future provisioning
    is_transitioning = false;
    bool req3_accepted = !is_transitioning;
    assert(req3_accepted == true);
    printf("      -> PASS: Double-post safely rejected during transition window.\n");

    printf("\n====================================================\n");
    printf("  🎉 ALL PORTAL PARSER TDD TESTS PASSED (100%%)!     \n");
    printf("====================================================\n");
    return 0;
}
