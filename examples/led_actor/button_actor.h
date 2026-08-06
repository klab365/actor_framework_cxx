/*
 * button_actor.h — Pure message type definitions for the button actor.
 * No actor references. Safe to include from any module.
 */
#pragma once
#include <ipc.h>

IPC_EVENT_DEFINE_LOCAL(ButtonClick, { uint8_t button_id; });
IPC_EVENT_DEFINE_LOCAL(ButtonDoubleClick, { uint8_t button_id; });
IPC_EVENT_DEFINE_LOCAL(ButtonHold, {
    uint8_t button_id;
    uint32_t hold_ms;
});
