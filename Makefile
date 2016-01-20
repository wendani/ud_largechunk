top_builddir := ..
lib_LTLIBRARIES = src/libibverbs.la

AM_CFLAGS := -g -Wall -pg
CC      := gcc
CFLAGS  := $(AM_CFLAGS) -I../include -D_GNU_SOURCE -D_REENTRANT
LD      := gcc
LIBS 	:= $(top_builddir)/$(lib_LTLIBRARIES) -lpthread
LDFLAGS := $(LDFLAGS) $(LIBS)
#-L../src/.libs -libverbs
#../src/.libs/libibverbs.so
#../src/.libs/libibverbs.a

SHELL 	:= /bin/bash
LIBTOOL := $(SHELL) $(top_builddir)/libtool
APPS    := ud devinfo ud_pingpong rc


all: $(APPS)

ud: ud.o pingpong.o
	$(LIBTOOL) --tag=CC --mode=link $(LD) $(AM_CFLAGS) -o $@ $^ $(LDFLAGS)

ud.o: ud.c pingpong.h
	$(CC) $(CFLAGS) -c $<

pingpong.o: pingpong.c pingpong.h
	$(CC) $(CFLAGS) -c $<


rc: rc.o pingpong.o
	$(LIBTOOL) --tag=CC --mode=link $(LD) $(AM_CFLAGS) -o $@ $^ $(LDFLAGS)

rc.o: rc.c pingpong.h
	$(CC) $(CFLAGS) -c $<


devinfo: devinfo.o pingpong.o
	$(LIBTOOL) --tag=CC --mode=link $(LD) $(AM_CFLAGS) -o $@ $^ $(LDFLAGS)

devinfo.o: devinfo.c pingpong.h
	$(CC) $(CFLAGS) -c $<

ud_pingpong: ud_pingpong.o pingpong.o
	$(LIBTOOL) --tag=CC --mode=link $(LD) $(AM_CFLAGS) -o $@ $^ $(LDFLAGS)
#	$(LD) -o $@ $^ $(LDFLAGS)

clean:
	rm -rf $(APPS) ud.o pingpong.o devinfo.o *.out
