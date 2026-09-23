/* ipc_actor_define.h — deterministic test-port actor declarations. */
#pragma once

/*
 * Actor declaration and static-registration mechanics are independent of
 * message delivery. Reuse the host constructor implementation; ipc_test_port
 * ignores the POSIX port state stored on the actor and supplies all transport
 * operations itself.
 */
#include "../posix/ipc_actor_define.h"
