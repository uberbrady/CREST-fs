



crestfs: crestfs.c
	gcc -g -Wall -o crestfs crestfs.c -lfuse
	
test: crestfs
	#test ! -d /tmp/doodle && mkdir /tmp/doodle
	./crestfs /tmp/doodle /tmp/cachetest

debug: crestfs
	gdb --args ./crestfs /tmp/doodle /tmp/cachetest
	