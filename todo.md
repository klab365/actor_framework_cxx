- framework: error-handling contract for CMD and QUERY paths
- framework: explicit cancellation of pending ipc_send_after
- framework: single timer wheel refactor


## framework: run or message based?

- Idea is to this itself in handler function with `while (cancel token)`

## framework: stack size or msg payload size how to make it dynamic for different message types?

## findings

- needed: /** Internal use (QUERY only) */
    void *_wait;

- why struct ipc_actor *_next;?

