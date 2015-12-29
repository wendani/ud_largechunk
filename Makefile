LD      := gcc
LDFLAGS := ${LDFLAGS} -D_REENTRANT -lpthread -libverbs

APPS    := ud_pingpong

all: ${APPS}

ud_pingpong: ud_pingpong.o pingpong.o
	${LD} -o $@ $^ ${LDFLAGS}

ud_pingpong.o: ud_pingpong.c pingpong.h
	${LD} -c $< ${LDFLAGS}

pingpong.o: pingpong.c pingpong.h
	${LD} -c $< ${LDFLAGS}

clean:
	rm -rf ud_pingpng *.o
