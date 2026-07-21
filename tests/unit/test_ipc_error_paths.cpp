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

#include <array>
#include <string>

IPC_CMD_DEFINE(MsgA, { int x; });
IPC_CMD_DEFINE(MsgB, { int y; });
IPC_EVENT_DEFINE(EvtA, { int v; });

struct ipc_actor g_actor;
struct ipc_actor g_no_handler;

namespace
{

template <typename Op>
int fill_table_with_fillers(struct ipc_actor *a, const std::string &prefix, ipc_msg_kind_t kind,
                            int count, Op op)
{
    static std::array<std::string, 32> namebufs{};
    std::array<ipc_msg_desc_t, 32> descs{};
    for (int i = 0; i < count; i++) {
        namebufs[i] = prefix + std::to_string(i);
        descs[i]    = (ipc_msg_desc_t) {
            .id   = 0,
            .kind = kind,
            .size = 0,
            .name = namebufs[i].c_str(),
        };
        if (op(a, &descs[i]) != 0) {
            return i;
        }
    }
    return count;
}

class ErrorPathTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        _ipc_reset_for_testing();
        mock_port_init();
        memset(&g_actor, 0, sizeof(g_actor));
        memset(&g_no_handler, 0, sizeof(g_no_handler));
        g_actor.name         = "test_actor";
        g_no_handler.name    = "no_handler_actor";
        g_actor.handler      = nullptr;
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
    ASSERT_EQ(ipc_register(&g_no_handler, &MsgA), 0);
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
    ASSERT_EQ(ipc_register(&g_no_handler, &MsgB), 0);
    MsgB_payload_t payload = {.y = 22};
    EXPECT_EQ(ipc_send_after_raw(&MsgB, 50, &payload), 0);
    const auto *st = mock_port_actor_state(&g_no_handler);
    EXPECT_EQ(st->send_after_count, 1);
}

TEST_F(ErrorPathTest, PublishToHandlerNullSubscriberSucceeds)
{
    ASSERT_EQ(ipc_subscribe(&g_no_handler, &EvtA), 0);
    EvtA_payload_t payload = {.v = 7};
    EXPECT_EQ(ipc_publish_raw(&EvtA, &payload), 0);
    const auto *st = mock_port_actor_state(&g_no_handler);
    EXPECT_EQ(st->send_count, 1);
}

TEST_F(ErrorPathTest, SendMacroPropagatesReturnCode)
{
    ASSERT_EQ(ipc_register(&g_actor, &MsgA), 0);
    mock_port_set_next_send_rc(-EAGAIN);
    MsgA_payload_t p = {.x = 1};
    EXPECT_EQ(ipc_send(MsgA, p), -EAGAIN);
    EXPECT_EQ(ipc_send(MsgA, p), 0);
}

TEST_F(ErrorPathTest, SendAfterMacroPropagatesReturnCode)
{
    ASSERT_EQ(ipc_register(&g_actor, &MsgB), 0);
    mock_port_set_next_send_after_rc(-ENOSPC);
    MsgB_payload_t p = {.y = 2};
    EXPECT_EQ(ipc_send_after(MsgB, 10, p), -ENOSPC);
    EXPECT_EQ(ipc_send_after(MsgB, 10, p), 0);
}

TEST_F(ErrorPathTest, PublishMacroPropagatesError)
{
    struct ipc_actor sub1{};
    struct ipc_actor sub2{};
    ASSERT_EQ(ipc_subscribe(&sub1, &EvtA), 0);
    ASSERT_EQ(ipc_subscribe(&sub2, &EvtA), 0);

    mock_port_set_send_should_fail(true);
    EvtA_payload_t p = {.v = 3};
    EXPECT_EQ(ipc_publish(EvtA, p), -ENOMEM);
}

TEST_F(ErrorPathTest, RegistrationTableOverflowAssertsInDebug)
{
#ifndef NDEBUG
    ASSERT_EQ(fill_table_with_fillers(&g_actor, "Filler", IPC_CMD, 32, ipc_register), 32);
    ipc_msg_desc_t overflow = {.id = 0, .kind = IPC_CMD, .size = 0, .name = "Overflow"};
    EXPECT_DEATH(ipc_register(&g_actor, &overflow), "registration table full");
#endif
}

TEST_F(ErrorPathTest, RegistrationTableFullReturnsEnomemInRelease)
{
#ifdef NDEBUG
    ASSERT_EQ(fill_table_with_fillers(&g_actor, "RelFiller", IPC_CMD, 32, ipc_register), 32);
    ipc_msg_desc_t overflow = {.id = 0, .kind = IPC_CMD, .size = 0, .name = "RelOverflow"};
    EXPECT_EQ(ipc_register(&g_actor, &overflow), -ENOMEM);
#endif
}

TEST_F(ErrorPathTest, SubscriptionTableFullReturnsEnomem)
{
    ASSERT_EQ(fill_table_with_fillers(&g_actor, "EvtFiller", IPC_EVENT, 32, ipc_subscribe), 32);
    ipc_msg_desc_t overflow = {.id = 0, .kind = IPC_EVENT, .size = 0, .name = "SubOverflow"};
    EXPECT_EQ(ipc_subscribe(&g_actor, &overflow), -ENOMEM);
}

TEST_F(ErrorPathTest, SendPropagatesPortErrors)
{
    ASSERT_EQ(ipc_register(&g_actor, &MsgA), 0);
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
    ASSERT_EQ(ipc_subscribe(&sub1, &EvtA), 0);
    ASSERT_EQ(ipc_subscribe(&sub2, &EvtA), 0);

    mock_port_set_next_send_rc(-EIO);
    EvtA_payload_t p = {.v = 1};
    EXPECT_EQ(ipc_publish_raw(&EvtA, &p), -EIO);
    EXPECT_EQ(mock_port_actor_state(&sub1)->send_count, 1);
    EXPECT_EQ(mock_port_actor_state(&sub2)->send_count, 1);
}

TEST_F(ErrorPathTest, SendAfterPropagatesPortError)
{
    ASSERT_EQ(ipc_register(&g_actor, &MsgB), 0);
    mock_port_set_next_send_after_rc(-ENOMEM);
    MsgB_payload_t p = {.y = 1};
    EXPECT_EQ(ipc_send_after_raw(&MsgB, 100, &p), -ENOMEM);
    EXPECT_EQ(ipc_send_after_raw(&MsgB, 100, &p), 0);
}

} // namespace
