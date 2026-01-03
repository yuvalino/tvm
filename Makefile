.PHONY: first all clean mksh forkless dropbear dummy toybox libtvm unicorn

first: all

all: forkless

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
	@make -C dropbear/ clean || true
	@make -C toybox/ clean || true
	@make -C mini-vim/ clean || true
	@rm dropbear/Makefile || true
	@rm mksh/Rebuild.sh || true
	@rm -rf unicorn/build || true
	@rm *.a *.o || true
	@rm test_tvm || true
	@rm test_rmem || true
	@rm forkless || true

libtvm: dropbear mksh toybox unicorn minivim
	$(CC) $(CFLAGS) -g -I.  -I./unicorn/include -c rmem.c ulinker.c tvm.c main.c
	ar rcs libtvm.a rmem.o ulinker.o tvm.o main.o

forkless: dropbear mksh toybox unicorn minivim
	$(CC) $(CFLAGS) -g -rdynamic -I. -I./unicorn/include rmem.c ulinker.c tvm.c main.c -o forkless -L . -ldropbear -lmksh -ltoybox -lz -lpthread -lutil -lunicorn -lm -lminivim $(LDFLAGS)

test_tvm: unicorn
	$(CC) $(CFLAGS) -g -rdynamic -I. -I./unicorn/include rmem.c ulinker.c tvm.c test_tvm.c -o test_tvm -L . -lpthread -lunicorn -lm $(LDFLAGS)

test_rmem: rmem.c test_rmem.c
	$(CC) $(CFLAGS) -g -rdynamic -I. rmem.c test_rmem.c -o test_rmem -lpthread $(LDFLAGS)

mksh:
	if [ -f mksh/Rebuild.sh ]; then cd mksh && sh ./Rebuild.sh; else cd mksh && CFLAGS="$(CFLAGS) -g -I$(realpath .) -DMKSH_FORKLESS=1" sh ./Build.sh; fi
	find mksh/ -type f -name '*.o' | xargs ar rcs libmksh.a

dropbear: dummy
	if [ ! -f dropbear/Makefile ]; then cd dropbear && ./configure CFLAGS="$(CFLAGS) -g -I$(realpath .)" ac_cv_type_socklen_t=yes ac_cv_type_uid_t=yes $(CONFFLAGS); fi
	make -C dropbear/ PROGRAMS="dropbear scp" MULTI=1 LDFLAGS="$(LDFLAGS) /tmp/dummy.o -o /tmp/dummy.out && echo"
	find dropbear/ -type f -name '*.o' | xargs ar rcs libdropbear.a

toybox:
	CFLAGS="$(CFLAGS) -g -I$(realpath .) -DTOYBOX_FORKLESS=1" V=1 make -C toybox
	find toybox/ -type f -name '*.o' | xargs ar rcs libtoybox.a

unicorn:
	mkdir -p unicorn/build
	if [ ! -f unicorn/build/Makefile ] ; then cd unicorn/build && cmake .. $(CMAKE_FLAGS) -DCMAKE_BUILD_TYPE=Release -DUNICORN_INTERPRETER=ON -DUNICORN_ARCH="x86;aarch64" -DUNICORN_BUILD_TESTS=OFF; fi
	cd unicorn/build && make
	cp unicorn/build/libunicorn.a ./libunicorn.a

minivim:
	CFLAGS="$(CFLAGS) -g -I$(realpath .) -DMINIVIM_FORKLESS=1" make -C mini-vim libminivim.a
	cp mini-vim/libminivim.a .

dummy:
	@$(CC) $(CFLAGS) dummy/dummy.c -c -o /tmp/dummy.o

snake: examples/snake.c
	@$(CC) $(CFLAGS) $(LDFLAGS) -g $^ -o $@

stopit: examples/stopit.c
	@$(CC) $(CFLAGS) $(LDFLAGS) -g $^ -o $@

hello_jmpbuf: examples/hello_jmpbuf.c
	@$(CC) $(CFLAGS) $(LDFLAGS) -g $^ -o $@