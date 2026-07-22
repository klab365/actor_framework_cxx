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
void test_ipc_hooks_reset_counters(void);
void test_ipc_hooks_register_actor(void);
struct ipc_actor *test_ipc_hooks_actor(void);
int test_ipc_hooks_start_count(void);
int test_ipc_hooks_stop_count(void);
int test_ipc_hooks_unknown_count(void);
uint32_t test_ipc_hooks_unknown_id(void);
int test_ipc_hooks_failure_count(void);
int test_ipc_hooks_failure_reason(void);
void test_ipc_hooks_dispatch_unknown(uint32_t msg_id);
}

namespace
{

IPC_CMD_DEFINE(LifecycleCmd, { int dummy; });

struct ipc_actor g_a, g_b;

static void lifecycle_cmd_handler(struct ipc_actor *self, const void *payload,
                                  const struct ipc_msg *raw_msg)
{
    (void) self;
    (void) payload;
    (void) raw_msg;
}

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

TEST_F(LifecycleTest, ReRegisteringExistingActorForHandlerDoesNotCorruptActorList)
{
    define_test_actor(&g_a, "a", {0, 0, 0});
    define_test_actor(&g_b, "b", {0, 0, 0});

    _ipc_actor_register_handler_static(&g_a, &LifecycleCmd, lifecycle_cmd_handler);

    ASSERT_EQ(ipc_start_all_actors(), 0);
    EXPECT_EQ(mock_port_actor_state(&g_a)->start_count, 1);
    EXPECT_EQ(mock_port_actor_state(&g_b)->start_count, 1);
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

TEST_F(LifecycleTest, StopAllOnEmptyActorListIsNoop)
{
    ipc_stop_all(); /* must not crash */
}

TEST_F(LifecycleTest, ActorStartAndStopHookMacrosAreCalled)
{
    test_ipc_hooks_reset_counters();
    test_ipc_hooks_register_actor();

    ASSERT_EQ(ipc_start_all_actors(), 0);
    EXPECT_EQ(test_ipc_hooks_start_count(), 1);
    EXPECT_EQ(test_ipc_hooks_stop_count(), 0);

    ipc_stop_all();
    EXPECT_EQ(test_ipc_hooks_stop_count(), 1);
}

TEST_F(LifecycleTest, ActorUnknownHookMacroIsCalledForUnhandledMessage)
{
    test_ipc_hooks_reset_counters();

    constexpr uint32_t unknown_id = 0x12345678u;
    test_ipc_hooks_dispatch_unknown(unknown_id);

    EXPECT_EQ(test_ipc_hooks_unknown_count(), 1);
    EXPECT_EQ(test_ipc_hooks_unknown_id(), unknown_id);
}

TEST_F(LifecycleTest, ActorFailureRejectsNullActor)
{
    EXPECT_EQ(ipc_actor_fail(nullptr, -EIO), -EINVAL);
}

TEST_F(LifecycleTest, ActorFailureWithDefaultSupervisionDoesNothing)
{
    define_test_actor(&g_a, "a", {0, 0, 0});

    EXPECT_EQ(ipc_actor_fail(&g_a, -EIO), 0);
    EXPECT_EQ(mock_port_actor_state(&g_a)->stop_count, 0);
    EXPECT_EQ(mock_port_actor_state(&g_a)->restart_count, 0);
}

TEST_F(LifecycleTest, ActorFailureWithRestartSupervisionRestartsAndCallsHooks)
{
    test_ipc_hooks_reset_counters();
    test_ipc_hooks_register_actor();
    struct ipc_actor *actor = test_ipc_hooks_actor();

    EXPECT_EQ(ipc_actor_fail(actor, -EIO), 0);

    EXPECT_EQ(test_ipc_hooks_failure_count(), 1);
    EXPECT_EQ(test_ipc_hooks_failure_reason(), -EIO);
    EXPECT_EQ(test_ipc_hooks_stop_count(), 1);
    EXPECT_EQ(test_ipc_hooks_start_count(), 1);
    EXPECT_EQ(mock_port_actor_state(actor)->restart_count, 1);
}

TEST_F(LifecycleTest, ActorFailureWithStopSupervisionStopsActor)
{
    define_test_actor(&g_a, "a", {0, 0, 0});
    _ipc_actor_register_supervision_static(&g_a, IPC_SUPERVISE_STOP);

    EXPECT_EQ(ipc_actor_fail(&g_a, -EIO), 0);
    EXPECT_EQ(mock_port_actor_state(&g_a)->stop_count, 1);
    EXPECT_EQ(mock_port_actor_state(&g_a)->restart_count, 0);
}

TEST_F(LifecycleTest, ActorFailurePropagatesRestartErrorAndSkipsStartHook)
{
    test_ipc_hooks_reset_counters();
    test_ipc_hooks_register_actor();
    struct ipc_actor *actor = test_ipc_hooks_actor();

    mock_port_set_next_restart_rc(-EAGAIN);
    EXPECT_EQ(ipc_actor_fail(actor, -EIO), -EAGAIN);

    EXPECT_EQ(test_ipc_hooks_failure_count(), 1);
    EXPECT_EQ(test_ipc_hooks_stop_count(), 1);
    EXPECT_EQ(test_ipc_hooks_start_count(), 0);
    EXPECT_EQ(mock_port_actor_state(actor)->restart_count, 1);
}

TEST_F(LifecycleTest, ActorFailureRejectsInvalidSupervisionStrategy)
{
    define_test_actor(&g_a, "a", {0, 0, 0});
    _ipc_actor_register_supervision_static(&g_a, static_cast<ipc_supervision_strategy_t>(99));

    EXPECT_EQ(ipc_actor_fail(&g_a, -EIO), -EINVAL);
    EXPECT_EQ(mock_port_actor_state(&g_a)->stop_count, 0);
    EXPECT_EQ(mock_port_actor_state(&g_a)->restart_count, 0);
}

} // namespace
