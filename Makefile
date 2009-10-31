

#gcc=/root/buildroot-2009.05/build_i686/staging_dir/usr/i686-linux/bin/gcc

PATH=/root/buildroot-2009.05/build_i686/staging_dir/usr/bin/:/usr/bin/:/bin/:/sbin:/usr/sbin

gcc=i686-linux-uclibc-gcc
CFLAGS=-D_FILE_OFFSET_BITS=64 -g

#PATH="/root/buildroot-2009.05/build_i686/staging_dir/usr/i686-linux-uclibc/bin/:$PATH"

.PHONY: ALL testdriven

ALL: crestfs.static #crestfs 



crestfs.memtest: Makefile crestfs.memtest.o
	gcc $(CFLAGS) -Wall -Werror -o crestfs.memtest crestfs.memtest.o -lfuse -ldl -lpthread -lrt
	
crestfs.memtest.o: Makefile crestfs.c
	gcc -Wall -W -Werror -c $(CFLAGS) -DMEMTEST -o crestfs.memtest.o crestfs.c
	
memtest: crestfs.memtest
	export MALLOC_TRACE=/tmp/memlog
	 ./crestfs.memtest /tmp/doodle /tmp/cachetest -s -d -f


	
crestfs.static: Makefile crestfs.static.o
	$(gcc) $(CFLAGS) -static -Wall -Werror -o crestfs.static crestfs.static.o libfuse.a -lpthread -ldl

crestfs.static.o: crestfs.c Makefile
	$(gcc)  -Wall -W -Werror -idirafter /usr/include/fuse -c $(CFLAGS) -o crestfs.static.o crestfs.c




crestfs: crestfs.o Makefile
	#diet ld -static -o crestfs crestfs.o libfuse.a -lc -lpthread -ldl
	gcc -g -Wall -Werror -o crestfs crestfs.o -lfuse
	#gcc -static -g -Wall -Werror -o crestfs.static crestfs.o -lfuse

crestfs.o: crestfs.c Makefile
	#diet gcc -g -Wall -Werror -c -o crestfs.o crestfs.c
	gcc -g -Wall -Werror -c -o crestfs.o crestfs.c
	




test: crestfs.static
	#umount /tmp/doodle
	mkdir -p /tmp/doodle
	mkdir -p /tmp/cachetest
	strace -o /tmp/straceo ./crestfs.static /tmp/doodle /tmp/cachetest -s -d -f 1> /tmp/1.out 2> /tmp/2.out
	#ulimit ?

debug: crestfs.static
	gdb --args ./crestfs.static /tmp/doodle /tmp/cachetest -s -d -f 

testdriven: crestfs.testframework
	cd /tmp/crestotesto && ~/universix/CREST-fs/crestfs.testframework /desk.nu/testdir HEAD
	cd /tmp/crestotesto && gdb --args ~/universix/CREST-fs/crestfs.testframework /desk.nu/testdir/stupid/dumb/weird GET
	cd ~/universix/CREST-fs

crestfs.testframework: crestfs.c Makefile
	$(gcc) -g -Wall -Werror -idirafter /usr/include/fuse -c -DTESTFRAMEWORK -o crestfs.testframework.o crestfs.c
	$(gcc) -static -g -Wall -Werror -o crestfs.testframework crestfs.testframework.o libfuse.a -lpthread -ldl
