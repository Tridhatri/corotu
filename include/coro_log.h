#pragma once

#include <stdio.h>
#include <stdint.h>

/*
 * Debug logging for the coroutine runtime.
 *
 * Off by default. Enable by compiling with -DCORO_DEBUG, e.g.:
 *   make debug
 *
 * All output goes to stderr so it doesn't mix with your program's stdout.
 *
 * Short coroutine ID: lower 16 bits of the pointer address.
 * Enough to tell coroutines apart without printing a full 64-bit address.
 *   e.g. coro at 0x000060000031a020 → id = 0xa020
 */

#define CORO_ID(c)  ((unsigned)((uintptr_t)(c) & 0xFFFF))

#ifdef CORO_DEBUG
#  define CORO_LOG(fmt, ...) \
       fprintf(stderr, "[CORO] " fmt "\n", ##__VA_ARGS__)
#else
#  define CORO_LOG(fmt, ...) (void)0
#endif
