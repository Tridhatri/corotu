CC      = clang
CFLAGS  = -Wall -Wextra -O2 -g -Iinclude \
          -D_XOPEN_SOURCE=600 -D_DARWIN_C_SOURCE \
          -Wno-deprecated-declarations
LDFLAGS =

SRC     = src/coroutine.c

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
