/*
 * button_actor.c — Simulated button: publishes Click / DoubleClick / Hold events
 *                  on a 1 s tick. Demonstrates self-driven actor (send_after)
 *                  and finite-state behaviour purely via incoming messages.
 */
#include "button_actor.h"
#include "led_actor.h"
#include <stdint.h>
#include <stdio.h>

/* ── Tick: scheduler heartbeat sent to ourselves ─────────────────────────── */

IPC_CMD_DEFINE(ButtonTick, { uint32_t seq; });

/* ── Actor state ─────────────────────────────────────────────────────────── */

typedef enum {
    BTN_IDLE, /* nothing pressed                       */
    BTN_FIRST_DOWN, /* one click seen, waiting for 2nd       */
    BTN_HELD, /* button held long enough → Hold event  */
} btn_state_t;

static btn_state_t g_state   = BTN_IDLE;
static uint32_t g_ticks      = 0; /* ms since first press          */
static uint8_t g_btn_id      = 0; /* which "physical" button       */
static unsigned g_presses    = 0; /* total presses simulated      */
static unsigned g_hold_ticks = 0; /* ticks spent in HELD state    */
static bool g_fault_fired    = false; /* fault published this hold */

/* ── Tick handler ────────────────────────────────────────────────────────── */

static void on_tick(struct ipc_actor *self)
{
    (void) self;
    g_ticks += 250; /* each tick is 250 ms */

    /* Re-arm the next tick */
    ButtonTick_payload_t tick = {.seq = g_ticks};
    ipc_send_after(ButtonTick, 250, tick);

    switch (g_state) {
    case BTN_IDLE:
        g_btn_id = (uint8_t) ((g_presses++) % 3); /* cycle through 0,1,2 */
        g_state  = BTN_FIRST_DOWN;
        g_ticks  = 0;
        printf("[btn] press (id=%u) — waiting for outcome\n", g_btn_id);
        break;

    case BTN_FIRST_DOWN:
        /* Outcome determined by which "physical" button id this is:
         *   id 0 → single click after 1 tick (250 ms)
         *   id 1 → double click after 1 tick (emulated 2nd press)
         *   id 2 → hold start after 2 ticks (500 ms)
         */
        if (g_btn_id == 0 && g_ticks >= 250) {
            g_state = BTN_IDLE;
            printf("[btn] single click (id=%u)\n", g_btn_id);
            ButtonClick_payload_t click = {.button_id = g_btn_id};
            ipc_publish(ButtonClick, click);
        } else if (g_btn_id == 1 && g_ticks >= 250) {
            g_state = BTN_IDLE;
            printf("[btn] double-click (id=%u)\n", g_btn_id);
            ButtonDoubleClick_payload_t dbl = {.button_id = g_btn_id};
            ipc_publish(ButtonDoubleClick, dbl);
        } else if (g_btn_id == 2 && g_ticks >= 500) {
            g_state = BTN_HELD;
            g_ticks = 0;
            printf("[btn] hold start (id=%u)\n", g_btn_id);
        }
        break;

    case BTN_HELD:
        if (g_ticks >= 1000) {
            /* Emit Hold event every second while held */
            printf("[btn] hold tick (id=%u, %u ms)\n", g_btn_id, g_ticks);
            ButtonHold_payload_t hold = {.button_id = g_btn_id, .hold_ms = g_ticks};
            ipc_publish(ButtonHold, hold);
            g_ticks = 0;
        }
        /* Stuck-button detection: after 4 hold-ticks (4 s) report a fault
         * (channel = button id, code = 0xBADC0DE), then release. */
        if (g_ticks == 0 && g_hold_ticks >= 4 && !g_fault_fired) {
            printf("[btn] STUCK button (id=%u) — publishing LedFault\n", g_btn_id);
            LedFault_payload_t fault = {.error_code = 0xBADC0DEu, .channel = g_btn_id};
            ipc_publish(LedFault, fault);
            g_fault_fired = true;
        }
        if (g_ticks == 0 && g_hold_ticks++ >= 5) {
            g_hold_ticks  = 0;
            g_fault_fired = false;
            g_state       = BTN_IDLE;
            g_presses     = (g_presses + 1) % 3; /* advance rotation */
            printf("[btn] release (id=%u)\n", g_btn_id);
        }
        break;
    }
}

/* ── Typed handlers ──────────────────────────────────────────────────────── */

IPC_HANDLE(ButtonTick, on_tick_msg)
{
    (void) raw_msg;
    (void) msg;
    (void) self;

    on_tick(self);
}

IPC_HANDLE(LedFault, on_fault)
{
    (void) self;
    (void) raw_msg;
    /* Button actor reacts to faults (e.g., could reset its own debounce
     * state here). For the demo we just log it.  We must NOT re-publish
     * LedFault from this handler — that would create a feedback loop. */
    printf("[btn] received fault ch=%u code=0x%x — resetting debounce\n", msg->channel,
           msg->error_code);
    g_state       = BTN_IDLE;
    g_ticks       = 0;
    g_hold_ticks  = 0;
    g_fault_fired = false;
}

/* ── Actor instance ──────────────────────────────────────────────────────── */

static const struct ipc_actor_handler_entry button_handlers[] = {
    IPC_ON(ButtonTick, on_tick_msg),
    IPC_ON(LedFault, on_fault),
};

IPC_ACTOR_DEFINE(button_actor, "button", 1024, 5, 16, IPC_ACTOR_HANDLERS(button_handlers));

int button_actor_module_init(void)
{
    return 0;
}

/* Called by main() once threads are up — starts the 1 s tick loop. */
void button_actor_kick(void)
{
    ButtonTick_payload_t first_tick = {.seq = 1};
    ipc_send_after(ButtonTick, 1000, first_tick);
}
