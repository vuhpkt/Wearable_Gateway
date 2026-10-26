#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>

#define ARG_UNUSED(x) ((void)(x))
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

extern int64_t g_host_uptime_ms;
static inline int64_t k_uptime_get(void) {
    return g_host_uptime_ms;
}

typedef int32_t k_timeout_t;
#define K_NO_WAIT 0
#define K_FOREVER (-1)
#define K_MSEC(ms) (ms)

#define K_PRIO_PREEMPT(prio) (prio)
#define K_THREAD_DEFINE(name, stack_size, entry, p1, p2, p3, prio, options, delay) \
    /* Stub for host unit test */

struct k_msgq {
    uint8_t *buffer;
    size_t msg_size;
    uint32_t max_msgs;
    uint32_t num_used;
    uint32_t read_ptr;
    uint32_t write_ptr;
};

#define K_MSGQ_DEFINE(q_name, q_msg_size, q_max_msgs, q_align)                       \
    static uint8_t q_name##_buf[(q_msg_size) * (q_max_msgs)];                       \
    static struct k_msgq q_name = {                                                  \
        .buffer = q_name##_buf,                                                      \
        .msg_size = (q_msg_size),                                                    \
        .max_msgs = (q_max_msgs),                                                    \
        .num_used = 0,                                                               \
        .read_ptr = 0,                                                               \
        .write_ptr = 0,                                                              \
    }

static inline int k_msgq_put(struct k_msgq *msgq, const void *data, k_timeout_t timeout) {
    ARG_UNUSED(timeout);
    if (!msgq || !data) {
        return -EINVAL;
    }
    if (msgq->num_used >= msgq->max_msgs) {
        return -ENOMSG;
    }
    memcpy(msgq->buffer + (msgq->write_ptr * msgq->msg_size), data, msgq->msg_size);
    msgq->write_ptr = (msgq->write_ptr + 1) % msgq->max_msgs;
    msgq->num_used++;
    return 0;
}

static inline int k_msgq_get(struct k_msgq *msgq, void *data, k_timeout_t timeout) {
    ARG_UNUSED(timeout);
    if (!msgq || !data) {
        return -EINVAL;
    }
    if (msgq->num_used == 0) {
        return -ENOMSG;
    }
    memcpy(data, msgq->buffer + (msgq->read_ptr * msgq->msg_size), msgq->msg_size);
    msgq->read_ptr = (msgq->read_ptr + 1) % msgq->max_msgs;
    msgq->num_used--;
    return 0;
}

static inline uint32_t k_msgq_num_used_get(struct k_msgq *msgq) {
    return msgq ? msgq->num_used : 0;
}

static inline void k_msgq_purge(struct k_msgq *msgq) {
    if (msgq) {
        msgq->num_used = 0;
        msgq->read_ptr = 0;
        msgq->write_ptr = 0;
    }
}

/* k_timer mock for host testing */
struct k_timer;
typedef void (*k_timer_expiry_t)(struct k_timer *timer);
typedef void (*k_timer_stop_t)(struct k_timer *timer);

struct k_timer {
    k_timer_expiry_t expiry_fn;
    k_timer_stop_t stop_fn;
    union {
        bool active;
        bool is_active;
    };
    int32_t duration_ms;
};

static inline void k_timer_init(struct k_timer *timer, k_timer_expiry_t expiry_fn, k_timer_stop_t stop_fn) {
    if (timer) {
        timer->expiry_fn = expiry_fn;
        timer->stop_fn = stop_fn;
        timer->is_active = false;
        timer->duration_ms = 0;
    }
}

static inline void k_timer_start(struct k_timer *timer, k_timeout_t duration, k_timeout_t period) {
    ARG_UNUSED(period);
    if (timer) {
        timer->is_active = true;
        timer->duration_ms = (int32_t)duration;
    }
}

static inline void k_timer_stop(struct k_timer *timer) {
    if (timer) {
        timer->is_active = false;
        if (timer->stop_fn) timer->stop_fn(timer);
    }
}

/* k_work mock for host testing */
struct k_work;
typedef void (*k_work_handler_t)(struct k_work *work);

struct k_work {
    k_work_handler_t handler;
    bool is_pending;
    bool is_running;
};

extern uint32_t g_k_work_submit_count;
extern uint32_t g_k_work_cancel_count;

static inline void k_work_init(struct k_work *work, k_work_handler_t handler) {
    if (work) {
        work->handler = handler;
        work->is_pending = false;
        work->is_running = false;
    }
}

static inline int k_work_submit(struct k_work *work) {
    if (!work) return -EINVAL;
    work->is_pending = true;
    g_k_work_submit_count++;
    return 1;
}

static inline int k_work_cancel(struct k_work *work) {
    if (!work) return 0;
    bool was_pending = work->is_pending;
    work->is_pending = false;
    g_k_work_cancel_count++;
    return was_pending ? 1 : 0;
}
