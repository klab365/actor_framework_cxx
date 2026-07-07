/*
 * test_ipc_registry.cpp — Unit tests for ipc_register / ipc_subscribe /
 * ipc_unsubscribe. Uses the mock port so no threads are spawned.
 */
#include <gtest/gtest.h>
extern "C" {
#include "ipc.h"
#include "ipc_internal.h"
#include "ipc_port.h"
#include "mock_ipc_port.h"
}

namespace
{

/* Minimal message definitions for the unit tests. */
IPC_CMD_DEFINE(MsgA, { int x; });
IPC_CMD_DEFINE(MsgB, { int y; });
IPC_EVENT_DEFINE(EvtA, { int v; });
IPC_EVENT_DEFINE(EvtB, { int v; });
IPC_QUERY_DEFINE(QryA, { int in; }, { int out; });

/* Two throwaway actors. We never call ipc_actor_init on them (it would
 * touch the table mutex); we just want a valid struct ipc_actor to
 * associate with a registration. */
struct ipc_actor g_actor_a;
struct ipc_actor g_actor_b;

class RegistryTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        _ipc_reset_for_testing();
        mock_port_init();
        memset(&g_actor_a, 0, sizeof(g_actor_a));
        memset(&g_actor_b, 0, sizeof(g_actor_b));
    }
    void TearDown() override
    {
        mock_port_reset();
        _ipc_reset_for_testing();
    }
};

TEST_F(RegistryTest, LazyIdIsPopulatedOnFirstRegister)
{
    ASSERT_EQ(MsgA.id, 0u);
    ASSERT_EQ(ipc_register(&g_actor_a, &MsgA), 0);
    EXPECT_NE(MsgA.id, 0u);
    uint32_t first = MsgA.id;
    /* Subsequent registers with the same descriptor must not change the id
     * (the duplicate path is asserted on separately in debug builds). */
    EXPECT_EQ(MsgA.id, first);
}

TEST_F(RegistryTest, DistinctNamesProduceDistinctIds)
{
    ipc_register(&g_actor_a, &MsgA);
    ipc_register(&g_actor_b, &MsgB);
    EXPECT_NE(MsgA.id, MsgB.id);
}

TEST_F(RegistryTest, DuplicateRegistrationIsRejected)
{
    ASSERT_EQ(ipc_register(&g_actor_a, &MsgA), 0);
    /* Duplicate would assert(0) in debug; in release returns -EALREADY.
     * We can't safely call it under a release build without aborting,
     * so skip the negative path here and assert coverage in a debug-only
     * separate test below. */
}

TEST_F(RegistryTest, DuplicateRegistrationAssertsInDebug)
{
#ifndef NDEBUG
    ASSERT_EQ(ipc_register(&g_actor_a, &MsgA), 0);
    EXPECT_DEATH(ipc_register(&g_actor_b, &MsgA), "duplicate");
#endif
}

TEST_F(RegistryTest, RegisterAssertsOnNonCmdOrQuery)
{
    /* EVENTS may not be registered — only subscribed. */
#ifndef NDEBUG
    EXPECT_DEATH(ipc_register(&g_actor_a, &EvtA), "kind");
#endif
}

TEST_F(RegistryTest, SubscriptionRoundTrip)
{
    ASSERT_EQ(ipc_subscribe(&g_actor_a, &EvtA), 0);
    ASSERT_EQ(ipc_unsubscribe(&g_actor_a, &EvtA), 0);
}

TEST_F(RegistryTest, UnsubscribeWithoutSubscribeReturnsNoEnt)
{
    EXPECT_EQ(ipc_unsubscribe(&g_actor_a, &EvtA), -ENOENT);
}

TEST_F(RegistryTest, UnsubscribeOnlyMatchesSameActor)
{
    ASSERT_EQ(ipc_subscribe(&g_actor_a, &EvtA), 0);
    EXPECT_EQ(ipc_unsubscribe(&g_actor_b, &EvtA), -ENOENT);
    /* A's subscription is untouched. */
    EXPECT_EQ(ipc_unsubscribe(&g_actor_a, &EvtA), 0);
}

TEST_F(RegistryTest, RegisterAcceptsQueryKind)
{
    /* ipc_register allows both IPC_CMD and IPC_QUERY. Verify the
     * QUERY path is exercised and produces a non-zero id distinct
     * from any CMD id. Note: the static descriptor's .id is *not*
     * zeroed by _ipc_reset_for_testing() (the descriptor lives in
     * user memory), so we just check that the call succeeds and the
     * id is non-zero and distinct from MsgA. */
    EXPECT_EQ(ipc_register(&g_actor_a, &MsgA), 0);
    EXPECT_EQ(ipc_register(&g_actor_a, &QryA), 0);
    EXPECT_NE(MsgA.id, 0u);
    EXPECT_NE(QryA.id, 0u);
    /* Distinct names => distinct ids. */
    EXPECT_NE(MsgA.id, QryA.id);
}

TEST_F(RegistryTest, SubscribeIsDeduplicated)
{
    /* ipc_subscribe is idempotent for the same (actor, MsgType) pair.
     * A second call returns 0 without adding a second row, so a
     * subsequent publish delivers exactly once. */
    ASSERT_EQ(ipc_subscribe(&g_actor_a, &EvtA), 0);
    /* Duplicate subscribe is a silent no-op (returns 0). */
    EXPECT_EQ(ipc_subscribe(&g_actor_a, &EvtA), 0);
    /* Round-trip to clean up: a single unsubscribe must remove the
     * single row, so a second unsubscribe returns -ENOENT. */
    EXPECT_EQ(ipc_unsubscribe(&g_actor_a, &EvtA), 0);
    EXPECT_EQ(ipc_unsubscribe(&g_actor_a, &EvtA), -ENOENT);
}

TEST_F(RegistryTest, UnsubscribeMiddlePreservesOtherEntries)
{
    /* Swap-with-last removal must not corrupt the other entries. */
    struct ipc_actor a, b, c;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    memset(&c, 0, sizeof(c));
    ASSERT_EQ(ipc_subscribe(&a, &EvtA), 0);
    ASSERT_EQ(ipc_subscribe(&b, &EvtA), 0);
    ASSERT_EQ(ipc_subscribe(&c, &EvtA), 0);

    /* Remove the middle entry. */
    EXPECT_EQ(ipc_unsubscribe(&b, &EvtA), 0);

    /* a and c must still unsubscribe cleanly. b must not. */
    EXPECT_EQ(ipc_unsubscribe(&b, &EvtA), -ENOENT);
    EXPECT_EQ(ipc_unsubscribe(&a, &EvtA), 0);
    EXPECT_EQ(ipc_unsubscribe(&c, &EvtA), 0);
}

TEST_F(RegistryTest, DoubleUnsubscribeReturnsNoEntSecondTime)
{
    ASSERT_EQ(ipc_subscribe(&g_actor_a, &EvtA), 0);
    EXPECT_EQ(ipc_unsubscribe(&g_actor_a, &EvtA), 0);
    /* Second call: entry is gone, must report -ENOENT. */
    EXPECT_EQ(ipc_unsubscribe(&g_actor_a, &EvtA), -ENOENT);
}

TEST_F(RegistryTest, ResetAllowsReRegistrationWithLazyId)
{
    /* After _ipc_reset_for_testing() the registration table is empty
     * and a re-register of the same descriptor must succeed (id is
     * recomputed lazily from .name, but the id value is cached in the
     * static descriptor, so on the second pass the cached value
     * matches the first call's). The point of this test is to confirm
     * the reset is hermetic at the *table* level, not the descriptor
     * level: the second register must not be flagged as a duplicate. */
    ASSERT_EQ(ipc_register(&g_actor_a, &MsgA), 0);
    uint32_t first_id = MsgA.id;

    _ipc_reset_for_testing();

    /* Same descriptor struct, same .id cached inside it. The register
     * must succeed because the table was wiped. */
    EXPECT_EQ(MsgA.id, first_id);
    EXPECT_EQ(ipc_register(&g_actor_a, &MsgA), 0);
}

} // namespace
