/*
 * ipc_port.h — Generic port interface for the IPC actor framework.
 *
 * No platform-specific types in this file. Implementations live in
 * posix_ipc_port.c / zephyr_ipc_port.c (or any other backend).
 */
#pragma once

#include "ipc.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── Per-actor lifecycle (unchanged) ─────────────────────────────────────── */

int ipc_port_actor_init(struct ipc_actor *a);
int ipc_port_start(struct ipc_actor *a);
void ipc_port_stop_actor(struct ipc_actor *a);

/* ── Per-actor transport (unchanged) ─────────────────────────────────────── */

/*
 * ipc_port_send MUST copy `msg` synchronously; it must not retain a
 * pointer to the message. The publish path reuses one struct ipc_msg
 * across all subscribers.
 */
int ipc_port_send(struct ipc_actor *a, const struct ipc_msg *msg);
int ipc_port_send_isr(struct ipc_actor *a, const struct ipc_msg *msg);
int ipc_port_send_after(struct ipc_actor *a, const struct ipc_msg *msg, uint32_t delay_ms);

/* ── Run-all (blocks on POSIX, no-op on Zephyr) ──────────────────────────── */

/*
 * Block until every actor's thread has exited. On POSIX this is the
 * pthread_join phase after ipc_stop_all() has signalled the threads
 * to exit. On Zephyr the actor threads are owned by the kernel and
 * keep running until the app exits, so this hook returns 0 without
 * joining — ipc_run_all() is a no-op on Zephyr, but calling it is
 * still safe and gives a single portable "wait for shutdown" point
 * for code that wants to be cross-platform.
 */
int ipc_port_run_all(void);
