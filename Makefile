#  -O2
CFLAGS=-D_FILE_OFFSET_BITS=64 -g
WORKERCFLAGS := $(CFLAGS) -DBRINTF_FILEOUT=1
# -g
# -DNOIMPOSSIBLE 
# -DSHUTUP
# -pg
#-DNOIMPOSSIBLE is a way to beat the crap out of the client and server. Good for making bugs show up perhaps?
#only want this for the Static builds, not the regular Dynamic ones.
#SILENCE=-DSHUTUP
# Silly. I just changed the Make rules so this shouldn't have to be messed with anymore. I'm leaving it here anyways.

include Makefile.mac

ifdef USE64
	CFLAGS:=$(CFLAGS) -DUSE64
endif


.PHONY: ALL clean

ALL: crestfs

crestfs.memtest: Makefile crestfs.memtest.o
	gcc $(CFLAGS) -Wall -Werror -o crestfs.memtest crestfs.memtest.o -lfuse -ldl -lpthread -lrt

crestfs.memtest.o: Makefile crestfs.c
	gcc -Wall -W -Werror -c $(CFLAGS) -DMEMTEST -o crestfs.memtest.o crestfs.c

memtest: crestfs.memtest
	export MALLOC_TRACE=/tmp/memlog
	 ./crestfs.memtest /tmp/doodle /tmp/cachetest 60 -s -d -f



crestfs.static: Makefile crestfs.static.o resource.static.o common.static.o http.static.o
	$(gcc) -g $(CFLAGS) -DSHUTUP -static -Wall -Werror -o crestfs.static crestfs.static.o resource.static.o common.static.o http.static.o libfuse.a -lpthread -ldl -lcrypt

crestfs.static.o: crestfs.c Makefile common.h http.h
	$(gcc) -DSHUTUP -g -Wall -W -Werror -idirafter $(FUSEINC) -c $(CFLAGS) -o crestfs.static.o crestfs.c

resource.static.o: resource.c resource.h Makefile common.h
	$(gcc) -DSHUTUP -g -Wall -W -Werror -idirafter $(FUSEINC) -c $(CFLAGS) -o resource.static.o resource.c

common.static.o: common.c common.h Makefile http.h
	$(gcc) -DSHUTUP -g -Wall -W -Werror -idirafter $(FUSEINC) -c $(CFLASG) -o common.static.o common.c

http.static.o: http.c http.h Makefile
	$(gcc) -DSHUTUP -g -Wall -W -Werror -idirafter $(FUSEINC) -c $(CFLASG) -o http.static.o http.c






crestfs.dynamic: crestfs.o resource.o common.o Makefile http.o worker.o metacache.o transfers.o async_http.o
	#diet ld -static -o crestfs crestfs.o libfuse.a -lc -lpthread -ldl
	gcc -g -pg -Wall -Werror -o crestfs.dynamic crestfs.o resource.o common.o http.o worker.o metacache.o transfers.o async_http.o -l$(FUSELIB) -lpthread
	#gcc -static -g -Wall -Werror -o crestfs.static crestfs.o -lfuse

crestfs.o: crestfs.c Makefile http.h common.h worker.h
	#diet gcc -g -Wall -Werror -c -o crestfs.o crestfs.c
	gcc $(CFLAGS) $(SILENCE) -Wall -W -Werror -idirafter $(FUSEINC) -c -o crestfs.o crestfs.c

resource.o: resource.h resource.c Makefile common.h resource.h http.h
	gcc $(CFLAGS) $(SILENCE) -Wall -W -Werror -idirafter $(FUSEINC) -c -o resource.o resource.c

common.o: common.c common.h resource.h Makefile http.h
	gcc $(CFLAGS) $(SILENCE) -Wall -W -Werror -idirafter $(FUSEINC) -c -o common.o common.c

http.o: http.c http.h Makefile
	gcc $(CFLAGS) $(SILENCE) -Wall -W -Werror -idirafter $(FUSEINC) -c -o http.o http.c

worker.o: worker.c worker.h transfers.h
	gcc $(WORKERCFLAGS) $(SILENCE) -Wall -W -Werror -c -o worker.o worker.c

metacache.o: worker.h metacache.h metacache.c
	gcc $(WORKERCFLAGS) $(SILENCE) -Wall -W -Werror -c -o metacache.o metacache.c

transfers.o: transfers.c transfers.h async_http.h
	gcc $(WORKERCFLAGS) $(SILENCE) -Wall -W -Werror -c -o transfers.o transfers.c

async_http.o: async_http.c async_http.h
	gcc $(WORKERCFLAGS) $(SILENCE) -Wall -W -Werror -c -o async_http.o async_http.c



plausibilitytest.o: plausibilitytest.c resource.h common.h
	gcc $(CFLAGS) $(SILENCE) -Wall -W -Werror -idirafter $(FUSEINC) -c -o plausibilitytest.o plausibilitytest.c

plausibilitytest: http.o common.o resource.o plausibilitytest.o worker.o
	gcc -g -pg -Wall -W -Werror -o plausibilitytest http.o common.o resource.o plausibilitytest.o -l$(FUSELIB) -lpthread 



getresource.o: getresource.c resource.h
	gcc $(CFLAGS) $(SILENCE) -Wall -W -Werror -idirafter $(FUSEINC) -c -o getresource.o getresource.c

getresource: http.o common.o resource.o getresource.o worker.o metacache.o transfers.o async_http.o
	gcc -g -pg -Wall -W -Werror -o getresource http.o common.o resource.o getresource.o worker.o metacache.o transfers.o async_http.o -l$(FUSELIB) -lpthread 



test: crestfs.static
	#umount /tmp/doodle
	mkdir -p /tmp/doodle
	mkdir -p /tmp/cachetest
	strace -o /tmp/straceo ./crestfs.static /tmp/doodle /tmp/cachetest 60 -s -d -f 1> /tmp/1.out 2> /tmp/2.out
	#ulimit ?

debug: crestfs.static
	gdb --args ./crestfs.static /tmp/doodle /tmp/cachetest 60 -s -d -f 

clean:
	rm *.o crestfs.static crestfs.dynamic crestfs
