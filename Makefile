CPPFLAGS := -D_POSIX_C_SOURCE=200809L
CFLAGS   := -std=c99 -Wall -Wextra
LDLIBS   := -lX11

.PHONY: build clean

build: khatus

khatus: khlib_log.o khlib_time.o

khlib_time.o: khlib_log.o

clean:
	rm -f khatus *.o
