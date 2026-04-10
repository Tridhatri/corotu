#include <stdio.h>
#include "coroutine.h"

/*
 * Two coroutines taking turns — the simplest possible demonstration.
 *
 * Expected output:
 *   [main] created A and B
 *   [A] step 1
 *   [main] A yielded, resuming B
 *   [B] step 1
 *   [main] B yielded, resuming A
 *   [A] step 2
 *   [main] A yielded, resuming B
 *   [B] step 2
 *   [main] B done
 *   [main] resuming A final time
 *   [A] step 3 — done
 *   [main] all done
 */

void task_a(void *arg) {
    (void)arg;
    printf("[A] step 1\n");
    coro_yield();

    printf("[A] step 2\n");
    coro_yield();

    printf("[A] step 3 — done\n");
    /* returning from fn() calls coro_exit() automatically via trampoline */
}

void task_b(void *arg) {
    (void)arg;
    printf("[B] step 1\n");
    coro_yield();

    printf("[B] step 2\n");
    /* exit explicitly this time */
    coro_exit();

    printf("[B] this line is never reached\n");
}

int main(void) {
    coroutine_t *a = coro_create(task_a, NULL, 0, 0);
    coroutine_t *b = coro_create(task_b, NULL, 0, 0);

    printf("[main] created A and B\n");

    coro_resume(a);
    printf("[main] A yielded, resuming B\n");

    coro_resume(b);
    printf("[main] B yielded, resuming A\n");

    coro_resume(a);
    printf("[main] A yielded, resuming B\n");

    coro_resume(b);
    printf("[main] B done\n");

    printf("[main] resuming A final time\n");
    coro_resume(a);

    printf("[main] all done\n");

    coro_destroy(a);
    coro_destroy(b);
    return 0;
}
