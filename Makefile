



crestfs: crestfs.c
	gcc -g -Wall -Werror -o crestfs crestfs.c -lfuse
	
test: crestfs
	umount /tmp/doodle
	mkdir -p /tmp/doodle
	mkdir -p /tmp/cachetest
	./crestfs /tmp/doodle /tmp/cachetest &> /tmp/garbage

debug: crestfs
	gdb --args ./crestfs /tmp/doodle /tmp/cachetest
	