It's more of a FAQ than a README, but here's what I've got.

What OS does this run on?
	Currently, only Mac OS X. But it's intended to run on Linux, which is where I expect to use it.
	
What does this require?
	MacFuse is needed to be able to run filesystems in userspace.

Do I need a webdav server to use this crest thing?

	No. Any webserver that does directory listings/index pages in a particular way should work. That way is - lists relative links, the href="" attribute should match the name for the link. Those are considered 'rest resources' that can be listed. E.g.:
	<a href='resource1'>resource1</a> ...
	<a href='res2'>res2</a> ...
	
	That will display resource1 and res2 as usable file system resources.
	
	Other links on the page that are *not* relative, or whose names don't match their href's will *not* be listed.

How do I use it?
	
	./crestfs whereyouwanttomountit whereyouwanttocacheit
	then, you can cd to whereyouwanttomountit/example.com/mysubdir
	A feature that's not tested heavily will actually attempt to cache the contents 'underneath' the mountpoint if you don't specify a cache directory, but this hasn't been tested much yet.
	
Hey, I don't want The Whole Web, I just want *my* server. (server.com/sampledir/myfile1...)
	You'll still have to mount the web as a whole, but you can make a symlink -
	
	./crestfs mymountpoint /tmp/cachedir # to mount the web normally
	
	ln -s mymountpoint/server.com/sampledir/ justmyserver # to have a 'justmyserver' "directory" which points to your server's directory
	
