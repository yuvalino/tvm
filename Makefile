.PHONY: first all clean clean_tvm tvm libtvm unicorn

first: all

all: tvm

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S), Linux)
	LDFLAGS += -pie
endif
ifeq ($(UNAME_S), Darwin)
	LDFLAGS +=
endif

CC ?= gcc
CFLAGS ?=
LDFLAGS ?=
CONFFLAGS ?=

clean:
	@rm -rf unicorn/build || true
	@rm *.a *.o || true
	@rm test_tvm || true
	@rm test_rmem || true

clean_tvm:
	@rm *.o || true

libtvm: unicorn
	$(CC) $(CFLAGS) -g -Wno-deprecated-declarations -I.  -I./unicorn/include -c rmem.c ulinker.c tvm.c main.c
	ar rcs libtvm.a rmem.o ulinker.o tvm.o main.o

tvm: unicorn
	$(CC) $(CFLAGS) -g -rdynamic -I. -I./unicorn/include rmem.c ulinker.c tvm.c main.c -o tvm -L . -lz -lpthread -lutil -lunicorn -lm $(LDFLAGS)

test_tvm:
	$(CC) $(CFLAGS) -g -I. test_tvm.c -o test_tvm -lpthread -lutil $(LDFLAGS)

test_rmem: rmem.c test_rmem.c
	$(CC) $(CFLAGS) -g -rdynamic -I. rmem.c test_rmem.c -o test_rmem -lpthread $(LDFLAGS)

unicorn:
	mkdir -p unicorn/build
	if [ ! -f unicorn/build/Makefile ] ; then cd unicorn/build && cmake .. $(CMAKE_FLAGS) -DCMAKE_BUILD_TYPE=Release -DUNICORN_INTERPRETER=ON -DUNICORN_ARCH="x86;aarch64" -DUNICORN_BUILD_TESTS=OFF; fi
	cd unicorn/build && make
	cp unicorn/build/libunicorn.a ./libunicorn.a

snake: examples/snake.c
	@$(CC) $(CFLAGS) $(LDFLAGS) -g $^ -o $@

stopit: examples/stopit.c
	@$(CC) $(CFLAGS) $(LDFLAGS) -g $^ -o $@

hello_jmpbuf: examples/hello_jmpbuf.c
	@$(CC) $(CFLAGS) $(LDFLAGS) -g $^ -o $@

gdb_backtrace: examples/gdb_backtrace.c
	@$(CC) $(CFLAGS) $(LDFLAGS) -g $^ -o $@

threads: examples/threads.c
	@$(CC) $(CFLAGS) $(LDFLAGS) -g $^ -o $@