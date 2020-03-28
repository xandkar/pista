CPPFLAGS := -D_POSIX_C_SOURCE=200809L
CFLAGS   := -std=c99 -Wall -Wextra
LDLIBS   := -lX11 -lm

.PHONY: build demo clean

build: pista

demo: pista
	./demo

clean:
	rm -f pista a b c d e f
