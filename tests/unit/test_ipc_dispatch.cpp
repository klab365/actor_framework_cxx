#include <gtest/gtest.h>

#include <cstring>

#include "ipc.h"

IPC_CMD_DEFINE(DispatchA, { uint32_t value; });
IPC_CMD_DEFINE(DispatchB, { uint32_t value; });

namespace
{

int g_a_calls;
int g_b_calls;
uint32_t g_last_value;
uint32_t g_unknown_id;

void reset_dispatch_state()
{
    DispatchA.id = 0xA001U;
    DispatchB.id = 0xB001U;
    g_a_calls    = 0;
    g_b_calls    = 0;
    g_last_value = 0;
    g_unknown_id = 0;
}

IPC_HANDLE(DispatchA, on_dispatch_a)
{
    (void) raw_msg;
    EXPECT_NE(self, nullptr);
    g_a_calls++;
    g_last_value = msg->value;
}

IPC_HANDLE(DispatchB, on_dispatch_b)
{
    (void) raw_msg;
    EXPECT_NE(self, nullptr);
    g_b_calls++;
    g_last_value = msg->value;
}

struct ipc_msg make_msg(uint32_t id, uint32_t value)
{
    struct ipc_msg msg          = {};
    DispatchA_payload_t payload = {.value = value};
    msg.id                      = id;
    msg.kind                    = IPC_CMD;
    std::memcpy(msg.payload, &payload, sizeof(payload));
    return msg;
}

void handler_with_unknown(struct ipc_actor *self, const struct ipc_msg *msg)
{
    IPC_DISPATCH_TO(msg, DispatchA, on_dispatch_a)
    IPC_DISPATCH_TO(msg, DispatchB, on_dispatch_b)
    IPC_UNKNOWN({
        uint32_t values[] = {msg->id, 0U};
        g_unknown_id      = values[0] + values[1];
    });
}

void handler_ignore_unknown(struct ipc_actor *self, const struct ipc_msg *msg)
{
    IPC_DISPATCH_TO(msg, DispatchA, on_dispatch_a)
    IPC_DISPATCH_IGNORE_UNKNOWN();
}

} // namespace

TEST(IpcDispatchMacros, MatchedMessageCallsTypedHandlerOnly)
{
    reset_dispatch_state();
    struct ipc_actor actor = {};
    struct ipc_msg msg     = make_msg(DispatchB.id, 42U);

    handler_with_unknown(&actor, &msg);

    EXPECT_EQ(g_a_calls, 0);
    EXPECT_EQ(g_b_calls, 1);
    EXPECT_EQ(g_last_value, 42U);
    EXPECT_EQ(g_unknown_id, 0U);
}

TEST(IpcDispatchMacros, UnknownTerminatorRunsCustomBlock)
{
    reset_dispatch_state();
    struct ipc_actor actor = {};
    struct ipc_msg msg     = make_msg(0xDEADU, 7U);

    handler_with_unknown(&actor, &msg);

    EXPECT_EQ(g_a_calls, 0);
    EXPECT_EQ(g_b_calls, 0);
    EXPECT_EQ(g_unknown_id, 0xDEADU);
}

TEST(IpcDispatchMacros, IgnoreUnknownTerminatorDropsUnmatchedMessage)
{
    reset_dispatch_state();
    struct ipc_actor actor = {};
    struct ipc_msg msg     = make_msg(0xDEADU, 7U);

    handler_ignore_unknown(&actor, &msg);

    EXPECT_EQ(g_a_calls, 0);
    EXPECT_EQ(g_unknown_id, 0U);
}
