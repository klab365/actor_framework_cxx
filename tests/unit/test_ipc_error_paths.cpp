/*
 * test_ipc_error_paths.cpp — Unit tests for IPC error handling.
 */
#include <gtest/gtest.h>
extern "C" {
#include "ipc.h"
#include "ipc_internal.h"
#include "mock_ipc_port.h"
#include <errno.h>
#include <string.h>
}

IPC_CMD_DEFINE(MsgA, { int x; });
IPC_CMD_DEFINE(MsgB, { int y; });
IPC_EVENT_DEFINE(EvtA, { int v; });

struct ipc_actor g_actor;
struct ipc_actor g_no_handler;

namespace
{

static void on_msg_a(struct ipc_actor *self, const MsgA_payload_t *msg,
                     const struct ipc_msg *raw_msg)
{
    (void) self;
    (void) msg;
    (void) raw_msg;
}

static void on_msg_b(struct ipc_actor *self, const MsgB_payload_t *msg,
                     const struct ipc_msg *raw_msg)
{
    (void) self;
    (void) msg;
    (void) raw_msg;
}

static void on_evt_a(struct ipc_actor *self, const EvtA_payload_t *msg,
                     const struct ipc_msg *raw_msg)
{
    (void) self;
    (void) msg;
    (void) raw_msg;
}

void on_msg_a_shim(struct ipc_actor *self, const void *payload, const struct ipc_msg *raw_msg)
{
    on_msg_a(self, (const MsgA_payload_t *) payload, raw_msg);
}

void on_msg_b_shim(struct ipc_actor *self, const void *payload, const struct ipc_msg *raw_msg)
{
    on_msg_b(self, (const MsgB_payload_t *) payload, raw_msg);
}

void on_evt_a_shim(struct ipc_actor *self, const void *payload, const struct ipc_msg *raw_msg)
{
    on_evt_a(self, (const EvtA_payload_t *) payload, raw_msg);
}

void register_static_handler(struct ipc_actor *actor, const char *name, ipc_msg_desc_t *desc,
                             ipc_actor_msg_handler_t handler)
{
    memset(actor, 0, sizeof(*actor));
    actor->name    = name;
    actor->handler = nullptr;
    _ipc_actor_register_handler_static(actor, desc, handler);
}

class ErrorPathTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        _ipc_reset_for_testing();
        mock_port_init();
        memset(&g_actor, 0, sizeof(g_actor));
        g_actor.name    = "test_actor";
        g_actor.handler = nullptr;
        memset(&g_no_handler, 0, sizeof(g_no_handler));
        g_no_handler.name    = "no_handler_actor";
        g_no_handler.handler = nullptr;
    }

    void TearDown() override
    {
        _ipc_reset_for_testing();
        mock_port_reset();
    }
};

TEST_F(ErrorPathTest, SendToHandlerNullActorSucceedsWithoutDispatch)
{
    register_static_handler(&g_no_handler, "no_handler_actor", &MsgA, on_msg_a_shim);
    mock_port_set_invoke_handlers(true);

    MsgA_payload_t payload = {.x = 11};
    EXPECT_EQ(ipc_send_raw(&MsgA, &payload), 0);

    const auto *st = mock_port_actor_state(&g_no_handler);
    EXPECT_EQ(st->send_count, 1);
    ASSERT_TRUE(mock_port_has_last_send_msg(&g_no_handler));
    EXPECT_EQ(st->last_send_msg.payload[0], 11);
}

TEST_F(ErrorPathTest, SendAfterToHandlerNullActorSucceeds)
{
    register_static_handler(&g_no_handler, "no_handler_actor", &MsgB, on_msg_b_shim);
    MsgB_payload_t payload = {.y = 22};
    EXPECT_EQ(ipc_send_after_raw(&MsgB, 50, &payload), 0);
    const auto *st = mock_port_actor_state(&g_no_handler);
    EXPECT_EQ(st->send_after_count, 1);
}

TEST_F(ErrorPathTest, PublishToHandlerNullSubscriberSucceeds)
{
    register_static_handler(&g_no_handler, "no_handler_actor", &EvtA, on_evt_a_shim);
    EvtA_payload_t payload = {.v = 7};
    EXPECT_EQ(ipc_publish_raw(&EvtA, &payload), 0);
    const auto *st = mock_port_actor_state(&g_no_handler);
    EXPECT_EQ(st->send_count, 1);
}

TEST_F(ErrorPathTest, SendMacroPropagatesReturnCode)
{
    register_static_handler(&g_actor, "test_actor", &MsgA, on_msg_a_shim);
    mock_port_set_next_send_rc(-EAGAIN);
    MsgA_payload_t p = {.x = 1};
    EXPECT_EQ(ipc_send(MsgA, p), -EAGAIN);
    EXPECT_EQ(ipc_send(MsgA, p), 0);
}

TEST_F(ErrorPathTest, SendAfterMacroPropagatesReturnCode)
{
    register_static_handler(&g_actor, "test_actor", &MsgB, on_msg_b_shim);
    mock_port_set_next_send_after_rc(-ENOSPC);
    MsgB_payload_t p = {.y = 2};
    EXPECT_EQ(ipc_send_after(MsgB, 10, p), -ENOSPC);
    EXPECT_EQ(ipc_send_after(MsgB, 10, p), 0);
}

TEST_F(ErrorPathTest, PublishMacroPropagatesError)
{
    struct ipc_actor sub1{};
    struct ipc_actor sub2{};
    register_static_handler(&sub1, "sub1", &EvtA, on_evt_a_shim);
    register_static_handler(&sub2, "sub2", &EvtA, on_evt_a_shim);

    mock_port_set_send_should_fail(true);
    EvtA_payload_t p = {.v = 3};
    EXPECT_EQ(ipc_publish(EvtA, p), -ENOMEM);
}

TEST_F(ErrorPathTest, SendPropagatesPortErrors)
{
    register_static_handler(&g_actor, "test_actor", &MsgA, on_msg_a_shim);
    MsgA_payload_t p = {.x = 1};

    mock_port_set_next_send_rc(-EINVAL);
    EXPECT_EQ(ipc_send_raw(&MsgA, &p), -EINVAL);
    mock_port_set_next_send_rc(-EAGAIN);
    EXPECT_EQ(ipc_send_raw(&MsgA, &p), -EAGAIN);
    mock_port_set_next_send_rc(-EIO);
    EXPECT_EQ(ipc_send_raw(&MsgA, &p), -EIO);
    mock_port_set_next_send_rc(-ENOSPC);
    EXPECT_EQ(ipc_send_raw(&MsgA, &p), -ENOSPC);
}

TEST_F(ErrorPathTest, PublishPropagatesFirstNonZeroError)
{
    struct ipc_actor sub1{};
    struct ipc_actor sub2{};
    register_static_handler(&sub1, "sub1", &EvtA, on_evt_a_shim);
    register_static_handler(&sub2, "sub2", &EvtA, on_evt_a_shim);

    mock_port_set_next_send_rc(-EIO);
    EvtA_payload_t p = {.v = 1};
    EXPECT_EQ(ipc_publish_raw(&EvtA, &p), -EIO);
    EXPECT_EQ(mock_port_actor_state(&sub1)->send_count, 1);
    EXPECT_EQ(mock_port_actor_state(&sub2)->send_count, 1);
}

TEST_F(ErrorPathTest, SendAfterPropagatesPortError)
{
    register_static_handler(&g_actor, "test_actor", &MsgB, on_msg_b_shim);
    mock_port_set_next_send_after_rc(-ENOMEM);
    MsgB_payload_t p = {.y = 1};
    EXPECT_EQ(ipc_send_after_raw(&MsgB, 100, &p), -ENOMEM);
    EXPECT_EQ(ipc_send_after_raw(&MsgB, 100, &p), 0);
}

} // namespace
