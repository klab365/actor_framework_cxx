/*
 * ipc_defaults.h — Compile-time message payload default.
 *
 * IPC_PAYLOAD_SIZE controls the inline payload bytes carried by
 * struct ipc_msg; IPC_*_DEFINE payloads must fit this cap at compile time.
 *
 * Override paths:
 *   1. Define IPC_PAYLOAD_SIZE before including <ipc.h>.
 *   2. Pass -DIPC_PAYLOAD_SIZE=... globally.
 *   3. On Zephyr, set CONFIG_ACTOR_PAYLOAD_SIZE; the Zephyr ipc_config.h
 *      maps it before including this default.
 */
#pragma once

#ifndef IPC_PAYLOAD_SIZE
#define IPC_PAYLOAD_SIZE 32
#endif
