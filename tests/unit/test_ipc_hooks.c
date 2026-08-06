#include "ipc.h"
#include "ipc_internal.h"

IPC_CMD_DEFINE_LOCAL(HookUnhandledCmd, { int value; });
IPC_CMD_DEFINE_LOCAL(HookLocalIrq, { uint8_t pin; });

IPC_ACTOR_DEFINE(hook_actor, "hook", 1024, 0, 4, IPC_MESSAGE_MAX(HookUnhandledCmd, HookLocalIrq));

static int hook_start_count;
static int hook_stop_count;
static int hook_unknown_count;
static int hook_failure_count;
static int hook_failure_reason;
static uint32_t hook_unknown_id;
static int hook_local_irq_count;
static uint8_t hook_local_irq_pin;

IPC_SUPERVISE(hook_actor, IPC_SUPERVISE_RESTART);

IPC_START_HOOK(hook_actor, hook_on_start)
{
    (void) self;
    hook_start_count++;
}

IPC_STOP_HOOK(hook_actor, hook_on_stop)
{
    (void) self;
    hook_stop_count++;
}

IPC_UNKNOWN(hook_actor, hook_on_unknown)
{
    (void) self;
    hook_unknown_count++;
    hook_unknown_id = msg->id;
}

IPC_FAIL_HOOK(hook_actor, hook_on_failure)
{
    (void) self;
    hook_failure_count++;
    hook_failure_reason = reason;
}

IPC_ACTOR_HANDLE(hook_actor, HookLocalIrq, hook_on_local_irq)
{
    (void) self;
    (void) raw_msg;
    hook_local_irq_count++;
    hook_local_irq_pin = msg->pin;
}

void test_ipc_hooks_reset_counters(void)
{
    hook_start_count     = 0;
    hook_stop_count      = 0;
    hook_unknown_count   = 0;
    hook_failure_count   = 0;
    hook_failure_reason  = 0;
    hook_unknown_id      = 0;
    hook_local_irq_count = 0;
    hook_local_irq_pin   = 0;
}

void test_ipc_hooks_register_actor(void)
{
    _ipc_actor_register_static(&hook_actor);
}

void test_ipc_hooks_register_local_irq_handler(void)
{
    _ipc_actor_register_handler_static(&hook_actor, &HookLocalIrq,
                                       hook_actor_hook_on_local_irq_ipc_adapter);
}

struct ipc_actor *test_ipc_hooks_actor(void)
{
    return &hook_actor;
}

int test_ipc_hooks_start_count(void)
{
    return hook_start_count;
}

int test_ipc_hooks_stop_count(void)
{
    return hook_stop_count;
}

int test_ipc_hooks_unknown_count(void)
{
    return hook_unknown_count;
}

uint32_t test_ipc_hooks_unknown_id(void)
{
    return hook_unknown_id;
}

int test_ipc_hooks_failure_count(void)
{
    return hook_failure_count;
}

int test_ipc_hooks_failure_reason(void)
{
    return hook_failure_reason;
}

int test_ipc_hooks_local_irq_count(void)
{
    return hook_local_irq_count;
}

uint8_t test_ipc_hooks_local_irq_pin(void)
{
    return hook_local_irq_pin;
}

uint32_t test_ipc_hooks_local_irq_id(void)
{
    return HookLocalIrq.id;
}

int test_ipc_hooks_send_local_irq(uint8_t pin)
{
    HookLocalIrq_payload_t payload = {.pin = pin};
    return ipc_send_to(&hook_actor, HookLocalIrq, payload);
}

void test_ipc_hooks_dispatch_unknown(uint32_t msg_id)
{
    struct ipc_msg msg = {
        .id   = msg_id,
        .kind = IPC_CMD,
    };
    hook_actor.handler(&hook_actor, &msg);
}
