#include "ipc.h"
#include "ipc_internal.h"

IPC_CMD_DEFINE(HookUnhandledCmd, { int value; });

IPC_ACTOR_DEFINE(hook_actor, "hook", 1024, 0, 4);

static int hook_start_count;
static int hook_stop_count;
static int hook_unknown_count;
static uint32_t hook_unknown_id;

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

void test_ipc_hooks_reset_counters(void)
{
    hook_start_count   = 0;
    hook_stop_count    = 0;
    hook_unknown_count = 0;
    hook_unknown_id    = 0;
}

void test_ipc_hooks_register_actor(void)
{
    _ipc_actor_register_static(&hook_actor);
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

void test_ipc_hooks_dispatch_unknown(uint32_t msg_id)
{
    struct ipc_msg msg = {
        .id   = msg_id,
        .kind = IPC_CMD,
    };
    hook_actor.handler(&hook_actor, &msg);
}
