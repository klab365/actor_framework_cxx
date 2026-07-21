/* ipc_port_state.h — POSIX per-actor port state. */
#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ipc_msg;

struct ipc_port_state {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;

    /* Ring buffer (calloc'd once in ipc_port_start) */
    struct ipc_msg *ring;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;

    bool running;
    bool joined;

    /* Delayed send: single pending delayed message per actor */
    pthread_t delay_thread;
    bool delay_active;
    pthread_mutex_t delay_lock;
    pthread_cond_t delay_cond;
    struct ipc_msg delay_msg;
    uint32_t delay_ms;
    bool delay_cancel;
};
