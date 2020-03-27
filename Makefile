CPPFLAGS := -D_POSIX_C_SOURCE=200809L
CFLAGS   := -std=c99 -Wall -Wextra
LDLIBS   := -lX11

.PHONY: build clean

build: pista

pista: pista_log.o pista_time.o

pista_time.o: pista_log.o

clean:
	rm -f pista *.o
