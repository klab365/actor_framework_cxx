/*
 * button_actor.h — Pure message type definitions for the button actor.
 * No actor references. Safe to include from any module.
 */
#pragma once
#include <ipc.h>

IPC_EVENT_DEFINE(ButtonClick, { uint8_t button_id; });
IPC_EVENT_DEFINE(ButtonDoubleClick, { uint8_t button_id; });
IPC_EVENT_DEFINE(ButtonHold, {
    uint8_t  button_id;
    uint32_t hold_ms;
});
