



crestfs: crestfs.c
	gcc -g -Wall -o crestfs crestfs.c -lfuse
	
test: crestfs
	#test ! -d /tmp/doodle && mkdir /tmp/doodle
	# single user, read-only. NB!
	./crestfs -r -s -d /tmp/doodle

debug: crestfs
	gdb --args ./crestfs -r -s -d /tmp/doodle/
	