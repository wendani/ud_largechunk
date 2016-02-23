AM_CFLAGS := -g -Wall -pg
CC      := gcc
CFLAGS  := $(AM_CFLAGS) -D_GNU_SOURCE -D_REENTRANT
LD      := gcc
LIBS 	:= -libverbs -lpthread
LDFLAGS := $(LDFLAGS) $(LIBS)

APPS    := ud devinfo ud_pingpong rc


all: $(APPS)

ud: ud.o pingpong.o
	$(LD) $(AM_CFLAGS) -o $@ $^ $(LDFLAGS)

ud.o: ud.c pingpong.h
	$(CC) $(CFLAGS) -c $<

pingpong.o: pingpong.c pingpong.h
	$(CC) $(CFLAGS) -c $<


rc: rc.o pingpong.o
	$(LD) $(AM_CFLAGS) -o $@ $^ $(LDFLAGS)

rc.o: rc.c pingpong.h
	$(CC) $(CFLAGS) -c $<


devinfo: devinfo.o pingpong.o
	$(LD) $(AM_CFLAGS) -o $@ $^ $(LDFLAGS)

devinfo.o: devinfo.c pingpong.h
	$(CC) $(CFLAGS) -c $<

ud_pingpong: ud_pingpong.o pingpong.o
	$(LD) $(AM_CFLAGS) -o $@ $^ $(LDFLAGS)
#	$(LD) -o $@ $^ $(LDFLAGS)

clean:
	rm -rf $(APPS) *.o *.out
