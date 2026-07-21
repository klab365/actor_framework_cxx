/*
 * test_ipc_lifecycle.cpp — Unit tests for static actor registration / start_all /
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
IPC_EVENT_DEFINE(NoOpEvent, { int dummy; });

struct ipc_actor g_a, g_b;

static void define_test_actor(struct ipc_actor *a, const char *name, struct ipc_actor_cfg cfg)
{
    memset(a, 0, sizeof(*a));
    a->name    = name;
    a->handler = nullptr;
    a->cfg     = cfg;
    _ipc_actor_register_static(a);
}

class LifecycleTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        _ipc_reset_for_testing();
        mock_port_init();
        memset(&g_a, 0, sizeof(g_a));
        memset(&g_b, 0, sizeof(g_b));
    }
    void TearDown() override
    {
        _ipc_reset_for_testing();
        mock_port_reset();
    }
};

TEST_F(LifecycleTest, StaticActorRegistrationAddsToListButDoesNotStart)
{
    define_test_actor(&g_a, "a", {0, 0, 0});
    define_test_actor(&g_b, "b", {0, 0, 0});

    EXPECT_EQ(mock_port_actor_state(&g_a)->start_count, 0);
    EXPECT_EQ(mock_port_actor_state(&g_b)->start_count, 0);

    ASSERT_EQ(ipc_start_all_actors(), 0);
    EXPECT_EQ(mock_port_actor_state(&g_a)->start_count, 1);
    EXPECT_EQ(mock_port_actor_state(&g_b)->start_count, 1);
}

TEST_F(LifecycleTest, RunAllReturnsProgrammableRc)
{
    define_test_actor(&g_a, "a", {0, 0, 0});
    ASSERT_EQ(ipc_start_all_actors(), 0);

    mock_port_set_run_all_rc(-EIO);
    EXPECT_EQ(ipc_run_all(), -EIO);

    mock_port_set_run_all_rc(0);
}

TEST_F(LifecycleTest, StopAllCallsStopOnEveryActor)
{
    define_test_actor(&g_a, "a", {0, 0, 0});
    define_test_actor(&g_b, "b", {0, 0, 0});
    ASSERT_EQ(ipc_start_all_actors(), 0);

    ipc_stop_all();
    EXPECT_EQ(mock_port_actor_state(&g_a)->stop_count, 1);
    EXPECT_EQ(mock_port_actor_state(&g_b)->stop_count, 1);
}

TEST_F(LifecycleTest, StaticActorRegistrationPreservesCfgFields)
{
    struct ipc_actor_cfg cfg = {
        .stack_size  = 1024,
        .priority    = 7,
        .queue_depth = 4,
    };
    define_test_actor(&g_a, "a", cfg);

    EXPECT_EQ(g_a.cfg.stack_size, 1024u);
    EXPECT_EQ(g_a.cfg.priority, 7);
    EXPECT_EQ(g_a.cfg.queue_depth, 4u);
}

TEST_F(LifecycleTest, StartAllOnEmptyActorListIsNoop)
{
    EXPECT_EQ(ipc_start_all_actors(), 0);
}

TEST_F(LifecycleTest, StartAllPropagatesFirstPortError)
{
    define_test_actor(&g_a, "a", {0, 0, 0});
    define_test_actor(&g_b, "b", {0, 0, 0});
    mock_port_set_next_start_should_fail(&g_a);

    int rc = ipc_start_all_actors();
    EXPECT_EQ(rc, -EINVAL);
    EXPECT_EQ(mock_port_actor_state(&g_a)->start_count, 1);
    EXPECT_EQ(mock_port_actor_state(&g_b)->start_count, 0);
}

TEST_F(LifecycleTest, LateRegisterFailsAfterStartAllFreezesRegistry)
{
    define_test_actor(&g_a, "a", {0, 0, 0});
    ASSERT_EQ(ipc_start_all_actors(), 0);

    EXPECT_EQ(ipc_register(&g_a, &NoOpCmd), -EPERM);
}

TEST_F(LifecycleTest, LateSubscribeFailsAfterStartAllFreezesRegistry)
{
    define_test_actor(&g_a, "a", {0, 0, 0});
    ASSERT_EQ(ipc_start_all_actors(), 0);

    EXPECT_EQ(ipc_subscribe(&g_a, &NoOpEvent), -EPERM);
}

TEST_F(LifecycleTest, LateUnsubscribeFailsAfterStartAllFreezesRegistry)
{
    define_test_actor(&g_a, "a", {0, 0, 0});
    ASSERT_EQ(ipc_subscribe(&g_a, &NoOpEvent), 0);
    ASSERT_EQ(ipc_start_all_actors(), 0);

    EXPECT_EQ(ipc_unsubscribe(&g_a, &NoOpEvent), -EPERM);
}

TEST_F(LifecycleTest, StopAllOnEmptyActorListIsNoop)
{
    ipc_stop_all(); /* must not crash */
}

} // namespace
