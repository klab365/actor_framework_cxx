/* ipc_port_state.h — Zephyr per-actor port state. */
#pragma once

#include <zephyr/kernel.h>

struct ipc_actor;
struct ipc_msg;

struct ipc_port_state {
    struct k_msgq msgq;
    struct k_poll_signal signal;
    struct k_thread thread;
    k_thread_stack_t *stack;
    struct k_work_delayable delayed_work;
    struct ipc_msg delayed_msg;
    char *delayed_payload;
    char *send_slot;
    char *recv_slot;
    size_t slot_size;
    struct k_spinlock send_lock;
    struct ipc_actor *owner;
};
