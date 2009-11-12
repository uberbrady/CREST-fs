It's more of a FAQ than a README, but here's what I've got.

What OS does this run on?
	I originally wrote it on Mac OS X. But that's probably rotted by now, and I think it may only run on Linux, which is where I use it.
	
What does this require?
	MacFuse is needed to be able to run filesystems in userspace on the Mac, and Fuse and uclibc are needed for the Linux version. You can probably
	hack the Makefile to not need it; I need static builds for my purposes.

Do I need a webdav server to use this crest thing?

	No. The only special thing you'll need is some additional headers (Which need to appear on all HTTP 200 responses, as well as redirects, and 404's, but not on 304's). Any webserver that does directory listings/index pages in a particular way should work. That particular way is: it should list relative links, and the href="" attribute should match the name for the link. Those 'entries' are considered 'rest resources' that can be listed. E.g.:
	<a href='resource1'>resource1</a> ...
	<a href='res2'>res2</a> ...
	
	That will display resource1 and res2 as usable file system resources.
	
	Other links on the page that are *not* relative, or whose names don't match their href's will *not* be listed. The default directory listing module in Apache displays links in this fashion.
	
	You *really* should have etags enabled on your server, or you will get horrible caching performance. Having it on directories will be nice as well

How do I use it?
	
	./crestfs whereyouwanttomountit whereyouwanttocacheit
	then, you can cd to whereyouwanttomountit/example.com/mysubdir
	A feature that's now in heavy use will actually attempt to cache the contents 'underneath' the mountpoint by specifying the same directory twice.
	
	I usually run it with options of:
	  -s -d -f 
	and redirect stdout and stderr to files for debugging. There are thread-safe locks in place, but this isn't tested.
	
Hey, I don't want The Whole Web, I just want *my* server. (server.com/sampledir/myfile1...)
	You'll still have to mount the web as a whole, but you can make a symlink -
	
	./crestfs mymountpoint /tmp/cachedir # to mount the web normally
	
	ln -s mymountpoint/server.com/sampledir/ justmyserver # to have a 'justmyserver' "directory" which points to your server's directory
	
What features does it have?
	(NEW) User-Agent header is now "CREST-fs/0.7", a big bump in version since some significant changes affect how the system works now.
	(NEW) Anti-HotSpot pollution - since you're treating REST resources from the Web as actual files, when you go into a Starbucks and use their Wifi, every page you ask for gives you a redirect to a login page. Those redirects would normally be parsed as symlinks and could completely destroy your filesystem. Now, all filesystem-level 200's, 404's and 30x-series redirects require a special header to be set - "X-Bespin-Crest:" - it can be set to anything for now (the production server uses the word 'yes'), but will eventually be some kind of digital signature. This unfortunately makes using a stock Apache install far more difficult than it should be.
	(NEW) HTTP/1.1 pipelining support is built and has improved performance dramatically, especially since everything usually runs single-threaded anyways (-s).
	
	etags on directory listings will be respected (so later directory listings don't require another full GET)

	The cache file system looks almost exactly like the live filesystem, so you can boot off of it.
	
	Metadata for files (HTTP Headers section) is stored under .crestfs_metadata_rootnode/{PATH}
	
	a stat() on a file typically is implemented as a HEAD on the appropriate HTTP resource
	
	Directories are aggressively fetched when stat()'ed, as are files that have etags with them already.
	
	Files that are 'impossible' can be culled from being stat()'ed if they don't exist in one of their parents' directory entries.
	
	etags are stored and later fetches of files use them to avoid re-downloading unchanged files.
	
	HTTP redirects (that aren't your standard directory-redirects, which just add a '/' to the Location) are treated as symlinks
		
	Symlinks and directories are now cached normally, and the negative-directory entry caching system has been removed
	
	The bulk of the complexity is the get_resource() routine, which does the bulk of the work of managing caches and so on. The rest of the code
	is mostly just a shim to get it to work with FUSE.
	
	There's a test harness setup that allows testing get_resource() without the rest of FUSE, using simple command-line parameters.
	
	nonexistent files will create negative-caching entries, reducing server traffic
	
BUGS
	The Anti-HotSpot Pollution system requires a lot of poking around at Apache to get working.
	Unexpected conditions in the cache directories will crash the filesystem.
	Multi-threaded support is totally untested
	The Keep-alive array and structure is totally un-thread-safe, and is not prepared to handle multiple sockets to the same host.
	Directory listings can't be larger than 1MB
	Some of the static buffers are probably too small for practical use
	The Makefile is horrible and needs to probably be set up with 'autoconf' or something like it
	The max-cache age(age at which you want to check with the server) should be runtime or mount-time configurable
	Magic numbers are used more than they should be, and this is Naughty.
	Symlinks to directories probably don't work.
