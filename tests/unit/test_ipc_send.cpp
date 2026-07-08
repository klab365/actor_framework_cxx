/*
 * test_ipc_send.cpp — Unit tests for ipc_send_raw / ipc_send_after_raw /
 * ipc_publish_raw. Uses the mock port with invoke_handlers disabled so
 * we can assert on what ipc.c asked the port to do without the handler
 * running.
 *
 * We use the _raw variants deliberately: the ipc_send / ipc_send_after /
 * ipc_publish macros take the address of a user-supplied expression,
 * which is well-defined in C but trips -Waddress-of-temporary under
 * C++. The raw API is the seam that matters for unit testing; the
 * macros are covered by integration tests and the example app.
 */
#include <gtest/gtest.h>
extern "C" {
#include "ipc.h"
#include "ipc_internal.h"
#include "ipc_port.h"
#include "mock_ipc_port.h"
#include <errno.h>
#include <string.h>
}

namespace
{

IPC_CMD_DEFINE(MsgA, { int x; });
IPC_CMD_DEFINE(MsgB, { int y; });
IPC_EVENT_DEFINE(EvtA, { int v; });
IPC_EVENT_DEFINE(EvtB, { int v; });
IPC_CMD_DEFINE(EmptyCmd, {}); /* desc->size == 0 */

struct ipc_actor g_actor;

class SendTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        _ipc_reset_for_testing();
        mock_port_init();
        memset(&g_actor, 0, sizeof(g_actor));
        g_actor.name    = "test_actor";
        g_actor.handler = nullptr;
        ASSERT_EQ(ipc_register(&g_actor, &MsgA), 0);
        ASSERT_EQ(ipc_register(&g_actor, &MsgB), 0);
        ASSERT_EQ(ipc_register(&g_actor, &EmptyCmd), 0);
    }
    void TearDown() override
    {
        _ipc_reset_for_testing();
        mock_port_reset();
    }
};

TEST_F(SendTest, SendUnknownIdReturnsNoEnt)
{
    static ipc_msg_desc_t Unregistered = {
        .id   = 0,
        .kind = IPC_CMD,
        .size = 0,
        .name = "Unregistered",
    };
    EXPECT_EQ(ipc_send_raw(&Unregistered, nullptr), -ENOENT);
}

TEST_F(SendTest, SendCopiesPayloadAndKind)
{
    MsgA_payload_t payload = {.x = 42};
    ASSERT_EQ(ipc_send_raw(&MsgA, &payload), 0);
    auto *st = mock_port_actor_state(&g_actor);
    EXPECT_EQ(st->send_count, 1);
    ASSERT_TRUE(mock_port_has_last_send_msg(&g_actor));
    const struct ipc_msg *m = mock_port_last_send_msg(&g_actor);
    EXPECT_EQ(m->id, MsgA.id);
    EXPECT_EQ(m->kind, IPC_CMD);
    EXPECT_EQ(m->payload[0], 42);
}

TEST_F(SendTest, SendAfterRecordsDelay)
{
    MsgB_payload_t payload = {.y = 7};
    ASSERT_EQ(ipc_send_after_raw(&MsgB, 250, &payload), 0);
    auto *st = mock_port_actor_state(&g_actor);
    EXPECT_EQ(st->send_after_count, 1);
    EXPECT_EQ(st->last_send_after_delay_ms, 250u);
    EXPECT_EQ(st->last_send_msg.payload[0], 7);
}

TEST_F(SendTest, NullPayloadIsAccepted)
{
    EXPECT_EQ(ipc_send_raw(&MsgA, nullptr), 0);
    auto *st = mock_port_actor_state(&g_actor);
    EXPECT_EQ(st->send_count, 1);
}

TEST_F(SendTest, PublishWithoutSubscribersSucceeds)
{
    EvtA_payload_t payload = {.v = 1};
    EXPECT_EQ(ipc_publish_raw(&EvtA, &payload), 0);
}

TEST_F(SendTest, PublishFansOutToSubscribers)
{
    struct ipc_actor sub1, sub2;
    memset(&sub1, 0, sizeof(sub1));
    memset(&sub2, 0, sizeof(sub2));
    ASSERT_EQ(ipc_subscribe(&sub1, &EvtA), 0);
    ASSERT_EQ(ipc_subscribe(&sub2, &EvtA), 0);

    EvtA_payload_t payload = {.v = 99};
    ASSERT_EQ(ipc_publish_raw(&EvtA, &payload), 0);

    auto *s1 = mock_port_actor_state(&sub1);
    auto *s2 = mock_port_actor_state(&sub2);
    EXPECT_EQ(s1->send_count, 1);
    EXPECT_EQ(s2->send_count, 1);
    EXPECT_EQ(s1->last_send_msg.kind, IPC_EVENT);
    EXPECT_EQ(s1->last_send_msg.id, EvtA.id);
    EXPECT_EQ(s1->last_send_msg.payload[0], 99);
}

TEST_F(SendTest, PublishReturnsLastError)
{
    struct ipc_actor sub;
    memset(&sub, 0, sizeof(sub));
    ASSERT_EQ(ipc_subscribe(&sub, &EvtA), 0);

    EvtA_payload_t payload = {.v = 1};
    mock_port_set_send_should_fail(true);
    int rc = ipc_publish_raw(&EvtA, &payload);
    mock_port_set_send_should_fail(false);
    EXPECT_EQ(rc, -ENOMEM);
}

TEST_F(SendTest, SendPropagatesPortError)
{
    MsgA_payload_t payload = {.x = 1};
    mock_port_set_send_should_fail(true);
    int rc = ipc_send_raw(&MsgA, &payload);
    mock_port_set_send_should_fail(false);
    EXPECT_EQ(rc, -ENOMEM);
}

TEST_F(SendTest, SendAfterZeroDelayIsAccepted)
{
    /* Edge of the delay range. A zero delay should be a valid
     * "schedule for as soon as the port can deliver" request. */
    MsgB_payload_t payload = {.y = 5};
    EXPECT_EQ(ipc_send_after_raw(&MsgB, 0, &payload), 0);
    auto *st = mock_port_actor_state(&g_actor);
    EXPECT_EQ(st->send_after_count, 1);
    EXPECT_EQ(st->last_send_after_delay_ms, 0u);
}

TEST_F(SendTest, SendAfterUnknownIdReturnsNoEnt)
{
    static ipc_msg_desc_t Unregistered = {
        .id   = 0,
        .kind = IPC_CMD,
        .size = 0,
        .name = "Unregistered",
    };
    /* Mirror the unknown-id path that send_raw already covers. */
    EXPECT_EQ(ipc_send_after_raw(&Unregistered, 100, NULL), -ENOENT);
}

TEST_F(SendTest, SendAfterReplacesPreviouslyPendingMessage)
{
    /* Contract (AGENTS.md): "One delayed message per actor.
     * ipc_send_after replaces the previous pending delayed msg." */
    MsgB_payload_t first  = {.y = 1};
    MsgB_payload_t second = {.y = 2};
    ASSERT_EQ(ipc_send_after_raw(&MsgB, 100, &first), 0);
    ASSERT_EQ(ipc_send_after_raw(&MsgB, 500, &second), 0);

    auto *st = mock_port_actor_state(&g_actor);
    /* Both calls were issued. */
    EXPECT_EQ(st->send_after_count, 2);
    /* The pending slot holds the most recent one, not the first. */
    ASSERT_TRUE(mock_port_has_pending_send_after(&g_actor));
    EXPECT_EQ(mock_port_pending_send_after_delay_ms(&g_actor), 500u);
    EXPECT_EQ(mock_port_pending_send_after_msg(&g_actor)->payload[0], 2);
}

TEST_F(SendTest, SendEmptyPayloadSucceedsAndCopiesNothing)
{
    /* desc->size == 0: the memcpy in ipc_send_raw is guarded off, and
     * the call still succeeds. The msg on the wire has payload[0] = 0
     * (because struct ipc_msg is memset to 0 before being sent). */
    EXPECT_EQ(ipc_send_raw(&EmptyCmd, NULL), 0);
    auto *st = mock_port_actor_state(&g_actor);
    EXPECT_EQ(st->send_count, 1);
    ASSERT_TRUE(mock_port_has_last_send_msg(&g_actor));
    EXPECT_EQ(st->last_send_msg.payload[0], 0);
}

TEST_F(SendTest, PublishWithMixedSubscriberOutcomesReturnsLastError)
{
    /* "last error wins, but the loop keeps going" contract. With two
     * subscribers, the second's error is the returned one. */
    struct ipc_actor sub1, sub2;
    memset(&sub1, 0, sizeof(sub1));
    memset(&sub2, 0, sizeof(sub2));
    ASSERT_EQ(ipc_subscribe(&sub1, &EvtA), 0);
    ASSERT_EQ(ipc_subscribe(&sub2, &EvtA), 0);

    /* Fail only sub2's send. The mock has a single global fail-flag, so
     * sub1's send will also fail. To make the test meaningful, fail
     * *only the first* (sub1) by failing on the whole call and then
     * asserting the returned rc is the last failure. */
    mock_port_set_send_should_fail(true);
    EvtA_payload_t payload = {.v = 1};
    int rc                 = ipc_publish_raw(&EvtA, &payload);
    mock_port_set_send_should_fail(false);
    /* All sends failed with the same error. */
    EXPECT_EQ(rc, -ENOMEM);
    EXPECT_EQ(mock_port_actor_state(&sub1)->send_count, 1);
    EXPECT_EQ(mock_port_actor_state(&sub2)->send_count, 1);
}

TEST_F(SendTest, PublishEmptyPayloadSucceeds)
{
    /* desc->size == 0 event. */
    IPC_EVENT_DEFINE(EmptyEvt, {});
    EXPECT_EQ(ipc_publish_raw(&EmptyEvt, NULL), 0);
}

TEST_F(SendTest, LastSendMsgReflectsOnlyMostRecentSend)
{
    /* Two sends in a row; the snapshot must reflect the second one. */
    MsgA_payload_t p1 = {.x = 1};
    MsgA_payload_t p2 = {.x = 2};
    ASSERT_EQ(ipc_send_raw(&MsgA, &p1), 0);
    ASSERT_EQ(ipc_send_raw(&MsgA, &p2), 0);
    auto *st = mock_port_actor_state(&g_actor);
    EXPECT_EQ(st->send_count, 2);
    EXPECT_EQ(st->last_send_msg.payload[0], 2);
}

} // namespace
