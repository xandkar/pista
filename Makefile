CPPFLAGS := -D_POSIX_C_SOURCE=200809L
CFLAGS   := -std=c99 -Wall -Wextra
LDLIBS   := -lX11 -lm
EXE      := pista

.PHONY: build build_with_compcert demo clean

build: $(EXE)

build_with_compcert:
	ccomp -v -Wall -fstruct-passing $(CPPFLAGS) $(EXE).c $(LDLIBS) -o $(EXE)

demo: $(EXE)
	./demo

clean:
	rm -f $(EXE)
	rm -rf .demo-pipes
