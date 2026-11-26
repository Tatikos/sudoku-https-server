CC       ?= cc
CFLAGS   += -std=c11 -Wall -Wextra -Wpedantic -O2 -D_DEFAULT_SOURCE -I. -pthread
LDLIBS   := -lssl -lcrypto -pthread

MODULES  := config sudoku cache http logger worker
OBJS     := $(MODULES:%=%.o)
TEST_OBJ := tests/test_main.o config.o sudoku.o cache.o http.o
HEADERS  := $(wildcard *.h)

.PHONY: all test dist clean

all: server

server: server.o $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tests/test_main: $(TEST_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

test: tests/test_main
	./tests/test_main

tests/loadgen: tests/loadgen.c
	$(CC) $(CFLAGS) -pthread -o $@ $< $(LDLIBS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

dist:
	chmod +x run.sh
	tar czf game-engine.tar.gz --transform 's,^,game-engine/,' \
		*.c *.h Makefile run.sh config.txt README.txt ARCHITECTURE.pdf \
		tests/test_main.c tests/loadtest.py tests/loadgen.c

clean:
	rm -f server *.o tests/*.o tests/test_main tests/loadgen game-engine.tar.gz
