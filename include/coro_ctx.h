#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * Minimal coroutine context for ARM64.
 *
 * On ARM64, the C ABI divides registers into two classes:
 *   - Caller-saved (x0–x18): the calling function saves these before any
 *     call it cares about. By the time we switch, they're already handled.
 *   - Callee-saved (x19–x28, x29, x30, sp): WE must save/restore these
 *     because the caller trusts us not to clobber them.
 *
 * So a context switch only needs to save/restore the callee-saved set.
 *
 * Layout (must stay in sync with the offsets in coro_ctx_arm64.S):
 *
 *   offset   0 : x19
 *   offset   8 : x20
 *   offset  16 : x21
 *   offset  24 : x22
 *   offset  32 : x23
 *   offset  40 : x24
 *   offset  48 : x25
 *   offset  56 : x26
 *   offset  64 : x27
 *   offset  72 : x28
 *   offset  80 : fp  (x29 — frame pointer)
 *   offset  88 : lr  (x30 — becomes the PC when context is restored)
 *   offset  96 : sp
 */
typedef struct {
    uint64_t x19, x20, x21, x22, x23, x24;
    uint64_t x25, x26, x27, x28;
    uint64_t fp;   /* x29 */
    uint64_t lr;   /* x30 — ret in coro_ctx_swap jumps here */
    uint64_t sp;
} coro_ctx_t;

/*
 * Save current registers into *from, load from *to, jump.
 * Returns inside *to's context as if coro_ctx_swap had returned normally.
 */
void coro_ctx_swap(coro_ctx_t *from, coro_ctx_t *to);

/*
 * Prepare *ctx to start executing fn() on the given stack.
 * stack = base address (lowest address) of the stack region.
 * size  = usable size in bytes.
 * fn    must be a no-argument function; use TLS to pass data.
 */
void coro_ctx_init(coro_ctx_t *ctx, void (*fn)(void), void *stack, size_t size);
