/*
 * test_ipc_lifecycle.cpp — Unit tests for ipc_actor_init / start_all /
 * run_all / stop_all against the mock port.
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

IPC_CMD_DEFINE(NoOpCmd, { int dummy; });

struct ipc_actor g_a, g_b;

class LifecycleTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        _ipc_reset_for_testing();
        mock_port_init();
        memset(&g_a, 0, sizeof(g_a));
        memset(&g_b, 0, sizeof(g_b));
        g_a.name    = "a";
        g_a.handler = nullptr;
        g_b.name    = "b";
        g_b.handler = nullptr;
    }
    void TearDown() override
    {
        _ipc_reset_for_testing();
        mock_port_reset();
    }
};

TEST_F(LifecycleTest, ActorInitRegistersInList)
{
    ASSERT_EQ(ipc_actor_init(&g_a, "a", nullptr, {0, 0, 0}), 0);
    /* The actor is now in the global list — start_all will see it. */
    ASSERT_EQ(ipc_actor_init(&g_b, "b", nullptr, {0, 0, 0}), 0);

    ASSERT_EQ(ipc_start_all_threads(), 0);

    auto *sa = mock_port_actor_state(&g_a);
    auto *sb = mock_port_actor_state(&g_b);
    EXPECT_EQ(sa->start_count, 1);
    EXPECT_EQ(sb->start_count, 1);
}

TEST_F(LifecycleTest, RunAllReturnsProgrammableRc)
{
    ipc_actor_init(&g_a, "a", nullptr, {0, 0, 0});
    ASSERT_EQ(ipc_start_all_threads(), 0);

    mock_port_set_run_all_rc(-EIO);
    EXPECT_EQ(ipc_run_all(), -EIO);

    mock_port_set_run_all_rc(0);
}

TEST_F(LifecycleTest, StopAllCallsStopOnEveryActor)
{
    ipc_actor_init(&g_a, "a", nullptr, {0, 0, 0});
    ipc_actor_init(&g_b, "b", nullptr, {0, 0, 0});

    ipc_stop_all();
    EXPECT_EQ(mock_port_actor_state(&g_a)->stop_count, 1);
    EXPECT_EQ(mock_port_actor_state(&g_b)->stop_count, 1);
}

TEST_F(LifecycleTest, ActorInitPreservesCfgFields)
{
    struct ipc_actor_cfg cfg = {
        .stack_size  = 1024,
        .priority    = 7,
        .queue_depth = 4,
    };
    ASSERT_EQ(ipc_actor_init(&g_a, "a", nullptr, cfg), 0);
    /* Round-trip the three cfg fields through the actor struct. The core
     * does an aggregate copy; this locks that in against a future
     * field-by-field refactor that might drop one. */
    EXPECT_EQ(g_a.cfg.stack_size, 1024u);
    EXPECT_EQ(g_a.cfg.priority, 7);
    EXPECT_EQ(g_a.cfg.queue_depth, 4u);
}

TEST_F(LifecycleTest, StartAllOnEmptyActorListIsNoop)
{
    /* No ipc_actor_init calls. start_all must walk the empty list and
     * return 0 without dereferencing anything. */
    EXPECT_EQ(ipc_start_all_threads(), 0);
}

TEST_F(LifecycleTest, StartAllPropagatesFirstPortError)
{
    ipc_actor_init(&g_a, "a", nullptr, {0, 0, 0});
    ipc_actor_init(&g_b, "b", nullptr, {0, 0, 0});

    mock_port_set_next_start_should_fail(&g_a);
    int rc = ipc_start_all_threads();
    /* Core returns early on the first non-zero rc. */
    EXPECT_EQ(rc, -EINVAL);
    /* And must not have continued to start b. */
    EXPECT_EQ(mock_port_actor_state(&g_a)->start_count, 1);
    EXPECT_EQ(mock_port_actor_state(&g_b)->start_count, 0);
}

TEST_F(LifecycleTest, StopAllOnEmptyActorListIsNoop)
{
    /* No ipc_actor_init calls. stop_all must walk the empty list and
     * return without dereferencing anything. */
    ipc_stop_all(); /* must not crash */
}

} // namespace
