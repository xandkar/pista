CPPFLAGS := -D_POSIX_C_SOURCE=200809L
CFLAGS   := -std=c99 -Wall -Wextra
LDLIBS   := -lX11 -lm

.PHONY: build clean

build: pista

clean:
	rm -f pista
