# Timer wheel design

This document describes the current delayed-send implementation in the
IPC framework and the design constraints that shape it.

## Public surface

Two entry points deliver a message on a delay:

```c
int ipc_send_after(MsgType, delay_ms, payload);
int ipc_send_after_raw(ipc_msg_desc_t *desc, uint32_t delay_ms, const void *payload);
```

`delay_ms` is a `uint32_t` in milliseconds. A delay of `0` is valid
and means "schedule for the next tick of the port's worker".

The public contract (AGENTS.md):

- **One delayed message per actor.** A new `ipc_send_after` call for
  the same target actor replaces the previous pending delayed message.
- **No cancellation.** The framework does not return a handle and has
  no `ipc_send_after_cancel` symbol. The only way to abort a pending
  delayed message is to issue a new `ipc_send_after` to the same actor
  (which replaces it) or to stop the actor via `ipc_stop_all`.
- **Unknown target returns `-ENOENT`.** `ipc_send_after_raw` looks the
  target up by message ID in the registration table; a hit is
  required, otherwise the call fails before scheduling anything.
- **The `ipc_send_after` macro takes the payload as a single typed
  expression** — the same compound-literal / variable convention as
  `ipc_send`.

The contract is enforced by the per-port implementation, not by the
core (`src/ipc.c`). `ipc_send_after_raw` itself only does the target
lookup; the actual scheduling is delegated to
`ipc_port_send_after(actor, msg, delay_ms)`, which is the entire
port-side surface for delayed sends.

## Per-port implementation

Each port maintains a **per-actor** delay primitive. The core does
not know which primitive is used; it only knows that the port will
deliver `msg` to the actor's inbox after at least `delay_ms` has
elapsed.

### POSIX port (`src/port/posix/posix_ipc_port.c`)

One helper `pthread_t` per actor (`p->delay_thread`), guarded by
`p->delay_lock` / `p->delay_cond`, holding a copy of the pending
message and a "cancel" flag.

```
ipc_port_send_after(a, msg, delay_ms)
  ├─ if previous delay thread is still active:
  │   set p->delay_cancel = true, signal cond, unlock, join thread
  ├─ p->delay_msg = *msg; p->delay_ms = delay_ms
  └─ pthread_create(delay_thread_fn)

delay_thread_fn(arg = a)
  ├─ pthread_cond_timedwait(p->delay_cond, p->delay_lock, abs_deadline)
  ├─ if !p->delay_cancel:
  │     ipc_port_send(a, &p->delay_msg)   /* synchronous, copies into the actor's ring */
  └─ thread exit; p->delay_active stays true until the NEXT send_after (or stop_actor) joins us
```

`p->delay_active` is a "join in progress" flag, not a "currently
running" flag. It stays `true` from `pthread_create` success until the
next `send_after` (or `stop_actor`) calls `pthread_join`. The join
itself is what guarantees the previous delay thread has finished its
unlocked `ipc_port_send` tail before a new `delay_msg` is installed —
this is the mechanism that preserves the "replace previous" contract
on POSIX.

If `pthread_create` fails, `delay_active` is left `false` so the next
call does not try to join a thread that was never created.

`ipc_port_stop_actor` cancels and joins the delay thread (if active),
sets `p->running = false`, broadcasts the actor's main cond, and
joins the actor's main thread.

Cost characteristics: every `ipc_send_after` pays one
`pthread_create` + one `pthread_join` (or one cancelled wakeup + one
`pthread_join` if a previous delay is still active). Self-rescheduling
actors (e.g. `examples/led_actor/button_actor.c` re-arms itself every
250 ms) pay this on every tick.

### Zephyr port (`src/port/zephyr/zephyr_ipc_port.c`)

One `struct k_work_delayable` per actor (`p->delayed_work`), plus a
single `struct ipc_msg delayed_msg` holding the pending payload.

```
ipc_port_send_after(a, msg, delay_ms)
  ├─ p->delayed_msg = *msg
  └─ k_work_reschedule(&p->delayed_work, K_MSEC(delay_ms))

delayed_work_fn(work)
  └─ ipc_port_send(a, &p->delayed_msg)
```

`k_work_reschedule` is the atomic primitive that makes "replace
previous" free: scheduling a new delay for the same
`k_work_delayable` cancels the previous one and starts a new timer
in a single kernel call. No per-actor thread, no join, no cancel
flag.

`ipc_port_stop_actor` calls `k_work_cancel_delayable` (synchronous
on the same `k_work_delayable`), then `k_thread_abort` on the
actor's main thread.

Cost characteristics: per-actor `k_work_delayable` (≈48 B on
Zephyr 3.x) plus a stack-aligned timer slot. Memory and timer-slot
pressure scale linearly with actor count.

### Mock port (`tests/mocks/mock_ipc_port.c`)

No thread, no timer, no `k_work`. The mock records the most recent
`ipc_port_send_after` call per actor:

- `send_after_count` — total number of calls
- `last_send_after_delay_ms` — delay of the most recent call
- `pending_send_after_msg` / `pending_send_after_delay_ms` /
  `has_pending_send_after` — the "currently scheduled" slot

"Replace previous" is implemented as a plain struct assignment
(`s->pending_send_after_msg = *msg`) and is what
`test_ipc_send.cpp::SendAfterReplacesPreviouslyPendingMessage`
asserts on: after two `ipc_send_after_raw` calls with different
delays, the recorded pending slot holds the *second* one.

The mock never delivers the message. Tests that need a real
round-trip use `mock_port_set_invoke_handlers(true)` on the
synchronous `ipc_port_send` path instead.

## Per-actor state layout

`struct ipc_port_state` is the platform-specific blob embedded in
`struct ipc_actor` as `ipc_port_state_t` (an opaque
`uintptr_t[IPC_PORT_STATE_WORDS]`). Its concrete contents are
private to each port, but the items relevant to delayed sends are:

| Field                | POSIX port                          | Zephyr port                  |
|----------------------|-------------------------------------|------------------------------|
| Delay worker handle  | `pthread_t delay_thread`            | `struct k_work_delayable`    |
| Delay synchronisation| `pthread_mutex_t delay_lock` + `cond` | (kernel-internal)          |
| Delay message        | `struct ipc_msg delay_msg`          | `struct ipc_msg delayed_msg`  |
| Delay bookkeeping    | `uint32_t delay_ms; bool delay_active; bool delay_cancel;` | (none)            |
| "Last scheduled"     | (none — last is whatever the thread holds) | (none — `k_work` reschedule is atomic) |

The size of this struct must fit in `IPC_PORT_STATE_WORDS ×
sizeof(uintptr_t)`. Both ports have a `_Static_assert` enforcing
this. Default `IPC_PORT_STATE_WORDS = 64` (512 B on 64-bit, 256 B on
32-bit) is sized for the Zephyr port, which is the larger of the
two.

## Lifecycle

`ipc_port_send_after` is callable from any thread context, including
the actor's own handler (a self-rescheduling actor is the canonical
case). It must not be called after `ipc_stop_all` / `ipc_port_stop_actor`
on the same actor — POSIX will still create a thread that then has
no inbox to deliver to, and Zephyr's `k_work_reschedule` after
`k_work_cancel_delayable` is technically allowed but delivers into
a torn-down `k_msgq`.

There is no separate "init" step for the delay primitive; it is
implicitly initialised by `ipc_port_start` (POSIX: `delay_active =
false`; Zephyr: `k_work_init_delayable`). The first `ipc_port_send_after`
on a started actor creates the actual delay thread / `k_work_reschedule`.

## What is *not* here

This is the model as it exists. It is intentionally not:

- A shared timer wheel. The per-actor primitives are independent;
  there is no central scheduler.
- A cancellable timer. There is no handle, no cancel-by-ID, no
  per-`msg_id` tracking. Two `ipc_send_after` calls for the same
  actor with different `MsgType`s collapse to the second; the
  first is dropped unconditionally.
- A "many timers per actor" model. Each actor can have at most one
  pending delayed message at a time, by storage layout.
- A "fire at absolute time" model. Delays are relative-to-now; the
  port computes the absolute deadline internally. There is no way
  to ask "fire at monotonic 12:34:56.789".
- A per-message priority. All delayed messages for the same actor
  are replaced wholesale; there is no ordering by `msg_id`, by
  deadline, or by submission time across different actors.

The framework's `todo.md` flags the move to a single shared timer
wheel as a refactor target. That refactor is not part of this
document; this document describes the model that ships today, so
that the refactor can be designed against a clear baseline.
