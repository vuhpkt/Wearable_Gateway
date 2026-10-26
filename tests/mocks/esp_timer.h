#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int64_t esp_timer_get_time(void) {
    return 100000000LL; /* Mock 100 seconds */
}

#ifdef __cplusplus
}
#endif
