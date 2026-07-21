/*
 * led_actor.h — Message type definitions handled by the LED actor.
 *
 * Includes commands owned by the LED actor and cross-actor
 * events that the LED subsystem publishes or consumes (LedFault).
 */
#pragma once
#include <ipc.h>

IPC_CMD_DEFINE(LedOn, { uint8_t brightness; });
IPC_CMD_DEFINE(LedOff, { uint8_t _pad; }); /* empty struct — use pad byte */
IPC_CMD_DEFINE(LedBlink, {
    uint32_t period_ms;
    uint8_t brightness;
});

IPC_CMD_DEFINE(GetLedStateRequest, { uint8_t channel; });
IPC_CMD_DEFINE(GetLedStateResponse, {
    uint8_t channel;
    uint8_t on;
    uint8_t brightness;
    uint32_t on_time_ms;
});

/* Hardware/driver fault notification. Channel identifies the source. */
IPC_EVENT_DEFINE(LedFault, {
    uint32_t error_code;
    uint8_t channel;
});