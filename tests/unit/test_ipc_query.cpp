/*
 * test_ipc_query.cpp — Unit tests for ipc_query_raw / ipc_reply_raw
 * exercising the full single-threaded round-trip via the mock port's
 * invoke_handlers mode.
 *
 * Uses the _raw variants — see comment in test_ipc_send.cpp.
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

IPC_QUERY_DEFINE(GetFoo, { int req; }, { int result; });

IPC_QUERY_DEFINE(TruncQry, {},
                 { uint8_t bytes[IPC_PAYLOAD_SIZE]; }); /* on-wire response, exactly the cap */

struct ipc_actor g_server;

class QueryTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        _ipc_reset_for_testing();
        mock_port_init();
        memset(&g_server, 0, sizeof(g_server));
        g_server.name    = "server";
        g_server.handler = &server_handler;
        ASSERT_EQ(ipc_register(&g_server, &GetFoo), 0);
        ASSERT_EQ(ipc_register(&g_server, &TruncQry), 0);
        mock_port_set_invoke_handlers(true);
    }
    void TearDown() override
    {
        mock_port_set_invoke_handlers(false);
        _ipc_reset_for_testing();
        mock_port_reset();
    }

    static void server_handler(struct ipc_actor *self, const struct ipc_msg *msg)
    {
        (void) self;
        if (msg->id == GetFoo.id) {
            const GetFoo_payload_t *p = (const GetFoo_payload_t *) msg->payload;
            GetFoo_response_t resp    = {.result = p->req * 3};
            ipc_reply_raw(msg, &resp, sizeof(resp));
        } else if (msg->id == TruncQry.id) {
            /* Reply with a buffer *larger* than IPC_PAYLOAD_SIZE so the
             * runtime truncation branch in ipc_reply_raw is exercised.
             * The wait blob holds at most IPC_PAYLOAD_SIZE response
             * bytes; we deliberately send more, and fill the trailing
             * bytes with a different pattern, so the test can tell
             * whether truncation happened at the right offset. */
            static uint8_t big[IPC_PAYLOAD_SIZE * 2];
            for (size_t i = 0; i < IPC_PAYLOAD_SIZE; i++) {
                big[i] = 0xA5;
            }
            for (size_t i = IPC_PAYLOAD_SIZE; i < sizeof(big); i++) {
                big[i] = 0x5A;
            }
            ipc_reply_raw(msg, big, sizeof(big));
        }
    }
};

TEST_F(QueryTest, RoundTripCopiesResponse)
{
    GetFoo_response_t resp = {.result = 0};
    GetFoo_payload_t req   = {.req = 7};
    int rc                 = ipc_query_raw(&GetFoo, &req, &resp, sizeof(resp), IPC_TIMEOUT_FOREVER);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(resp.result, 21);
}

TEST_F(QueryTest, ServerReplyLargerThanPayloadIsTruncated)
{
    /* Real truncation test: the server replies with a buffer
     * (IPC_PAYLOAD_SIZE * 2) bytes long. The caller's response buffer
     * is the same size, but ipc_query_raw copies at most
     * IPC_PAYLOAD_SIZE bytes from the on-wire wait blob. The first
     * IPC_PAYLOAD_SIZE bytes of the caller's buffer must match the
     * server's pattern (0xA5); the trailing bytes must be unchanged
     * (still 0x00 from the stack allocation / memset in
     * ipc_port_query_wait_init). */
    static uint8_t buf[IPC_PAYLOAD_SIZE * 2];
    memset(buf, 0, sizeof(buf));
    int rc = ipc_query_raw(&TruncQry, NULL, buf, sizeof(buf), IPC_TIMEOUT_FOREVER);
    ASSERT_EQ(rc, 0);
    for (size_t i = 0; i < IPC_PAYLOAD_SIZE; i++) {
        EXPECT_EQ(buf[i], 0xA5) << "i=" << i;
    }
    for (size_t i = IPC_PAYLOAD_SIZE; i < sizeof(buf); i++) {
        EXPECT_EQ(buf[i], 0x00) << "i=" << i;
    }
}

TEST_F(QueryTest, TimeoutIsReturnedWhenBlockFails)
{
    GetFoo_response_t resp = {.result = 0};
    GetFoo_payload_t req   = {.req = 1};
    mock_port_set_block_timeout(true);
    int rc = ipc_query_raw(&GetFoo, &req, &resp, sizeof(resp), IPC_TIMEOUT_MS(50));
    mock_port_set_block_timeout(false);
    EXPECT_EQ(rc, -ETIMEDOUT);
}

TEST_F(QueryTest, ReplyWithNoWaiterIsSilentNoOp)
{
    /* ipc_reply_raw on a stray msg must not crash, must not assert. */
    struct ipc_msg stray;
    memset(&stray, 0, sizeof(stray));
    ipc_reply_raw(&stray, NULL, 0);
}

TEST_F(QueryTest, UnknownQueryReturnsNoEnt)
{
    static ipc_msg_desc_t NotRegistered = {
        .id   = 0,
        .kind = IPC_QUERY,
        .size = 0,
        .name = "NotRegistered",
    };
    EXPECT_EQ(ipc_query_raw(&NotRegistered, NULL, NULL, 0, IPC_TIMEOUT_FOREVER), -ENOENT);
}

TEST_F(QueryTest, ResponseBufferSmallerThanPayloadIsCopiedUpToLimit)
{
    /* Caller asks for fewer bytes than IPC_PAYLOAD_SIZE. The core
     * must copy exactly resp_size bytes. The TruncQry server fills
     * its first IPC_PAYLOAD_SIZE bytes with 0xA5; we ask for 4. */
    uint8_t small[4] = {0};
    int rc           = ipc_query_raw(&TruncQry, NULL, small, sizeof(small), IPC_TIMEOUT_FOREVER);
    ASSERT_EQ(rc, 0);
    for (size_t i = 0; i < sizeof(small); i++) {
        EXPECT_EQ(small[i], 0xA5) << "i=" << i;
    }
}

TEST_F(QueryTest, ResponseNullWithZeroSizeIsAccepted)
{
    /* No caller buffer at all: the server's reply is still delivered
     * (the wait is woken), the function returns 0, and no bytes are
     * copied. The wait slot's bytes are still zeroed. */
    int rc = ipc_query_raw(&GetFoo, NULL, NULL, 0, IPC_TIMEOUT_FOREVER);
    EXPECT_EQ(rc, 0);
}

TEST_F(QueryTest, ServerErrorReplyStillWakesWaiter)
{
    /* If a future server reply path returns a non-zero status from
     * ipc_port_query_wait_wake, the waiter should observe it. The
     * current mock always wakes with status 0, so this is a baseline
     * test that documents the happy path through the wake edge. */
    int rc = ipc_query_raw(&GetFoo, NULL, NULL, 0, IPC_TIMEOUT_FOREVER);
    EXPECT_EQ(rc, 0);
}

} // namespace
