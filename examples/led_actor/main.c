/*
 * main.c — POSIX entry point for the led_actor example.
 *
 * Wires three actors (led, app, button) and runs the simulation
 * long enough to observe a few simulated button events.
 */
#define _POSIX_C_SOURCE 200809L
#include <ipc.h>
#include <stdio.h>
#include <time.h>

/* Actor entry points — declared here because they live in the example's
 * own .c files and there is no shared header for them. */

int led_actor_module_init(void);
int app_actor_module_init(void);
int button_actor_module_init(void);
void app_run(void);
void button_actor_kick(void);

static void sleep_ms(int ms)
{
    struct timespec ts = {
        .tv_sec  = ms / 1000,
        .tv_nsec = (long) (ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}

int main(void)
{
    printf("IPC Actor Framework — led_actor + button example (POSIX)\n");

    led_actor_module_init();
    app_actor_module_init();
    button_actor_module_init();
    ipc_start_all_actors();

    /* Static actor definitions are started as one explicit phase.
     * We just need to drive the simulation, then call ipc_stop_all()
     * to signal threads to exit and ipc_run_all() to block in
     * pthread_join so the program doesn't fall out of main() and kill
     * still-running pthreads. */
    sleep_ms(10);

    /* Run a one-shot app scenario (state request + fault publish) */
    app_run();
    sleep_ms(100);

    /* Kick the button actor's tick loop */
    button_actor_kick();

    /* Let it run long enough for click, double-click and hold events. */
    sleep_ms(8000);

    ipc_stop_all(); /* signal all pthreads to exit */
    ipc_run_all(); /* block in pthread_join until done */
    puts("done");
    return 0;
}
