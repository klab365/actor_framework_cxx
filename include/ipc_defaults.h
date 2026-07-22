/*
 * ipc_defaults.h — Compile-time default actor payload capacity.
 *
 * IPC_PAYLOAD_SIZE is kept as a compatibility fallback for low-level/manual
 * actor setup. Normal actor declarations should pass an explicit per-actor max
 * payload size, often with IPC_MESSAGE_MAX(...), so each actor queue is
 * sized for the messages that actor actually receives.
 *
 * Override paths:
 *   1. Define IPC_PAYLOAD_SIZE before including <ipc.h>.
 *   2. Pass -DIPC_PAYLOAD_SIZE=... globally.
 */
#pragma once

#ifndef IPC_PAYLOAD_SIZE
#define IPC_PAYLOAD_SIZE 32
#endif
