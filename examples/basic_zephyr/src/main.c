#include <ipc.h>
#include <stdlib.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

IPC_CMD_DEFINE(BasicPing, { uint32_t count; });

static struct ipc_actor basic_actor;

IPC_HANDLE(BasicPing, on_basic_ping)
{
    (void) self;
    (void) raw_msg;

    printk("ipc basic: ping %u\n", msg->count);

    if (msg->count < 5U) {
        BasicPing_payload_t payload = {.count = msg->count + 1U};
        ipc_send_after(BasicPing, 1000U, payload);
    }
}

static void basic_actor_handler(struct ipc_actor *self, const struct ipc_msg *msg)
{
    IPC_DISPATCH_TO(msg, BasicPing, on_basic_ping)
    {
        printk("ipc basic: unknown message id=0x%08x\n", msg->id);
    }
}

static int basic_actor_init(void)
{
    struct ipc_actor_cfg cfg = {
        .stack_size  = 1024,
        .priority    = K_PRIO_PREEMPT(7),
        .queue_depth = 4,
    };
    int rc = ipc_actor_init(&basic_actor, "ipc_basic", basic_actor_handler, cfg);
    if (rc != 0) {
        printk("ipc basic: actor init failed: %d\n", rc);
        return rc;
    }

    rc = IPC_REGISTER(&basic_actor, BasicPing);
    if (rc != 0) {
        printk("ipc basic: register failed: %d\n", rc);
        return rc;
    }

    BasicPing_payload_t payload = {.count = 1U};
    rc                          = ipc_send(BasicPing, payload);
    if (rc != 0) {
        printk("ipc basic: initial send failed: %d\n", rc);
        return rc;
    }

    return 0;
}

SYS_INIT(basic_actor_init, APPLICATION, 85);

int main(void)
{
    printk("IPC Actor Framework basic Zephyr example\n");

    /* The actor re-arms itself once per second until ping 5.  Stop the
     * actor thread afterwards so native_sim can terminate cleanly instead
     * of sitting forever in the actor's k_poll() loop.
     */
    k_sleep(K_SECONDS(6));
    ipc_stop_all();
    printk("zephyr-basic OK\n");

    /* native_sim keeps the Zephyr kernel/idle thread alive after main()
     * returns.  Use the host exit path so `west build -t run` terminates.
     */
    exit(0);
}
