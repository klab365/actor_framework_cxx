/*
 * app_actor.c — Consumer actor: subscribes to LedFault events,
 *               sends cmds, and performs a query.
 */
#include "button_actor.h"
#include "led_actor.h"
#include <stdio.h>

/* ── Typed handlers ──────────────────────────────────────────────────────── */

IPC_HANDLE(LedFault, led_fault_handler)
{
    (void) self;
    printf("[app] LED fault ch=%u code=0x%x\n", msg->channel, msg->error_code);
    (void) raw_msg;
}

IPC_HANDLE(ButtonClick, on_click)
{
    (void) self;
    (void) raw_msg;
    printf("[app] button click btn=%u → LedOn\n", msg->button_id);
    LedOn_payload_t on_cmd = {.brightness = 50 + msg->button_id * 20};
    ipc_send(LedOn, on_cmd);
}

IPC_HANDLE(ButtonDoubleClick, on_double_click)
{
    (void) self;
    (void) raw_msg;
    printf("[app] button double-click btn=%u → LedBlink fast\n", msg->button_id);
    LedBlink_payload_t blink_cmd = {.period_ms = 150, .brightness = 200};
    ipc_send(LedBlink, blink_cmd);
}

IPC_HANDLE(ButtonHold, on_hold)
{
    (void) self;
    (void) raw_msg;
    printf("[app] button hold btn=%u (%u ms) → LedOff\n", msg->button_id, msg->hold_ms);
    LedOff_payload_t off_cmd = {._pad = 0};
    ipc_send(LedOff, off_cmd);
}

/* ── Dispatch ────────────────────────────────────────────────────────────── */

static void app_handler(struct ipc_actor *self, const struct ipc_msg *msg)
{
    IPC_DISPATCH_TO(msg, LedFault, led_fault_handler)
    IPC_DISPATCH_TO(msg, ButtonClick, on_click)
    IPC_DISPATCH_TO(msg, ButtonDoubleClick, on_double_click)
    IPC_DISPATCH_TO(msg, ButtonHold, on_hold)
    { /* ignore */
    }
}

/* ── Actor instance ──────────────────────────────────────────────────────── */

static struct ipc_actor app_actor;

int app_actor_module_init(void)
{
    struct ipc_actor_cfg cfg = {
        .stack_size  = 1024,
        .priority    = 4,
        .queue_depth = 32,
    };

    ipc_actor_init(&app_actor, "app", app_handler, cfg);

    IPC_SUBSCRIBE(&app_actor, LedFault);
    IPC_SUBSCRIBE(&app_actor, ButtonClick);
    IPC_SUBSCRIBE(&app_actor, ButtonDoubleClick);
    IPC_SUBSCRIBE(&app_actor, ButtonHold);
    return 0;
}

void app_run(void)
{
    /* QUERY — blocks until led_actor replies or timeout */
    GetLedState_response_t state;
    GetLedState_payload_t req = {.channel = 0};
    int rc                    = ipc_query(GetLedState, &state, IPC_TIMEOUT_MS(100), req);
    if (rc == 0) {
        printf("[app] GetLedState: on=%u brightness=%u on_time_ms=%u\n", state.on, state.brightness,
               state.on_time_ms);
    } else {
        printf("[app] GetLedState failed: %d\n", rc);
    }

    /* EVENT — broadcast, no target */
    LedFault_payload_t fault = {.error_code = 0xDEAD, .channel = 1};
    ipc_publish(LedFault, fault);
}
