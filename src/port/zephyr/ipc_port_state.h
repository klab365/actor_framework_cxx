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
    /* Serializes ipc_port_send_after and guarantees any in-flight delayed
     * work has finished before delayed_msg/delayed_payload are overwritten. */
    struct k_mutex delay_lock;
    struct k_work_sync delayed_work_sync;
    struct ipc_actor *owner;
};
