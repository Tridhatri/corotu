CC      = clang
CFLAGS  = -Wall -Wextra -O2 -g -Iinclude
LDFLAGS =

SRC     = src/coroutine.c src/coro_ctx_arm64.S

# ── examples ──────────────────────────────────────────────────────────────────
EXAMPLES = examples/coro_basic

all: $(EXAMPLES)

examples/coro_basic: examples/coro_basic.c $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ── debug build (verbose runtime logging to stderr) ───────────────────────────
debug:
	$(CC) $(CFLAGS) -DCORO_DEBUG \
		-o examples/coro_basic_debug examples/coro_basic.c $(SRC)

# ── sanitizer builds ──────────────────────────────────────────────────────────
asan:
	$(CC) $(CFLAGS) -fsanitize=address,undefined \
		-o examples/coro_basic_asan examples/coro_basic.c $(SRC)

tsan:
	$(CC) $(CFLAGS) -fsanitize=thread \
		-o examples/coro_basic_tsan examples/coro_basic.c $(SRC)

# ── cleanup ───────────────────────────────────────────────────────────────────
clean:
	rm -f $(EXAMPLES) \
		examples/coro_basic_debug \
		examples/coro_basic_asan \
		examples/coro_basic_tsan

.PHONY: all debug asan tsan clean
