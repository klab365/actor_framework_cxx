/*
 * app_actor.c — Consumer actor: static event handlers and command sends.
 */
#include "button_actor.h"
#include "led_actor.h"
#include <stdio.h>

/* ── Actor instance ──────────────────────────────────────────────────────── */

IPC_ACTOR_DEFINE(app_actor, "app", 1024, 4, 32,
                 IPC_MESSAGE_MAX(LedFault, GetLedStateResponse, ButtonClick, ButtonDoubleClick,
                                 ButtonHold));

IPC_START_HOOK(app_actor, app_on_start)
{
    (void) self;
    printf("[app] start hook\n");
}

IPC_STOP_HOOK(app_actor, app_on_stop)
{
    (void) self;
    printf("[app] stop hook\n");
}

IPC_UNKNOWN(app_actor, app_on_unknown)
{
    (void) self;
    printf("[app] unknown message id=0x%x kind=%d\n", msg->id, (int) msg->kind);
}

/* ── Typed handlers ──────────────────────────────────────────────────────── */

IPC_ACTOR_HANDLE(app_actor, LedFault, led_fault_handler)
{
    (void) self;
    printf("[app] LED fault ch=%u code=0x%x\n", msg->channel, msg->error_code);
    (void) raw_msg;
}

IPC_ACTOR_RESPONSE_HANDLE(app_actor, GetLedStateRequest, GetLedStateResponse, on_led_state)
{
    (void) self;
    (void) raw_msg;
    if (result != 0) {
        printf("[app] GetLedState ask failed: %d\n", result);
        return;
    }

    printf("[app] GetLedStateResponse ch=%u on=%u brightness=%u on_time_ms=%u\n", msg->channel,
           msg->on, msg->brightness, msg->on_time_ms);
}

IPC_ACTOR_HANDLE(app_actor, ButtonClick, on_click)
{
    (void) self;
    (void) raw_msg;
    printf("[app] button click btn=%u → LedOn\n", msg->button_id);
    LedOn_payload_t on_cmd = {.brightness = 50 + msg->button_id * 20};
    ipc_send(LedOn, on_cmd);
}

IPC_ACTOR_HANDLE(app_actor, ButtonDoubleClick, on_double_click)
{
    (void) self;
    (void) raw_msg;
    printf("[app] button double-click btn=%u → LedBlink fast\n", msg->button_id);
    LedBlink_payload_t blink_cmd = {.period_ms = 150, .brightness = 200};
    ipc_send(LedBlink, blink_cmd);
}

IPC_ACTOR_HANDLE(app_actor, ButtonHold, on_hold)
{
    (void) self;
    (void) raw_msg;
    printf("[app] button hold btn=%u (%u ms) → LedOff\n", msg->button_id, msg->hold_ms);
    LedOff_payload_t off_cmd = {._pad = 0};
    ipc_send(LedOff, off_cmd);
}

int app_actor_module_init(void)
{
    return 0;
}

void app_run(void)
{
    /* Keep the correlation ID when this request may need cancellation. The
     * callback still runs in app_actor's context on either reply or timeout. */
    GetLedStateRequest_payload_t req = {.channel = 0};
    uint32_t ask_id                  = 0;
    int rc = ipc_ask_with_id(&app_actor, GetLedStateRequest, req, on_led_state, &ask_id, 1000);
    if (rc != 0) {
        printf("[app] GetLedState ask failed: %d\n", rc);
    } else {
        printf("[app] GetLedState ask queued (id=%u)\n", ask_id);
    }

    /* An unavailable channel demonstrates ipc_reply_error(): on_led_state
     * receives result == -ENODEV and must not read msg on that path. */
    GetLedStateRequest_payload_t unavailable = {.channel = 2};
    rc = ipc_ask_with(&app_actor, GetLedStateRequest, unavailable, on_led_state, 1000);
    if (rc != 0) {
        printf("[app] unavailable-channel ask failed to queue: %d\n", rc);
    }

    /* EVENT — broadcast, no target */
    LedFault_payload_t fault = {.error_code = 0xDEAD, .channel = 1};
    ipc_publish(LedFault, fault);
}
