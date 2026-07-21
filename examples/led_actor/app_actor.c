/*
 * app_actor.c — Consumer actor: static event handlers and command sends.
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

IPC_HANDLE(GetLedStateResponse, on_led_state)
{
    (void) self;
    (void) raw_msg;
    printf("[app] GetLedStateResponse ch=%u on=%u brightness=%u on_time_ms=%u\n", msg->channel,
           msg->on, msg->brightness, msg->on_time_ms);
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

/* ── Actor instance ──────────────────────────────────────────────────────── */

static const struct ipc_actor_handler_entry app_handlers[] = {
    IPC_ON(LedFault, led_fault_handler), IPC_ON(GetLedStateResponse, on_led_state),
    IPC_ON(ButtonClick, on_click),       IPC_ON(ButtonDoubleClick, on_double_click),
    IPC_ON(ButtonHold, on_hold),
};

IPC_ACTOR_DEFINE(app_actor, "app", 1024, 4, 32, IPC_ACTOR_HANDLERS(app_handlers));

int app_actor_module_init(void)
{
    return 0;
}

void app_run(void)
{
    /* Async request/response: ask LED actor for state, then handle
     * GetLedStateResponse when it arrives in app_handler(). */
    GetLedStateRequest_payload_t req = {.channel = 0};
    int rc                           = ipc_send(GetLedStateRequest, req);
    if (rc != 0) {
        printf("[app] GetLedStateRequest failed: %d\n", rc);
    }

    /* EVENT — broadcast, no target */
    LedFault_payload_t fault = {.error_code = 0xDEAD, .channel = 1};
    ipc_publish(LedFault, fault);
}
