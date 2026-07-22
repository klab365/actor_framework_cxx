/*
 * led_actor.c — LED actor: typed handlers and static routing.
 */
#include "led_actor.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>

/* ── Blink state ─────────────────────────────────────────────────────────── */

static int g_blink_remaining = 0;
static uint32_t g_blink_period;
static uint8_t g_blink_brightness;
static bool g_disabled          = false;
static bool g_restart_pending   = false;
static unsigned g_restart_count = 0;

/* ── Actor instance ──────────────────────────────────────────────────────── */

IPC_ACTOR_DEFINE(led_actor, "led", 512, 5, 8);
IPC_SUPERVISE(led_actor, IPC_SUPERVISE_RESTART);

IPC_START_HOOK(led_actor, led_on_start)
{
    (void) self;
    g_disabled        = false;
    g_blink_remaining = 0;
    if (g_restart_pending) {
        g_restart_pending = false;
        printf("[led] RESTART complete (#%u): state reset and actor is accepting messages again\n",
               g_restart_count);
    } else {
        printf("[led] initial start: state reset\n");
    }
}

IPC_STOP_HOOK(led_actor, led_on_stop)
{
    (void) self;
    if (g_restart_pending) {
        printf("[led] stop hook: preparing restart #%u\n", g_restart_count);
    } else {
        printf("[led] stop hook: shutdown requested\n");
    }
}

IPC_UNKNOWN(led_actor, led_on_unknown)
{
    (void) self;
    printf("[led] unknown message id=0x%x kind=%d\n", msg->id, (int) msg->kind);
}

IPC_FAIL_HOOK(led_actor, led_on_failure)
{
    (void) self;
    g_restart_count++;
    g_restart_pending = true;
    printf("[led] failure hook reason=%d -> supervision policy will restart actor (#%u)\n", reason,
           g_restart_count);
}

/* ── Typed handlers ──────────────────────────────────────────────────────── */

IPC_ACTOR_HANDLE(led_actor, LedOn, led_on_handler)
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

IPC_ACTOR_HANDLE(led_actor, LedOff, led_off_handler)
{
    (void) self;
    (void) msg;
    (void) raw_msg;
    g_blink_remaining = 0;
    printf("[led] LedOff\n");
}

IPC_ACTOR_HANDLE(led_actor, LedBlink, led_blink_handler)
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

IPC_ACTOR_HANDLE(led_actor, GetLedStateRequest, get_led_state_handler)
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

IPC_ACTOR_HANDLE(led_actor, LedFault, led_fault_handler)
{
    (void) self;
    (void) raw_msg;
    printf("[led] FAULT received ch=%u code=0x%x — reporting failure\n", msg->channel,
           msg->error_code);
    int rc = ipc_actor_fail(self, -EIO);
    if (rc != 0) {
        printf("[led] supervision failed: %d\n", rc);
    }
}

int led_actor_module_init(void)
{
    return 0;
}
