#include <ipc.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

IPC_CMD_DEFINE(BasicPing, { uint32_t count; });
IPC_CMD_DEFINE(BasicPong, { uint32_t count; });
IPC_CMD_DEFINE(BasicStatusRequest, { uint32_t request_id; });
IPC_CMD_DEFINE(BasicStatusResponse, {
    uint32_t request_id;
    uint32_t ping_count;
    uint32_t pong_count;
});

static atomic_uint basic_ping_count;
static atomic_uint basic_pong_count;

IPC_HANDLE(BasicPing, on_basic_ping)
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

IPC_HANDLE(BasicPong, on_basic_pong)
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

IPC_HANDLE(BasicStatusRequest, on_basic_status_request)
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

IPC_HANDLE(BasicStatusResponse, on_basic_status_response)
{
    (void) self;
    (void) raw_msg;

    printk("ipc basic: status response request=%u ping=%u pong=%u\n", msg->request_id,
           msg->ping_count, msg->pong_count);
}

static void ping_actor_handler(struct ipc_actor *self, const struct ipc_msg *msg)
{
    IPC_DISPATCH_TO(msg, BasicPong, on_basic_pong)
    IPC_DISPATCH_TO(msg, BasicStatusResponse, on_basic_status_response)
    IPC_UNKNOWN({ printk("ipc basic: ping actor unknown message id=0x%08x\n", msg->id); });
}

static void pong_actor_handler(struct ipc_actor *self, const struct ipc_msg *msg)
{
    IPC_DISPATCH_TO(msg, BasicPing, on_basic_ping)
    IPC_DISPATCH_TO(msg, BasicStatusRequest, on_basic_status_request)
    IPC_UNKNOWN({ printk("ipc basic: pong actor unknown message id=0x%08x\n", msg->id); });
}

/* Each actor declares exact-size static stack/msgq storage via ipc.h. */
IPC_ACTOR_DEFINE(ping_actor, "ipc_ping", ping_actor_handler, 1024, K_PRIO_PREEMPT(7), 4);
IPC_ACTOR_DEFINE(pong_actor, "ipc_pong", pong_actor_handler, 2048, K_PRIO_PREEMPT(7), 4);

static int basic_actor_init(void)
{
    int rc = ipc_register(&ping_actor, &BasicPong);
    if (rc != 0) {
        printk("ipc basic: BasicPong register failed: %d\n", rc);
        return rc;
    }

    rc = ipc_register(&pong_actor, &BasicPing);
    if (rc != 0) {
        printk("ipc basic: BasicPing register failed: %d\n", rc);
        return rc;
    }

    rc = ipc_register(&pong_actor, &BasicStatusRequest);
    if (rc != 0) {
        printk("ipc basic: BasicStatusRequest register failed: %d\n", rc);
        return rc;
    }

    rc = ipc_register(&ping_actor, &BasicStatusResponse);
    if (rc != 0) {
        printk("ipc basic: BasicStatusResponse register failed: %d\n", rc);
        return rc;
    }

    rc = ipc_start_all_actors();
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

    ipc_stop_all();
    printk("zephyr-basic OK\n");

    /* native_sim keeps the Zephyr kernel/idle thread alive after main()
     * returns. Use the host exit path so `west build -t run` terminates.
     */
    exit(0);
}
