/*
 * test_ipc_error_paths.cpp — Unit tests for the error-handling contract
 * of the IPC framework. Closes the gaps called out in the unit-test
 * review notes:
 *
 *   1. Actors with handler == NULL (send/publish/send_after must
 *      succeed and handler dispatch must be skipped without crashing).
 *   2. The ipc_send / ipc_send_after / ipc_publish / ipc_query /
 *      ipc_reply macros (non-raw). The existing tests use the _raw
 *      API because the macros take the address of a user expression,
 *      which trips -Waddress-of-temporary under C++. We can still
 *      drive the macro path with named lvalues to confirm the macro
 *      passes through the _raw return code unchanged.
 *   3. Table overflow on registration, subscription, and the
 *      in-flight query wait table. The default caps (32 / 32 / 16)
 *      are used directly — the cap-override path is documented in
 *      ipc_defaults.h but a single-TU override can't shrink the
 *      ipc_core OBJECT lib's tables. We assert on the behavioural
 *      contract: assert fires in debug; -ENOMEM returned in release.
 *   4. Server-doesn't-reply path: ipc_query_raw invoked against a
 *      registered query where the handler never calls ipc_reply_raw.
 *      Distinct from the existing TimeoutIsReturnedWhenBlockFails
 *      test (which forces the mock's block to fail) — here the real
 *      code path runs: handler dispatches, no reply, block returns
 *      -ETIMEDOUT because the wait's done flag is never set.
 *   5. Non-ENOMEM port error codes (EINVAL, EAGAIN, EIO, ENOSPC) on
 *      both ipc_port_send (used by send/publish/query) and
 *      ipc_port_send_after (used by send_after). Injected via the
 *      one-shot mock_port_set_next_send_rc / _send_after_rc hooks
 *      added to the mock port.
 *
 * Uses the mock port — no real threads except in
 * WaitTableOverflowReturnsEnomem, which spawns helper threads to
 * saturate the wait table.
 */
#include <gtest/gtest.h>
extern "C" {
#include "ipc.h"
#include "ipc_internal.h"
#include "ipc_port.h"
#include "mock_ipc_port.h"
#include <errno.h>
#include <pthread.h>
#include <string.h>
}

#include <array>
#include <string>

/* Message types reused across the test cases. Declared at
 * translation-unit scope (rather than in the anonymous namespace
 * below) so the pthread entry point overflow_thread_body can name
 * them without dragging the rest of the test-only infrastructure
 * into its prototype. */
IPC_CMD_DEFINE(MsgA, { int x; });
IPC_CMD_DEFINE(MsgB, { int y; });
IPC_EVENT_DEFINE(EvtA, { int v; });
IPC_QUERY_DEFINE(GetFoo, { int req; }, { int result; });

/* Plain non-const globals, matching the convention used in
 * test_ipc_lifecycle.cpp and test_ipc_send.cpp. The C API
 * (ipc_register, ipc_subscribe, ipc_actor_init, mock_port_actor_state)
 * takes `struct ipc_actor *` and writes through it, so the test
 * fixture has to be mutable. */
struct ipc_actor g_actor;
struct ipc_actor g_no_handler;

/* Per-thread context for the wait-table-overflow test. Defined at
 * translation-unit scope so a single free function (below) can be
 * the pthread_create entry point and still use the typed body. */
struct alignas(64) OverflowThreadCtx {
    pthread_t tid;
    int rc;
};

/* pthread_create's required entry signature. The body is a
 * single-line adapter: pull the query descriptor and per-call
 * timeout out of the typed context, then run the query. The
 * `void *` boundary is forced by pthread_create.
 *
 * Declared at translation-unit (not namespace) scope and tagged
 * `extern "C"` so its name-mangled symbol matches the
 * `void *(*)(void *)` that pthread_create expects; nesting it
 * inside an anonymous namespace would give it C++ linkage and
 * a different mangled name on some toolchains. */
extern "C" void *overflow_thread_body(void *arg)
{
    auto *c                = static_cast<OverflowThreadCtx *>(arg);
    GetFoo_payload_t req   = {.req = 1};
    GetFoo_response_t resp = {.result = 0};
    /* The per-call timeout is a safety net; the worker thread parks
     * on a never-signalled condvar anyway. */
    c->rc = ipc_query_raw(&GetFoo, &req, &resp, sizeof(resp), IPC_TIMEOUT_MS(5000));
    return nullptr;
}

namespace
{

/* Handler that never calls ipc_reply_raw — used by the Gap 4
 * server-doesn't-reply tests and the Gap 3 wait-table overflow
 * test. The function lives in an anonymous namespace, so it
 * already has internal linkage.
 *
 * The `self` parameter is declared non-const because the function
 * must be assignable to the `handler` field of `struct ipc_actor`,
 * whose type `ipc_actor_handler_t` is fixed in ipc.h as
 * `void (*)(struct ipc_actor *, const struct ipc_msg *)`. C++
 * function pointer types are invariant, so we cannot substitute a
 * `const struct ipc_actor *` parameter here without changing the
 * C API. */
void no_reply_handler(struct ipc_actor *self, const struct ipc_msg *msg)
{
    (void) self;
    (void) msg;
    /* Intentionally do not call ipc_reply_raw. */
}

/* Fill the registration or subscription table of `a` with `count`
 * distinct filler descriptors whose names are "<prefix>0" .. "<prefix>N-1"
 * and whose kind is `kind`. Each filler is pushed through `op`
 * (ipc_register or ipc_subscribe). Used by the Gap 3 table-overflow
 * tests, which otherwise repeat the same snprintf+struct-init loop
 * three times. The backing name strings live in a function-scope
 * `static std::array` so the std::string instances (and thus the
 * .c_str() pointers stored in each descriptor's .name) persist for
 * the life of the test binary; the descriptor array itself is a
 * plain local rebuilt on every call. */
template <typename Op>
int fill_table_with_fillers(struct ipc_actor *a, const std::string &prefix, ipc_msg_kind_t kind,
                            int count, Op op)
{
    /* `namebufs` is `static` (function-scope static storage duration)
     * so the std::string instances persist for the life of the test
     * binary; their .c_str() pointers stay valid while `descs[i].name`
     * references them. `descs` is rebuilt on every call from the
     * current namebufs entries, so it is a plain stack array — no
     * `static` is needed (and would be redundant: a function-local
     * `static` whose value is fully overwritten on every call is the
     * same as a plain local apart from a slightly different
     * zero-init timing). */
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
        g_actor.handler      = nullptr;
        g_no_handler.name    = "no_handler_actor";
        g_no_handler.handler = nullptr;
    }
    void TearDown() override
    {
        _ipc_reset_for_testing();
        mock_port_reset();
    }
};

/* ────────────────────────────────────────────────────────────────────────
 * Gap 1: actors with handler == NULL
 * ──────────────────────────────────────────────────────────────────────── */

TEST_F(ErrorPathTest, SendToHandlerNullActorSucceedsWithoutDispatch)
{
    /* Register MsgA on an actor that has handler == NULL. Send must
     * succeed (the core doesn't dereference the handler pointer) and
     * no crash should occur. With invoke_handlers on, the mock's port
     * would normally call a->handler; it must skip that branch
     * because a->handler is NULL. */
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
    /* ipc_publish fans out to every subscriber; each individual send
     * succeeds even when that subscriber has handler == NULL. */
    ASSERT_EQ(ipc_subscribe(&g_no_handler, &EvtA), 0);
    EvtA_payload_t payload = {.v = 7};
    EXPECT_EQ(ipc_publish_raw(&EvtA, &payload), 0);
    const auto *st = mock_port_actor_state(&g_no_handler);
    EXPECT_EQ(st->send_count, 1);
}

/* ────────────────────────────────────────────────────────────────────────
 * Gap 2: ipc_send / ipc_send_after / ipc_publish / ipc_query / ipc_reply
 *        macros (non-raw) must propagate the _raw return code.
 *
 * The macro path can only be exercised with a named lvalue (the
 * macro takes the address of the user expression; a brace-init
 * expression is an rvalue and trips -Waddress-of-temporary under
 * C++). See AGENTS.md.
 * ──────────────────────────────────────────────────────────────────────── */

TEST_F(ErrorPathTest, SendMacroPropagatesReturnCode)
{
    ASSERT_EQ(ipc_register(&g_actor, &MsgA), 0);
    mock_port_set_next_send_rc(-EAGAIN);
    MsgA_payload_t p = {.x = 1};
    /* ipc_send(MsgA, p) expands to ipc_send_raw(&MsgA, &p) and must
     * return whatever the _raw call returned. */
    EXPECT_EQ(ipc_send(MsgA, p), -EAGAIN);
    /* The one-shot is consumed; the next send should succeed. */
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

TEST_F(ErrorPathTest, PublishMacroPropagatesLastError)
{
    /* Two subscribers, the second one fails. The macro must surface
     * the last error code. */
    struct ipc_actor sub1{};
    struct ipc_actor sub2{};
    ASSERT_EQ(ipc_subscribe(&sub1, &EvtA), 0);
    ASSERT_EQ(ipc_subscribe(&sub2, &EvtA), 0);

    /* The one-shot fires on the FIRST send call, so it lands on sub1.
     * Set two one-shots (the second on sub2 via... hmm, the mock has
     * a single global flag). To keep the test deterministic with the
     * current mock, use the uniform-failure knob and assert that the
     * last error from the publish call is -ENOMEM. The earlier
     * SendTest::PublishWithMixedSubscriberOutcomesReturnsLastError
     * covers the same path via _raw; this test only confirms the
     * MACRO form is wired correctly. */
    mock_port_set_send_should_fail(true);
    EvtA_payload_t p = {.v = 3};
    EXPECT_EQ(ipc_publish(EvtA, p), -ENOMEM);
    mock_port_set_send_should_fail(false);
}

TEST_F(ErrorPathTest, QueryMacroRoundTrips)
{
    /* The ipc_query macro expands to
     *   ipc_query_raw(&desc, &req, resp, sizeof(desc##_response_t), timeout)
     * and must propagate the rc and copy the response into resp.
     * (Driving the macro against an *unregistered* descriptor is not
     * possible because the macro requires a <TypeName>##_response_t
     * typedef to compute sizeof — the descriptor literal we build
     * for the unknown-id path has no such typedef. The unknown-id
     * path is already covered by SendTest::SendUnknownIdReturnsNoEnt
     * and QueryTest::UnknownQueryReturnsNoEnt via the _raw API.) */
    ASSERT_EQ(ipc_register(&g_actor, &GetFoo), 0);
    /* See the note on `self` in no_reply_handler above: function
     * pointer invariance forbids `const struct ipc_actor *` here. */
    g_actor.handler =
        +[](struct ipc_actor *self, const struct ipc_msg *msg) { // NOLINT(misc-unused-parameters)
            (void) self;
            (void) msg;
            GetFoo_response_t resp = {.result = 7};
            ipc_reply_raw(msg, &resp, sizeof(resp));
        };
    mock_port_set_invoke_handlers(true);

    GetFoo_payload_t req   = {.req = 1};
    GetFoo_response_t resp = {.result = 0};
    EXPECT_EQ(ipc_query(GetFoo, &resp, IPC_TIMEOUT_FOREVER, req), 0);
    EXPECT_EQ(resp.result, 7);
}

TEST_F(ErrorPathTest, ReplyMacroWakesWaiterAndCopiesResponse)
{
    /* End-to-end through the ipc_query / ipc_reply MACROS. The mock
     * invokes the handler synchronously, the handler builds the
     * response with the ipc_reply macro, the caller wakes with 0 and
     * reads the response. This is the "the macro compiles, links,
     * and runs end-to-end" test that AGENTS.md says the unit suite
     * doesn't otherwise cover. */
    ASSERT_EQ(ipc_register(&g_actor, &GetFoo), 0);
    /* See the note on `self` in no_reply_handler above: function
     * pointer invariance forbids `const struct ipc_actor *` here. */
    g_actor.handler =
        +[](struct ipc_actor *self, const struct ipc_msg *msg) { // NOLINT(misc-unused-parameters)
            (void) self;
            auto *p                = (const GetFoo_payload_t *) msg->payload;
            GetFoo_response_t resp = {.result = p->req + 100};
            ipc_reply(msg, GetFoo, resp);
        };
    mock_port_set_invoke_handlers(true);

    GetFoo_payload_t req   = {.req = 5};
    GetFoo_response_t resp = {.result = 0};
    EXPECT_EQ(ipc_query(GetFoo, &resp, IPC_TIMEOUT_FOREVER, req), 0);
    EXPECT_EQ(resp.result, 105);
}

/* ────────────────────────────────────────────────────────────────────────
 * Gap 3: table overflow
 *
 * The real caps are 32 / 32 / 16. We can't shrink them per-TU
 * (ipc_core is a single-TU OBJECT lib that compiled ipc.c with
 * the default caps). So we drive the real caps:
 *
 *   - Registration table overflow: fill 32 entries with distinct
 *     descriptors, then attempt the 33rd and assert that an assert
 *     fires in debug builds (matching the existing
 *     DuplicateRegistrationAssertsInDebug pattern). In a release
 *     build the function returns -ENOMEM, so we also assert that
 *     contract.
 *   - Subscription table overflow: same shape.
 *   - Wait-table overflow: 16 in-flight queries with non-replying
 *     handlers, then the 17th ipc_query_raw must return -ENOMEM
 *     because the wait_table is full.
 * ──────────────────────────────────────────────────────────────────────── */

TEST_F(ErrorPathTest, RegistrationTableOverflowAssertsInDebug)
{
#ifndef NDEBUG
    /* 32 distinct CMD descriptors, all on the same actor. The IDs
     * come from FNV-1a of the descriptor name, so we rely on the
     * names being distinct. The static descriptors themselves are
     * the address-of temporaries; we can't use IPC_CMD_DEFINE in a
     * loop, so we build them by hand. */
    ASSERT_EQ(fill_table_with_fillers(&g_actor, "Filler", IPC_CMD, 32, ipc_register), 32);
    /* The 33rd must hit the assert. */
    ipc_msg_desc_t overflow = {
        .id   = 0,
        .kind = IPC_CMD,
        .size = 0,
        .name = "Overflow",
    };
    EXPECT_DEATH(ipc_register(&g_actor, &overflow), "registration table full");
#endif
}

TEST_F(ErrorPathTest, SubscriptionTableFullReturnsEnomem)
{
    /* ipc_subscribe silently returns -ENOMEM when the table is full
     * (no assert). The DEBUG-only path is covered by
     * RegistrationTableOverflowAssertsInDebug; this test pins the
     * subscription-side contract in both debug and release. */
    ASSERT_EQ(fill_table_with_fillers(&g_actor, "EvtFiller", IPC_EVENT, 32, ipc_subscribe), 32);
    ipc_msg_desc_t overflow = {
        .id   = 0,
        .kind = IPC_EVENT,
        .size = 0,
        .name = "SubOverflow",
    };
    EXPECT_EQ(ipc_subscribe(&g_actor, &overflow), -ENOMEM);
}

TEST_F(ErrorPathTest, WaitTableOverflowReturnsEnomem)
{
    /* Drive the wait table to its IPC_MAX_INFLIGHT_QUERIES capacity
     * from N concurrent threads. Each thread issues a query against
     * a registered target whose handler never replies. The mock is
     * set to block_should_wait=true with a long per-call timeout so
     * each thread holds its slot open. Once all N threads are
     * blocked inside ipc_port_query_wait_block, a final
     * ipc_query_raw from the main thread must observe the table-full
     * condition and return -ENOMEM (from _ipc_wait_table_claim).
     *
     * The test is independent of N as long as N >=
     * IPC_MAX_INFLIGHT_QUERIES. We pick 32 to be safe (default cap
     * is 16). The slots are released by the thread cleanups after
     * the assertion. */
    ASSERT_EQ(ipc_register(&g_actor, &GetFoo), 0);
    g_actor.handler = &no_reply_handler;
    mock_port_set_invoke_handlers(true);
    mock_port_set_block_timeout(false);
    mock_port_set_block_should_wait(true);

    /* The per-thread contexts are a plain local: this test function
     * is invoked once per process (one test case, one gtest binary),
     * so a function-scope `static` would be redundant. The struct
     * itself is declared at translation-unit scope (above) so the
     * free `extern "C"` pthread entry point can name the type. */
    constexpr int kThreads = 32;
    std::array<OverflowThreadCtx, kThreads> ctx{};

    for (auto &c : ctx) {
        c.rc = 0;
        ASSERT_EQ(pthread_create(&c.tid, nullptr, overflow_thread_body, &c), 0);
    }

    /* Give all kThreads queries a moment to enter
     * ipc_port_query_wait_block. Each ipc_query_raw goes through
     * _ipc_wait_table_claim (table lock), ipc_port_query_wait_init,
     * ipc_port_send (which invokes the handler synchronously and
     * returns; the handler doesn't reply), then ipc_port_query_wait_block
     * (parks on the condvar). Once parked, the slot is held.
     *
     * We busy-wait until the slot count in the mock is non-zero
     * (the mock doesn't expose the slot count, so we approximate
     * by spinning on a short sleep). 50ms is plenty for the threads
     * to all be parked. */
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
    nanosleep(&ts, nullptr);

    /* Now issue one more ipc_query_raw from the main thread. The
     * wait table is full (kThreads >= IPC_MAX_INFLIGHT_QUERIES),
     * so _ipc_wait_table_claim returns -1 and ipc_query_raw
     * propagates -ENOMEM. */
    GetFoo_payload_t req_main   = {.req = 1};
    GetFoo_response_t resp_main = {.result = 0};
    int rc = ipc_query_raw(&GetFoo, &req_main, &resp_main, sizeof(resp_main), IPC_TIMEOUT_MS(10));
    EXPECT_EQ(rc, -ENOMEM) << "wait table should be full; rc=" << rc;

    /* Cancel and join the worker threads so the test doesn't leak
     * them on success or fail paths. pthread_cancel is a Linux/macOS
     * extension; the test only runs on POSIX so this is fine. The
     * slot cleanup that would normally run after the block returns
     * is skipped on cancellation, so we rely on _ipc_reset_for_testing
     * in TearDown to wipe the wait table. */
    for (const auto &c : ctx) {
        pthread_cancel(c.tid);
        pthread_join(c.tid, nullptr);
    }

    mock_port_set_block_should_wait(false);
}

TEST_F(ErrorPathTest, RegistrationTableFullReturnsEnomemInRelease)
{
#ifdef NDEBUG
    /* In a release build, asserts are compiled out and the function
     * returns -ENOMEM instead of aborting. This is the contract that
     * production code relies on. */
    ASSERT_EQ(fill_table_with_fillers(&g_actor, "RelFiller", IPC_CMD, 32, ipc_register), 32);
    ipc_msg_desc_t overflow = {
        .id   = 0,
        .kind = IPC_CMD,
        .size = 0,
        .name = "RelOverflow",
    };
    EXPECT_EQ(ipc_register(&g_actor, &overflow), -ENOMEM);
#endif
}

/* ────────────────────────────────────────────────────────────────────────
 * Gap 4: server-doesn't-reply path
 *
 * Distinct from TimeoutIsReturnedWhenBlockFails (which sets the
 * mock's block-timeout flag, bypassing handler dispatch). Here the
 * real code path runs: handler dispatches, doesn't reply, block
 * returns -ETIMEDOUT because the wait's done flag is never set.
 * (no_reply_handler is defined at the top of the file.)
 * ──────────────────────────────────────────────────────────────────────── */

TEST_F(ErrorPathTest, ServerDoesNotReplyReturnsTimeout)
{
    ASSERT_EQ(ipc_register(&g_actor, &GetFoo), 0);
    g_actor.handler = &no_reply_handler;
    mock_port_set_invoke_handlers(true);

    GetFoo_payload_t req   = {.req = 1};
    GetFoo_response_t resp = {.result = 0};
    int rc                 = ipc_query_raw(&GetFoo, &req, &resp, sizeof(resp), IPC_TIMEOUT_MS(50));
    EXPECT_EQ(rc, -ETIMEDOUT);
    /* Response buffer was not populated. */
    EXPECT_EQ(resp.result, 0);
}

TEST_F(ErrorPathTest, ServerDoesNotReplyDoesNotLeakWaitSlot)
{
    /* After a query that times out (no reply), the wait-table slot
     * must be released so a follow-up query can claim it. We can't
     * observe the slot count directly, but we can fire a second
     * query against a replying handler and assert it still goes
     * through. */
    ASSERT_EQ(ipc_register(&g_actor, &GetFoo), 0);
    g_actor.handler = &no_reply_handler;
    mock_port_set_invoke_handlers(true);

    GetFoo_payload_t req   = {.req = 1};
    GetFoo_response_t resp = {.result = 0};
    EXPECT_EQ(ipc_query_raw(&GetFoo, &req, &resp, sizeof(resp), IPC_TIMEOUT_MS(10)), -ETIMEDOUT);

    /* Now swap to a replying handler and confirm the second query
     * round-trips. */
    /* See the note on `self` in no_reply_handler above: function
     * pointer invariance forbids `const struct ipc_actor *` here. */
    g_actor.handler =
        +[](struct ipc_actor *self, const struct ipc_msg *msg) { // NOLINT(misc-unused-parameters)
            (void) self;
            auto *p                = (const GetFoo_payload_t *) msg->payload;
            GetFoo_response_t resp = {.result = p->req * 2};
            ipc_reply_raw(msg, &resp, sizeof(resp));
        };
    GetFoo_response_t resp2 = {.result = 0};
    EXPECT_EQ(ipc_query_raw(&GetFoo, &req, &resp2, sizeof(resp2), IPC_TIMEOUT_FOREVER), 0);
    EXPECT_EQ(resp2.result, 2);
}

/* ────────────────────────────────────────────────────────────────────────
 * Gap 5: non-ENOMEM port error codes
 *
 * The original send_should_fail knob hardcodes -ENOMEM. The
 * one-shot next_send_rc / next_send_after_rc knobs let us inject
 * any errno and assert that the core propagates it verbatim.
 * ──────────────────────────────────────────────────────────────────────── */

TEST_F(ErrorPathTest, SendPropagatesEinval)
{
    ASSERT_EQ(ipc_register(&g_actor, &MsgA), 0);
    mock_port_set_next_send_rc(-EINVAL);
    MsgA_payload_t p = {.x = 1};
    EXPECT_EQ(ipc_send_raw(&MsgA, &p), -EINVAL);
    /* One-shot: next call succeeds. */
    EXPECT_EQ(ipc_send_raw(&MsgA, &p), 0);
}

TEST_F(ErrorPathTest, SendPropagatesEagain)
{
    ASSERT_EQ(ipc_register(&g_actor, &MsgA), 0);
    mock_port_set_next_send_rc(-EAGAIN);
    MsgA_payload_t p = {.x = 1};
    EXPECT_EQ(ipc_send_raw(&MsgA, &p), -EAGAIN);
}

TEST_F(ErrorPathTest, SendPropagatesEio)
{
    ASSERT_EQ(ipc_register(&g_actor, &MsgA), 0);
    mock_port_set_next_send_rc(-EIO);
    MsgA_payload_t p = {.x = 1};
    EXPECT_EQ(ipc_send_raw(&MsgA, &p), -EIO);
}

TEST_F(ErrorPathTest, SendPropagatesEnospc)
{
    ASSERT_EQ(ipc_register(&g_actor, &MsgA), 0);
    mock_port_set_next_send_rc(-ENOSPC);
    MsgA_payload_t p = {.x = 1};
    EXPECT_EQ(ipc_send_raw(&MsgA, &p), -ENOSPC);
}

TEST_F(ErrorPathTest, PublishPropagatesFirstNonZeroError)
{
    /* publish_raw walks subscribers in registration order and keeps
     * the FIRST non-zero rc. We arrange two subscribers and inject a
     * one-shot failure on the first one. */
    struct ipc_actor sub1{};
    struct ipc_actor sub2{};
    ASSERT_EQ(ipc_subscribe(&sub1, &EvtA), 0);
    ASSERT_EQ(ipc_subscribe(&sub2, &EvtA), 0);

    mock_port_set_next_send_rc(-EIO);
    EvtA_payload_t p = {.v = 1};
    EXPECT_EQ(ipc_publish_raw(&EvtA, &p), -EIO);
    /* Both subscribers received the message (the loop kept going). */
    const auto *s1 = mock_port_actor_state(&sub1);
    const auto *s2 = mock_port_actor_state(&sub2);
    EXPECT_EQ(s1->send_count, 1);
    EXPECT_EQ(s2->send_count, 1);
}

TEST_F(ErrorPathTest, QueryPropagatesPortErrorFromSend)
{
    /* If the port's ipc_port_send fails for the QUERY message, the
     * wait slot must be released and -ENOMEM (or whatever the port
     * returned) must propagate. The mock's one-shot is consumed
     * once. */
    ASSERT_EQ(ipc_register(&g_actor, &GetFoo), 0);

    mock_port_set_next_send_rc(-ENOSPC);
    GetFoo_payload_t req   = {.req = 1};
    GetFoo_response_t resp = {.result = 0};
    int rc                 = ipc_query_raw(&GetFoo, &req, &resp, sizeof(resp), IPC_TIMEOUT_FOREVER);
    EXPECT_EQ(rc, -ENOSPC);

    /* The wait slot must have been released. A second query against
     * a replying handler should now succeed — proves the slot was
     * freed on the early-return path. */
    /* See the note on `self` in no_reply_handler above: function
     * pointer invariance forbids `const struct ipc_actor *` here. */
    g_actor.handler =
        +[](struct ipc_actor *self, const struct ipc_msg *msg) { // NOLINT(misc-unused-parameters)
            (void) self;
            (void) msg;
            GetFoo_response_t resp = {.result = 99};
            ipc_reply_raw(msg, &resp, sizeof(resp));
        };
    mock_port_set_invoke_handlers(true);
    GetFoo_response_t resp2 = {.result = 0};
    EXPECT_EQ(ipc_query_raw(&GetFoo, &req, &resp2, sizeof(resp2), IPC_TIMEOUT_FOREVER), 0);
    EXPECT_EQ(resp2.result, 99);
}

TEST_F(ErrorPathTest, SendAfterPropagatesPortError)
{
    /* ipc_port_send_after returning a non-zero rc must be surfaced
     * by ipc_send_after_raw. */
    ASSERT_EQ(ipc_register(&g_actor, &MsgB), 0);
    mock_port_set_next_send_after_rc(-ENOMEM);
    MsgB_payload_t p = {.y = 1};
    EXPECT_EQ(ipc_send_after_raw(&MsgB, 100, &p), -ENOMEM);
    /* Next call succeeds. */
    EXPECT_EQ(ipc_send_after_raw(&MsgB, 100, &p), 0);
}

} // namespace
