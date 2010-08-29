


#PATH=/http/fs.lightdesktop.com/root/buildroot-2009.05/build_i686/staging_dir/usr/bin/:/usr/bin/:/bin/:/sbin:/usr/sbin
PATH=/root/buildroot-2009.05/build_i686/staging_dir/usr/bin/:/usr/bin/:/bin/:/sbin:/usr/sbin

gcc=i686-linux-uclibc-gcc
CFLAGS=-D_FILE_OFFSET_BITS=64 -g -O2
#only want this for the Static builds, not the regular Dynamic ones.
SILENCE=-DSHUTUP
SILENCE=


.PHONY: ALL testdriven clean

ALL: crestfs

crestfs: crestfs.static crestfs.dynamic
	cp crestfs.static crestfs


crestfs.memtest: Makefile crestfs.memtest.o
	gcc $(CFLAGS) -Wall -Werror -o crestfs.memtest crestfs.memtest.o -lfuse -ldl -lpthread -lrt
	
crestfs.memtest.o: Makefile crestfs.c
	gcc -Wall -W -Werror -c $(CFLAGS) -DMEMTEST -o crestfs.memtest.o crestfs.c
	
memtest: crestfs.memtest
	export MALLOC_TRACE=/tmp/memlog
	 ./crestfs.memtest /tmp/doodle /tmp/cachetest 60 -s -d -f


	
crestfs.static: Makefile crestfs.static.o resource.static.o common.static.o
	$(gcc) $(SILENCE) -g $(CFLAGS) -static -Wall -Werror -o crestfs.static crestfs.static.o resource.static.o common.static.o libfuse.a -lpthread -ldl -lcrypt

crestfs.static.o: crestfs.c Makefile common.h
	$(gcc) $(SILENCE) -g -Wall -W -Werror -idirafter /usr/include/fuse -c $(CFLAGS) -o crestfs.static.o crestfs.c

resource.static.o: resource.c resource.h Makefile common.h
	$(gcc) $(SILENCE) -g -Wall -W -Werror -idirafter /usr/include/fuse -c $(CFLAGS) -o resource.static.o resource.c
	
common.static.o: common.c common.h Makefile
	$(gcc) $(SILENCE) -g -Wall -W -Werror -idirafter /usr/include/fuse -c $(CFLASG) -o common.static.o common.c






crestfs.dynamic: crestfs.o resource.o common.o Makefile
	#diet ld -static -o crestfs crestfs.o libfuse.a -lc -lpthread -ldl
	gcc -g -pg -Wall -Werror -o crestfs.dynamic crestfs.o resource.o common.o -lfuse -lcrypt -lpthread
	#gcc -static -g -Wall -Werror -o crestfs.static crestfs.o -lfuse

crestfs.o: crestfs.c Makefile
	#diet gcc -g -Wall -Werror -c -o crestfs.o crestfs.c
	gcc $(CFLAGS) $(SILENCE) -pg -Wall -W -Werror -c -o crestfs.o crestfs.c
	
resource.o: resource.h resource.c Makefile
	gcc $(CFLAGS) $(SILENCE) -pg -Wall -W -Werror -c -o resource.o resource.c
	
common.o: common.c common.h resource.h Makefile
	gcc $(CFLAGS) $(SILENCE) -pg -Wall -W -Werror -c -o common.o common.c






test: crestfs.static
	#umount /tmp/doodle
	mkdir -p /tmp/doodle
	mkdir -p /tmp/cachetest
	strace -o /tmp/straceo ./crestfs.static /tmp/doodle /tmp/cachetest 60 -s -d -f 1> /tmp/1.out 2> /tmp/2.out
	#ulimit ?

debug: crestfs.static
	gdb --args ./crestfs.static /tmp/doodle /tmp/cachetest 60 -s -d -f 

testdriven: crestfs.testframework
	cd /tmp/crestotesto && ~/universix/CREST-fs/crestfs.testframework /desk.nu/testdir HEAD
	cd /tmp/crestotesto && gdb --args ~/universix/CREST-fs/crestfs.testframework /desk.nu/testdir/stupid/dumb/weird GET
	cd ~/universix/CREST-fs

crestfs.testframework: crestfs.c Makefile
	$(gcc) -g -Wall -Werror -idirafter /usr/include/fuse -c -DTESTFRAMEWORK -o crestfs.testframework.o crestfs.c
	$(gcc) -static -g -Wall -Werror -o crestfs.testframework crestfs.testframework.o libfuse.a -lpthread -ldl
 
clean:
	rm *.o crestfs.static crestfs.dynamic crestfs
