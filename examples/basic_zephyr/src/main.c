#include <errno.h>
#include <ipc.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

IPC_CMD_DEFINE(BasicPing, { uint32_t count; });
IPC_CMD_DEFINE(BasicPong, { uint32_t count; });
IPC_CMD_DEFINE(BasicStatusRequest, { uint32_t request_id; });
IPC_CMD_DEFINE(BasicFault, { uint32_t code; });
IPC_CMD_DEFINE(BasicStatusResponse, {
    uint32_t request_id;
    uint32_t ping_count;
    uint32_t pong_count;
});

static atomic_uint basic_ping_count;
static atomic_uint basic_pong_count;
static bool pong_restart_pending;
static unsigned int pong_restart_count;

/* Each actor declares exact-size static stack/msgq storage via ipc.h. */
IPC_ACTOR_DEFINE(ping_actor, "ipc_ping", 1024, K_PRIO_PREEMPT(7), 4);
IPC_ACTOR_DEFINE(pong_actor, "ipc_pong", 2048, K_PRIO_PREEMPT(7), 4);
IPC_SUPERVISE(pong_actor, IPC_SUPERVISE_RESTART);

IPC_START_HOOK(ping_actor, ping_on_start)
{
    ARG_UNUSED(self);
    printk("ipc basic: ping start hook\n");
}

IPC_STOP_HOOK(ping_actor, ping_on_stop)
{
    ARG_UNUSED(self);
    printk("ipc basic: ping stop hook\n");
}

IPC_UNKNOWN(ping_actor, ping_on_unknown)
{
    ARG_UNUSED(self);
    printk("ipc basic: ping unknown id=0x%x kind=%d\n", msg->id, (int) msg->kind);
}

IPC_START_HOOK(pong_actor, pong_on_start)
{
    ARG_UNUSED(self);
    if (pong_restart_pending) {
        pong_restart_pending = false;
        printk("ipc basic: pong RESTART complete (#%u)\n", pong_restart_count);
    } else {
        printk("ipc basic: pong initial start hook\n");
    }
}

IPC_STOP_HOOK(pong_actor, pong_on_stop)
{
    ARG_UNUSED(self);
    if (pong_restart_pending) {
        printk("ipc basic: pong stop hook before restart #%u\n", pong_restart_count);
    } else {
        printk("ipc basic: pong stop hook\n");
    }
}

IPC_UNKNOWN(pong_actor, pong_on_unknown)
{
    ARG_UNUSED(self);
    printk("ipc basic: pong unknown id=0x%x kind=%d\n", msg->id, (int) msg->kind);
}

IPC_FAIL_HOOK(pong_actor, pong_on_failure)
{
    ARG_UNUSED(self);
    pong_restart_count++;
    pong_restart_pending = true;
    printk("ipc basic: pong failure reason=%d -> restart #%u\n", reason, pong_restart_count);
}

IPC_ACTOR_HANDLE(pong_actor, BasicPing, on_basic_ping)
{
    ARG_UNUSED(self);

    atomic_fetch_add_explicit(&basic_ping_count, 1U, memory_order_relaxed);
    printk("ipc basic: ping %u\n", msg->count);

    BasicPong_payload_t payload = {.count = msg->count};
    int rc                      = ipc_send(BasicPong, payload);
    if (rc != 0) {
        printk("ipc basic: pong send failed: %d\n", rc);
    }
}

IPC_ACTOR_HANDLE(ping_actor, BasicPong, on_basic_pong)
{
    (void) self;
    (void) raw_msg;

    atomic_fetch_add_explicit(&basic_pong_count, 1U, memory_order_relaxed);
    printk("ipc basic: pong %u\n", msg->count);

    if (msg->count < 5U) {
        BasicPing_payload_t payload = {.count = msg->count + 1U};
        int rc                      = ipc_send_after(BasicPing, 1000U, payload);
        if (rc != 0) {
            printk("ipc basic: next ping schedule failed: %d\n", rc);
        }
    }
}

IPC_ACTOR_HANDLE(pong_actor, BasicStatusRequest, on_basic_status_request)
{
    (void) self;
    (void) raw_msg;

    BasicStatusResponse_payload_t response = {
        .request_id = msg->request_id,
        .ping_count = atomic_load_explicit(&basic_ping_count, memory_order_relaxed),
        .pong_count = atomic_load_explicit(&basic_pong_count, memory_order_relaxed),
    };
    printk("ipc basic: status request %u -> ping=%u pong=%u\n", response.request_id,
           response.ping_count, response.pong_count);
    ipc_send(BasicStatusResponse, response);
}

IPC_ACTOR_HANDLE(pong_actor, BasicFault, on_basic_fault)
{
    (void) raw_msg;

    printk("ipc basic: pong received fault code=0x%x; reporting actor failure\n", msg->code);
    int rc = ipc_actor_fail(self, -EIO);
    if (rc != 0) {
        printk("ipc basic: pong supervision failed: %d\n", rc);
    }
}

IPC_ACTOR_HANDLE(ping_actor, BasicStatusResponse, on_basic_status_response)
{
    (void) self;
    (void) raw_msg;

    printk("ipc basic: status response request=%u ping=%u pong=%u\n", msg->request_id,
           msg->ping_count, msg->pong_count);
}

static int basic_actor_init(void)
{
    int rc = ipc_start_all_actors();
    if (rc != 0) {
        printk("ipc basic: actor start failed: %d\n", rc);
        return rc;
    }

    BasicPing_payload_t payload = {.count = 1U};
    rc                          = ipc_send(BasicPing, payload);
    if (rc != 0) {
        printk("ipc basic: initial ping send failed: %d\n", rc);
        return rc;
    }

    return 0;
}

SYS_INIT(basic_actor_init, APPLICATION, 85);

int main(void)
{
    printk("IPC Actor Framework basic Zephyr example\n");

    /* The actors exchange ping/pong once per second until ping 5. Ask the
     * pong actor for status via async request/response before stopping the
     * actor threads so native_sim can terminate cleanly.
     */
    k_sleep(K_SECONDS(6));

    BasicStatusRequest_payload_t request = {.request_id = 1U};
    int rc                               = ipc_send(BasicStatusRequest, request);
    if (rc != 0) {
        printk("ipc basic: status request failed: %d\n", rc);
    }
    k_sleep(K_MSEC(100));

    BasicFault_payload_t fault = {.code = 0xBADCAFEu};
    rc                         = ipc_send(BasicFault, fault);
    if (rc != 0) {
        printk("ipc basic: fault send failed: %d\n", rc);
    }
    k_sleep(K_MSEC(100));

    ipc_stop_all();
    printk("zephyr-basic OK\n");

    /* native_sim keeps the Zephyr kernel/idle thread alive after main()
     * returns. Use the host exit path so `west build -t run` terminates.
     */
    exit(0);
}
