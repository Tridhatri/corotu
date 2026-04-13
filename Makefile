CC      = clang
CFLAGS  = -Wall -Wextra -O2 -g -Iinclude
LDFLAGS =

SRC     = src/coroutine.c src/coro_ctx_arm64.S

# ── examples ──────────────────────────────────────────────────────────────────
EXAMPLES = examples/coro_basic

all: $(EXAMPLES)

examples/coro_basic: examples/coro_basic.c $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ── sanitizer builds ──────────────────────────────────────────────────────────
asan:
	$(CC) $(CFLAGS) -fsanitize=address,undefined \
		-o examples/coro_basic_asan examples/coro_basic.c $(SRC)

tsan:
	$(CC) $(CFLAGS) -fsanitize=thread \
		-o examples/coro_basic_tsan examples/coro_basic.c $(SRC)

# ── cleanup ───────────────────────────────────────────────────────────────────
clean:
	rm -f $(EXAMPLES) examples/coro_basic_asan examples/coro_basic_tsan

.PHONY: all asan tsan clean
