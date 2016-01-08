CC      := gcc
CFLAGS  := -I../include -D_GNU_SOURCE -D_REENTRANT
LD      := gcc
LDFLAGS := ${LDFLAGS} -lpthread -L../src/.libs -libverbs
#../src/.libs/libibverbs.so
#../src/.libs/libibverbs.a
APPS    := ud devinfo ud_pingpong

all: ${APPS}

ud: ud.o pingpong.o
	${LD} -o $@ $^ ${LDFLAGS}

ud.o: ud.c pingpong.h
	${CC} ${CFLAGS} -c $<

pingpong.o: pingpong.c pingpong.h
	${CC} ${CFLAGS} -c $<



devinfo: devinfo.o pingpong.o
	${LD} -o $@ $^ ${LDFLAGS}

ud_pingpong: ud_pingpong.o pingpong.o
	${LD} -o $@ $^ ${LDFLAGS}

clean:
	rm -rf ${APPS} ud.o
