#include <gtest/gtest.h>

extern "C" {
#include "ipc.h"
}

namespace
{

IPC_CMD_DEFINE_LOCAL(TestBrightness, { uint8_t value; });
IPC_ACTOR_DEFINE(test_port_actor, "test_port_actor", 1024, 0, 2, IPC_MESSAGE_MAX(TestBrightness));

int handled_count;
uint8_t handled_brightness;

IPC_ACTOR_HANDLE(test_port_actor, TestBrightness, on_test_brightness)
{
    (void) self;
    (void) raw_msg;
    handled_count++;
    handled_brightness = msg->value;
}

TEST(IpcTestPort, QueuesCopiedMessagesUntilExplicitlyPumped)
{
    handled_count      = 0;
    handled_brightness = 0;
    ASSERT_EQ(ipc_test_start(), 0);

    TestBrightness_payload_t request = {.value = 200};
    ASSERT_EQ(ipc_send(TestBrightness, request), 0);
    request.value = 1;

    EXPECT_EQ(handled_count, 0);
    EXPECT_EQ(ipc_test_run_until_idle(), 1u);
    EXPECT_EQ(handled_count, 1);
    EXPECT_EQ(handled_brightness, 200);

    ipc_test_stop();
}

} // namespace
