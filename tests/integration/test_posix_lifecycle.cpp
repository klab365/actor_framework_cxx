#include <gtest/gtest.h>

extern "C" {
#include "ipc.h"
}

namespace
{

IPC_ACTOR_DEFINE(posix_lifecycle_actor, "posix_lifecycle", 16384, 0, 2, 0);

TEST(PosixLifecycleIntegration, RequiresJoinBeforeRestart)
{
    ASSERT_EQ(ipc_start_all_actors(), 0);
    ipc_stop_all();

    EXPECT_EQ(ipc_start_all_actors(), -EBUSY);
    ASSERT_EQ(ipc_run_all(), 0);

    ASSERT_EQ(ipc_start_all_actors(), 0);
    ipc_stop_all();
    EXPECT_EQ(ipc_run_all(), 0);
}

} // namespace
