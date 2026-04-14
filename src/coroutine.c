#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/mman.h>

#include "coroutine.h"   /* pulls in coro_ctx.h transitively */
#include "coro_log.h"

/* macOS uses MAP_ANON; Linux exposes MAP_ANONYMOUS */
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

/* coro_ctx_init: set up ctx to run fn() on stack[0..size].
 * fn is a no-argument function; data is passed via TLS (t_creating). */
void coro_ctx_init(coro_ctx_t *ctx, void (*fn)(void), void *stack, size_t size) {
    memset(ctx, 0, sizeof(*ctx));
    uint64_t top = (uint64_t)stack + size;
    top &= ~15ULL;          /* ARM64: sp must be 16-byte aligned */
    ctx->sp = top;
    ctx->lr = (uint64_t)fn; /* ret in coro_ctx_swap jumps here on first resume */
    ctx->fp = 0;

    CORO_LOG("CTX_INIT   stack_base=0x%lx  stack_top=0x%lx  trampoline=0x%lx",
             (uintptr_t)stack, (uintptr_t)top, (uintptr_t)fn);
}

/* -------------------------------------------------------------------------
 * Thread-local state
 *
 * Each OS thread has:
 *   t_current   — which coroutine is running right now (NULL = scheduler)
 *   t_sched_ctx — saved context of whoever called coro_resume();
 *                 yield/exit jump back here
 *   t_creating  — coroutine being initialised right now; trampoline reads
 *                 this instead of taking makecontext() arguments, which are
 *                 broken on macOS ARM64 for 64-bit pointer passing
 * ------------------------------------------------------------------------- */
static __thread coroutine_t *t_current   = NULL;
static __thread coro_ctx_t   t_sched_ctx;
static __thread coro_ctx_t   t_exit_ctx;   /* throwaway target for coro_exit */
static __thread coroutine_t *t_creating  = NULL;


/* -------------------------------------------------------------------------
 * Trampoline
 *
 * Zero arguments — avoids the makecontext() int-argument limitation that
 * causes SIGBUS on macOS ARM64 when splitting a 64-bit pointer into two
 * 32-bit halves.  The coroutine pointer is handed over via t_creating.
 * ------------------------------------------------------------------------- */
static void trampoline(void) {
    coroutine_t *c = t_creating;
    t_creating = NULL;          /* clear so it doesn't dangle              */

    CORO_LOG("TRAMPOLINE id=%04x  first run — entering fn()", CORO_ID(c));

    c->fn(c->arg);              /* run the actual coroutine function        */

    CORO_LOG("TRAMPOLINE id=%04x  fn() returned normally", CORO_ID(c));
    coro_exit();                /* fn() returned — mark DONE, back to sched */
}


/* -------------------------------------------------------------------------
 * coro_create
 * ------------------------------------------------------------------------- */
coroutine_t *coro_create(void (*fn)(void *), void *arg,
                          int priority, uint64_t deadline_ns) {
    coroutine_t *c = calloc(1, sizeof(coroutine_t));
    if (!c) return NULL;

    c->fn           = fn;
    c->arg          = arg;
    c->priority     = priority;
    c->deadline_ns  = deadline_ns;
    c->cpu_affinity = -1;
    c->state        = CORO_READY;
    c->stack_size   = CORO_DEFAULT_STACK_SIZE;

    /*
     * Memory layout (low address → high address):
     *
     *   [ guard page | PROT_NONE ][ stack | PROT_READ|WRITE ]
     *   ^                         ^
     *   mem                       c->stack
     *
     * The stack grows downward on x86_64. If it overflows past c->stack
     * into the guard page, the OS raises SIGSEGV instead of silently
     * corrupting adjacent memory.
     */
    size_t total = CORO_GUARD_PAGE_SIZE + c->stack_size;
    void  *mem   = mmap(NULL, total,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        CORO_LOG("CREATE     mmap FAILED — out of memory");
        free(c);
        return NULL;
    }

    /* Make guard page inaccessible */
    if (mprotect(mem, CORO_GUARD_PAGE_SIZE, PROT_NONE) != 0) {
        CORO_LOG("CREATE     mprotect FAILED on guard page");
        munmap(mem, total);
        free(c);
        return NULL;
    }

    c->stack = (char *)mem + CORO_GUARD_PAGE_SIZE;

    CORO_LOG("CREATE     id=%04x  fn=%p  "
             "guard=[%p..%p)  stack=[%p..%p)  "
             "prio=%d  deadline=%s",
             CORO_ID(c), (void *)fn,
             mem,        (char *)mem + CORO_GUARD_PAGE_SIZE,
             c->stack,   (char *)c->stack + c->stack_size,
             priority,
             deadline_ns ? "set" : "none");

    coro_ctx_init(&c->ctx, trampoline, c->stack, c->stack_size);

    return c;
}


/* -------------------------------------------------------------------------
 * coro_resume  — called from scheduler context to run coroutine c
 * ------------------------------------------------------------------------- */
void coro_resume(coroutine_t *c) {
    if (c->state != CORO_READY && c->state != CORO_BLOCKED) {
        CORO_LOG("RESUME     id=%04x  SKIPPED — state is not READY/BLOCKED (state=%d)",
                 CORO_ID(c), c->state);
        return;
    }

    CORO_LOG("RESUME     id=%04x  %s → RUNNING  "
             "(swap: scheduler → coroutine)",
             CORO_ID(c),
             c->state == CORO_READY ? "READY" : "BLOCKED");

    t_current  = c;
    t_creating = c;  /* trampoline reads this on the very first resume only */
    c->state   = CORO_RUNNING;

    /*
     * Save scheduler registers into t_sched_ctx, load c->ctx.
     * On first resume: ret in coro_ctx_swap jumps to trampoline.
     * On later resumes: ret returns into coro_yield where the coroutine
     * previously paused — t_creating is set but never read again.
     */
    coro_ctx_swap(&t_sched_ctx, &c->ctx);

    /* ---- we get back here when the coroutine yields or exits ---- */
    CORO_LOG("RESUME     id=%04x  coroutine returned control to scheduler",
             CORO_ID(c));
    t_current = NULL;
}


/* -------------------------------------------------------------------------
 * coro_yield  — called from inside a coroutine
 * ------------------------------------------------------------------------- */
void coro_yield(void) {
    coroutine_t *c = t_current;
    if (!c) {
        CORO_LOG("YIELD      called outside a coroutine — no-op");
        return;
    }

    CORO_LOG("YIELD      id=%04x  RUNNING → READY  "
             "(swap: coroutine → scheduler)", CORO_ID(c));

    c->state = CORO_READY;

    /*
     * Save our registers into c->ctx, restore t_sched_ctx.
     * Execution in the coroutine resumes from here next time coro_resume(c)
     * is called.
     */
    coro_ctx_swap(&c->ctx, &t_sched_ctx);

    /* ---- we get back here when coro_resume(c) is called again ---- */
    CORO_LOG("YIELD      id=%04x  resumed — back inside coroutine", CORO_ID(c));
}


/* -------------------------------------------------------------------------
 * coro_exit  — called when fn() returns (via trampoline) or explicitly
 * ------------------------------------------------------------------------- */
void coro_exit(void) {
    coroutine_t *c = t_current;
    if (c) {
        CORO_LOG("EXIT       id=%04x  RUNNING → DONE  "
                 "(swap: coroutine → scheduler, one-way)", CORO_ID(c));
        c->state = CORO_DONE;
    }

    /* Save into throwaway ctx, load scheduler — we don't care what
     * gets written to t_exit_ctx since this coroutine is finished. */
    coro_ctx_swap(&t_exit_ctx, &t_sched_ctx);

    /* unreachable */
}


/* -------------------------------------------------------------------------
 * coro_destroy  — free resources of a DONE coroutine
 * ------------------------------------------------------------------------- */
void coro_destroy(coroutine_t *c) {
    if (!c) return;

    CORO_LOG("DESTROY    id=%04x  munmap stack (%zu KB) + free struct",
             CORO_ID(c), c->stack_size / 1024);

    /* stack base = c->stack - CORO_GUARD_PAGE_SIZE */
    void  *mem   = (char *)c->stack - CORO_GUARD_PAGE_SIZE;
    size_t total = CORO_GUARD_PAGE_SIZE + c->stack_size;
    munmap(mem, total);
    free(c);
}


/* -------------------------------------------------------------------------
 * Introspection helpers
 * ------------------------------------------------------------------------- */
coroutine_t *coro_current(void) {
    return t_current;
}

uint64_t clock_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
