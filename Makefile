CPPFLAGS := -D_POSIX_C_SOURCE=200809L
CFLAGS   := -std=c99 -Wall -Wextra
LDLIBS   := -lX11 -lm

.PHONY: build build_with_compcert demo clean

build: pista

build_with_compcert:
	ccomp -v -Wall -fstruct-passing $(CPPFLAGS) pista.c $(LDLIBS) -o pista

demo: pista
	./demo

clean:
	rm -f pista a b c d e f g h
