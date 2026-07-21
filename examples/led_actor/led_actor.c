/*
 * led_actor.c — LED actor: typed handlers, dispatch, registration.
 */
#include "led_actor.h"
#include <stdbool.h>
#include <stdio.h>

/* ── Blink state ─────────────────────────────────────────────────────────── */

/* Finite blink: 5 ticks, then stops. A real driver would cancel the
 * pending delayed msg on LedOff; that's a framework TODO (see refactors.md). */
static int g_blink_remaining = 0;
static uint32_t g_blink_period;
static uint8_t g_blink_brightness;
static bool g_disabled = false; /* set on LedFault; ignore further cmds */

/* ── Typed handlers ──────────────────────────────────────────────────────── */

IPC_HANDLE(LedOn, led_on_handler)
{
    (void) self;
    (void) raw_msg;
    if (g_disabled) {
        printf("[led] LedOn ignored (disabled)\n");
        return;
    }
    g_blink_remaining = 0;
    printf("[led] LedOn brightness=%u\n", msg->brightness);
}

IPC_HANDLE(LedOff, led_off_handler)
{
    (void) self;
    (void) msg;
    (void) raw_msg;
    g_blink_remaining = 0;
    printf("[led] LedOff\n");
}

IPC_HANDLE(LedBlink, led_blink_handler)
{
    (void) self;
    (void) raw_msg;
    if (g_disabled) {
        printf("[led] LedBlink ignored (disabled)\n");
        return;
    }
    if (g_blink_remaining == 0) {
        g_blink_period     = msg->period_ms;
        g_blink_brightness = msg->brightness;
        g_blink_remaining  = 5; /* finite demo */
    }
    if (g_blink_remaining > 0) {
        printf("[led] LedBlink tick %d/%d period=%u ms brightness=%u\n", 6 - g_blink_remaining, 5,
               g_blink_period, g_blink_brightness);
        g_blink_remaining--;
        LedBlink_payload_t blink = {
            .period_ms  = g_blink_period,
            .brightness = g_blink_brightness,
        };
        ipc_send_after(LedBlink, g_blink_period, blink);
    }
}

IPC_HANDLE(GetLedStateRequest, get_led_state_handler)
{
    (void) self;
    (void) raw_msg;
    GetLedStateResponse_payload_t resp = {
        .channel    = msg->channel,
        .on         = g_disabled ? 0 : 1,
        .brightness = 80,
        .on_time_ms = 12345,
    };
    ipc_send(GetLedStateResponse, resp);
}

IPC_HANDLE(LedFault, led_fault_handler)
{
    (void) self;
    (void) raw_msg;
    printf("[led] FAULT received ch=%u code=0x%x — disabling self\n", msg->channel,
           msg->error_code);
    g_disabled        = true;
    g_blink_remaining = 0;
}

/* ── Dispatch ────────────────────────────────────────────────────────────── */

static void led_handler(struct ipc_actor *self, const struct ipc_msg *msg)
{
    IPC_DISPATCH_TO(msg, LedOn, led_on_handler)
    IPC_DISPATCH_TO(msg, LedOff, led_off_handler)
    IPC_DISPATCH_TO(msg, LedBlink, led_blink_handler)
    IPC_DISPATCH_TO(msg, GetLedStateRequest, get_led_state_handler)
    IPC_DISPATCH_TO(msg, LedFault, led_fault_handler)
    IPC_DISPATCH_IGNORE_UNKNOWN();
}

/* ── Actor instance ──────────────────────────────────────────────────────── */

IPC_ACTOR_DEFINE(led_actor, "led", led_handler, 512, 5, 8);

int led_actor_module_init(void)
{
    // registering handlers for actor.
    ipc_register(&led_actor, &LedOn);
    ipc_register(&led_actor, &LedOff);
    ipc_register(&led_actor, &LedBlink);
    ipc_register(&led_actor, &GetLedStateRequest);
    ipc_subscribe(&led_actor, &LedFault);

    return 0;
}
