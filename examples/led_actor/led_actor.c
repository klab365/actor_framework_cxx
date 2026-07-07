/*
 * led_actor.c — LED actor: typed handlers, dispatch, registration.
 */
#include "led_actor.h"
#include <stdbool.h>
#include <stdio.h>

/* ── Blink state ─────────────────────────────────────────────────────────── */

/* Finite blink: 5 ticks, then stops. A real driver would cancel the
 * pending delayed msg on LedOff; that's a framework TODO (see refactors.md). */
static int      g_blink_remaining = 0;
static uint32_t g_blink_period;
static uint8_t  g_blink_brightness;
static bool     g_disabled = false; /* set on LedFault; ignore further cmds */

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
        ipc_send_after(LedBlink, g_blink_period, .period_ms = g_blink_period,
                       .brightness = g_blink_brightness);
    }
}

IPC_HANDLE(GetLedState, get_led_state_handler)
{
    (void) self;
    (void) msg;
    ipc_reply(raw_msg, GetLedState, .on = g_disabled ? 0 : 1, .brightness = 80,
              .on_time_ms = 12345);
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
    IPC_DISPATCH_TO(msg, GetLedState, get_led_state_handler)
    IPC_DISPATCH_TO(msg, LedFault, led_fault_handler)
    { /* unknown */
    }
}

/* ── Actor instance ──────────────────────────────────────────────────────── */

static struct ipc_actor led_actor;

int                     led_actor_module_init(void)
{
    struct ipc_actor_cfg cfg = {
        .stack_size  = 512,
        .priority    = 5,
        .queue_depth = 8,
    };
    ipc_actor_init(&led_actor, "led", led_handler, cfg);

    // registering handlers for actor.
    IPC_REGISTER(&led_actor, LedOn);
    IPC_REGISTER(&led_actor, LedOff);
    IPC_REGISTER(&led_actor, LedBlink);
    IPC_REGISTER(&led_actor, GetLedState);
    IPC_SUBSCRIBE(&led_actor, LedFault);

    return 0;
}
