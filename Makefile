



crestfs: crestfs.c
	gcc -g -Wall -Werror -o crestfs crestfs.c -lfuse
	
test: crestfs
	mkdir -p /tmp/doodle
	mkdir -p /tmp/cachetest
	./crestfs /tmp/doodle /tmp/cachetest

debug: crestfs
	gdb --args ./crestfs /tmp/doodle /tmp/cachetest
	