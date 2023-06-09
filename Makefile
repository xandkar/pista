CPPFLAGS := -D_POSIX_C_SOURCE=200809L
CFLAGS   := -std=c99 -Wall -Wextra
LDLIBS   := -lX11 -lm
EXE      := pista
PREFIX   := /usr/local

.PHONY: build build_with_compcert demo clean install

build: $(EXE)

build_with_compcert:
	ccomp -v -Wall -fstruct-passing $(CPPFLAGS) $(EXE).c $(LDLIBS) -o $(EXE)

demo: $(EXE)
	./demo

install: $(EXE)
	cp -f $^ $(PREFIX)/bin

clean:
	rm -f $(EXE)
	rm -rf .demo-pipes
