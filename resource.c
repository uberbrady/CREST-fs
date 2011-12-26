#include <sys/file.h> //for flock?

/* Given, a 'path', of which the first element is a hostname, and the rest are path elements,
   Check the cache and refresh it (if necessary), returning finally a FILE * (read-only), which is
   the Content part of the HTTP resource. The headers can optionally be loaded into the headers buffer, (which can be 0),
   up to headerlength.

   This function will be aggressive in terms of pre-fetching data it believes could be useful, or that has been useful in the past. It
   may convert a HEAD request into a GET request if it feels it necessary. It may make no requests at all if the caches are recent enough.

   It probably won't know the difference between a file and a directory, nor will it know how to convert path names into cachefile names
   That's the responsibility of the caller. No, wait, in the case where there is not yet an existing cache file or directory, the caller
   won't know either!!! The idea is we want to 'hide' the cache from callers, so they don't need to know how it's organized or works, they
   can just depend upon this function to return a file pointer they can walk through. So it will be cleverer than I said.

   nonexistent resources will return 0

	404 cachefiles - interesting problem. If I ask for a completely stupid path - /a/b/c/d - my algorithm will look for /a/b/c.
			since there's no /a/b, I won't be able to drop a 404 file for /a/b/c. If stat()'s always walk up the dirpath
			that's probably okay, but I am not sure about that.
 */

#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <utime.h>

#include "resource.h"
#include "common.h"
#include "http.h"

#include "worker.h"
//path will look like "/domainname.com/directoryname/file" - with leading slash

/*
	examples:
	
	desk.nu/infinix <--resource is actually a directory.
	
	check caches
*/

typedef enum {
	Nonexistent,	//File is impossible.
	Unknown,	//Don't really know anything
	Fresh,		//Etags _do_ match, this file is fresh; use its cache
} file_plausibility;

#include <time.h>

#define MIN_META_SIZE 8

file_plausibility
is_plausible_file(const char *origpath,int wantdatatoo)
{
	char path[1024];
	#ifdef NOIMPOSSIBLE
	return Unknown;
	#endif
	strlcpy(path,origpath,1024);
	char *slashloc=(char *)path+1; //izziss right?
	
	//first, is file on the upload list? If so, cannot be impossible.
	if(check_put(origpath)) {
		brintf("NOT IMPOSSIBLE: %s (newly-put) cannot be impossible, and is always Fresh\n",origpath);
		return Fresh; //cannot discount a newly-put file as 'impossible'
	}

	//brintf("TESTIG FILE IMPOSSIBILITY FOR: %s\n",slashloc);
	//GET METAFILE STUFF FOR FILE? Yes, godddammit, get it.
	
	int exists=0; //is whether the DATAFILE exists
	char foundit=0;
	int stale=0;
	int definitive=0;
	char childetag[256]="";
	char metafilechildpath[1024]=""; //just to avoid spurious valgrind message
	struct stat dumbstat;
	//stat datafile for origpath (just for existence, all we care about). If exists, exists=1;
	if(lstat(origpath+1,&dumbstat)==0 || !wantdatatoo) { //do 404's drop empty data files? IF not, this will keep refreshing nonexistent files...
		brintf("Stat for origpath+1 succeeded: %s Or we don't want data as well: %d\n",origpath+1,wantdatatoo);
		FILE *metaptr=0;
		exists=1;
		// note - none of this runs if there's no data file. If !exists, then your only two options are Nonexistent or Unknown,
		// and for that we don't care about etag business.
		//we may also need to know if it's a directory or not?
		//check metadatafile for origpath - snag etag in case.
		metafile_for_path(origpath,metafilechildpath,1024,S_ISDIR(dumbstat.st_mode));
		brintf("Our metafile path is: %s\n",metafilechildpath);
		if((metaptr=fopen(metafilechildpath+1,"r"))) {
			brintf("Which we have opened\n");
			char headerbuf[65535]="";
			struct stat parentbuf;
			if(stat(metafilechildpath+1,&parentbuf)==0) {
				brintf("Metafile exists\n");
				if(parentbuf.st_size > MIN_META_SIZE) {
					//possibly big enough enough...
					brintf("Metafile is big enough!\n");
					if((time(0) - parentbuf.st_mtime) <=maxcacheage) {
						//new enough! This is going to end up being 'fresh' (not-stale) and 'existent'
						brintf("Metafile is NEW ENOUGH! WE're DEFINITIVE!\n");
						definitive=1;
						foundit=1;
					} else {
						//it's big enough, but it's not new enough. Let's note the etag for when we
						//decend into the parent.
						brintf("It's big enough, but not new enough :(\n");
						brintf("Time: %ld, Mtime: %ld, MaxCacheAge: %d\n",(long)time(0),(long)parentbuf.st_mtime,maxcacheage);
						int hdrlen=fread(headerbuf,1,65535,metaptr);
						headerbuf[hdrlen+1]='\0';
						fetchheader(headerbuf,"etag",childetag,256);
					}
				} else {
					brintf("Metafile too teensy - size: %ld\n",(long)parentbuf.st_size);
				}
			} else {
				brintf("Metafile doesn't exist. Which is pretty odd considering WE JUST OPENED IT!!!!!!\n");
			}
			fclose(metaptr);
		}
	
	} else {
		brintf("Stat of original file must've failed (statted: '%s')\n",origpath+1);
	}
	//this will itererate successively through parts of the path - 
	//e.g., for /a/b/c/d/e,
	//it should look at:
	// /a/b/c/d/ for e,
	// then /a/b/c/ for d, 
	// etc.
	// when we've hit a definitive directory, then we'll stop (something that's current or specific)
	brintf("'Old' Slashloc: %s\n",slashloc);
	while(!definitive && (slashloc=strrchr(path,'/'))!=0) {
		foundit=0; //haven't found it yet...
		path[slashloc-path]='\0';
		slashloc++;
		//okay 'path' is your directory name, 'slashloc' is the item name in it
		brintf("'New' Slashloc: %s\n",slashloc);
		brintf("'Fixed Path': %s\n",path);
		char metafoldbuf[1024]="";
		FILE *metaptr;
		FILE *dataptr;
		
		metafile_for_path(path,metafoldbuf,1024,1);
		brintf("metafolder: %s, dirname is: %s\n",metafoldbuf+1,path+1);		
		
		if((metaptr=fopen(metafoldbuf+1,"r"))) {
			//ok, we opened the metadata for the directory..
			char headerbuf[65535]="";
			struct stat statbuf;
			int thisfresh=0;
			
			fstat(fileno(metaptr),&statbuf);
			int hdrlen=fread(headerbuf,1,65535,metaptr);
			fclose(metaptr);
			headerbuf[hdrlen+1]='\0';
			//brintf("Buffer we are checking out is: %s",headerbuf);
			char contenttype[128];
			fetchheader(headerbuf,"content-type",contenttype,128);
			if((time(0) - statbuf.st_mtime) <= maxcacheage && statbuf.st_size > MIN_META_SIZE) {
				//okay, the metadata is fresh...
				brintf("Metadata is fresh enough! Marking DEFINITIVE (if it's a manifest)\n");
				if(strncasecmp(contenttype,MANIFESTTYPE,128)==0) {
					definitive=1; //we'll be done! this is the definitive thing
				} else {
					thisfresh=1; //only 'definitive' for nonexistence
				}
			} else {
				brintf("Metadata file is too stale or too tiny to be sure. Age: %ld, Size: %ld\n",(long)(time(0) - statbuf.st_mtime),(long)statbuf.st_size);
			}
			if(thisfresh || strncasecmp(contenttype,MANIFESTTYPE,128)==0) {
				//if we're an HTML dir listing that's fresh, or this is a MANIFEST, iterate the dir
				char datafile[1024];
				datafile_for_path(path,datafile,1024,1);
				if((dataptr=fopen(datafile+1,"r"))) {
					//okay, we managed to open the directory data...
					char filename[1024];
					char parentetag[256]="";
					directory_iterator iter;
					init_directory_iterator(&iter,headerbuf,dataptr);
					while(directory_iterate(&iter,filename,1024,parentetag,128)) {
						brintf("Comparing %s to %s\n",filename,slashloc);
						if(strcmp(filename,slashloc)==0) {
							brintf("Found it!\n");
							foundit=1;
							if(strlen(parentetag)>0) { //if the directory listing has an etag, use it. If the child doesn't have one, then it won't match
								brintf("ETAG CHECK Parent: %s, Child: %s!\n",parentetag,childetag);
								//don't compare empty etags! They don't count!
								if(strncmp(parentetag,childetag,256)!=0) {
									//etag mismatch - file is not fresh
									//we can DEFINITIVELY say this file is stale.
									//(or some set of directories leading up to it is stale)
									stale=1;
									definitive=1;
								}
								//regardless, the metadata for this element now is the 'childetag'
								fetchheader(headerbuf,"etag",childetag,256);
							} else {
								brintf("Could not do etag comparison: parent: %s, child: %s\n",parentetag,childetag);
							}
							break;
						}
					}
					free_directory_iterator(&iter);
					fclose(dataptr);
					if(thisfresh && !foundit) {
						brintf("Marking definitive because I *didn't* find the entry, and thid directory is fresh\n");
						definitive=1;
					}
					brintf("Definitiveness: %d, foundit: %d, thisfresh: %d, exists: %d\n",definitive,foundit,thisfresh,exists);
					if(!definitive) {
						brintf("The file %s doesn't seem impossible by %s\n",origpath,path);
					}
				} else {
					brintf("Can't open directory contents file.\n");
				}
			} else {
				//nothing definitive here - move along
			}
		} else {
			brintf("Could not open metadata file %s\n",metafoldbuf+1);
		}
	}
	brintf("While loop done! Definitive: %d, Foundit: %d stale: %d, exists: %d\n",definitive,foundit,stale,exists);
	if(definitive) {
		//we're definitely done
		if(foundit) {
			if(exists && !stale) {
				return Fresh;
			} else {
				return Unknown; //we found it, but we don't have a cachefile (or it's stale);
			}
		} else {
			//definitively NOT FOUND
			brintf("The file %s is impossible, Definitively.",origpath);
			return Nonexistent;
		}
	}
	//we must've never gotten anything definitive
	brintf("File '%s' appears not to be impossible, move along...\n",origpath);
	return Unknown; //must not be impossible, or we would've returned previously
}

/*
This stat()'ing, read()'ing and utimes()'ing is beating the shit out of the FS.

This needs to somehow be 'in memory'. How can we do that?

Well, when we fetch - actually fetch - a directory, and we have its contents, maybe we don't do anything with it. We don't freshen anything,
but we note that it _is_ fresh? Somehow? (memory cache: "dirpath" -> timestamp?).

What we do *now* to determine whether or not we go to disk or go to 'net' would have to instead become a little more complicated. (yay? Ugh.)

Whereas now, we basically check that the mtime of the metadata cache file is 'recent' enough, (after having checked file impossibility), the new 
algorithm might be:

impossible file detection (now, memory-cache aware!)
check the mtime() on the file. If so, easy-peasy, you're good.
If not - check the mtime()'s on all parents, starting at the root - this is similar to impossible-file detection. In fact, it ought to be built-in. 
It becomes "file_plausibility - returninig 1, 0, -1. 1 - file is fresh and dandy! don't go to internet. 0 - dunno. -1 - file is stale? Nonexistent?"

*/

#include <utime.h>
#include <sys/stat.h>

void directory_freshen(const char *path,char *headers __attribute__((unused)), FILE *dirfile __attribute__((unused)))
{
	char metafile[1024];
	metafile_for_path(path,metafile,1024,1);
	struct utimbuf stamp;
	struct stat oldtime;
	int timeresults;
	stat(metafile+1,&oldtime);
	stamp.actime=oldtime.st_atime; //always leave atimes alone! This says nothing about 'access'!
	stamp.modtime=time(0);
	timeresults=utime(metafile+1,&stamp);
	brintf("Freshening simply by touching directory metafile - results: %d",timeresults);
}

/* 
Old, touch-ey way. blech.
void directory_freshen(const char *path,char *headers,FILE *dirfile)
{
	char contenttype[1024];
	
	fetchheader(headers,"content-type",contenttype,1024);
	
	if(strcasecmp(contenttype,"x-vnd.bespin.corp/directory-manifest")==0) {
		brintf("RECEIVED MANIFEST - MANIPULATING CACHED ITEMS!\n");
		
		directory_iterator iter;
		char etag[128];
		char filename[1024];
		char headerbuf[8192]="";
		char fileetag[1024];
		struct utimbuf stamp;
		init_directory_iterator(&iter,headers,dirfile);
		while(directory_iterate(&iter,filename,1024,etag,128)) {
			if(strlen(etag)==0) {
				brintf("BLANK etag for filename %s, skipping!\n",filename);
				continue;
			}
			char potential_file[1024];
			char metadatafile[1024];
			
			snprintf(potential_file,1024,"%s/%s",path,filename);
			struct stat typeit;
			if(lstat(potential_file+1,&typeit)==0 && S_ISDIR(typeit.st_mode)) {
				metafile_for_path(potential_file,metadatafile,1024,1);
			} else {
				metafile_for_path(potential_file, metadatafile,1024,0);
			}
			FILE *metafile=fopen(metadatafile+1,"r"); //lock your damned self you ingrate
			brintf("I would like to open: %s, please? Results: %p\n",metadatafile+1,metafile);
			if(metafile) {
				int hbytes=fread(headerbuf,1,8191,metafile);
				headerbuf[hbytes+1]='\0';
				fclose(metafile);
				fetchheader(headerbuf,"etag",fileetag,1024);
				struct stat oldtime;
				stat(metadatafile+1,&oldtime);
				stamp.actime=oldtime.st_atime; //always leave atimes alone! This says nothing about 'access'!
				int timeresults=-1;
				if(strcmp(etag,fileetag)==0) {
					stamp.modtime=time(0); //NOW!
					brintf("%s UTIMING NEW!\n",filename);
					//utime(direlem+1,&stamp); //Match! So this file is GOOD as of right now!
					timeresults=utime(metadatafile+1,0);
				} else {
					stamp.modtime=0; //EPOCH! LONG TIME AGO!
					brintf("%s UTIMING OLD!\n",filename);
					timeresults=utime(metadatafile+1,&stamp); //unmatch! This file is BAD as of right now, set its time BACK!
				}
				brintf("Results of stamping were: %d\n",timeresults);
			} else {
				brintf("NO METAFILE I COULD OPEN FOR %s - doing *NOTHING!!!*\n",metadatafile+1);
			}
			//check etag for that file.
			//if it MATCHES, touch the file
			//otherwise, UNTOUCH the file
			//the 'file' may be a directory
		}
		//rewind(dirfile); //doesn't 'free' do this on its own?
		free_directory_iterator(&iter);
	}
}
*/

void rewrite_headers(FILE *headerfile,char *headerfilename __attribute__((unused)),char *received_headers,int headerlength, char *headers)
{ //when NOT run in debug mode, we need the 'attribute-unused' thing on headerfilename
	safe_flock(fileno(headerfile),LOCK_EX,headerfilename+1); //lock upgrade!
	int truncatestatus=ftruncate(fileno(headerfile),0); // we are OVERWRITING THE HEADERS - we got new headers, they're good, we wanna use 'em
	rewind(headerfile); //I think I have to do this or it will re-fill with 0's?!
	//brintf(" Craziness - the number of bytes we should be fputsing is: %d\n",strlen(mybuffer));
	//brintf("truncating did : %d",truncatestatus);
	if(truncatestatus) {
		brintf("truncating did : %d, cuzz: %s\n",truncatestatus,strerror(errno));
	}
	brintf("\n");			
	fputs(received_headers,headerfile);
	if(headers && headerlength>0) {
		//is this common enough to hoist up a little?
		//no, cases which have to fall back to the stale caches will rewrite the headers (no?)
		//wait, yes - if they do do that, they will overwrite the headers on their own!
		strncpy(headers,received_headers,headerlength);
		//if these headers don't get used, they will be rewritten when the stale cache gets loaded up
	}

	//copy the newly-found headers to the headerfile and keep fetching into the file buffer
}

int crestfs_header_check(char *received_headers)
{
	char crestheader[1024]="";
	fetchheader(received_headers,"x-bespin-crest",crestheader,1024);
	if(strlen(crestheader)==0) {
		//Apache does NOT allow modifying 304 headers. That's annoying,
		//but things could be worse. If you got a 304, it means you passed in an Etag, and it matched. So you're not at Starbucks.
		//WARNING - you could cause a...well, sorta "Denial of Updates" attack - by intercepting things and always returning 304.
		brintf("COULD NOT Find Crest-header - you have been STARBUCKSED. Going to cache!\n");
		brintf("Busted headers are: %s\n",received_headers);
		return 0;
	}
	return 1;
}

#define MAXTRIES 5

FILE *BADFILE=((FILE *)-1);

#ifdef NAIVE
//designed for testing 'what if' scenarios like - what if _get_resource were super-duper fast?
FILE *
	_get_resource(const char *path,char *headers,int headerlength, int *isdirectory,const char *preferredverb,char *purpose,char *cachefilemode,int count)
{
	datafile_for_path(path);
	struct stat stbuf;
	lstat(filepath,&stbuf);
	//NB - if headers && headerlength, we need to fill that in with somethign believable.
	switch(stbuf.modething) {
		case directory:
		//this one is hard - we'd try to read_Dir and we'd be iterating through the contents.
		//does it make enough sense to fake it? Or should we do a #ifdef NAIVE version of the directory iterators?
		break;
		
		case file:
		//easy
		break;
		
		case symlink
		//easy
		break;
	}
}
#else


#include <pthread.h>
pthread_key_t socketkey;
int resources_initted=0;

void finalclose(void *tls)
{
	brintf("Final close - tls is: %p\n",tls);
	//close((int)tls);
}

int init_resources(void) {
	int kc=pthread_key_create(&socketkey, finalclose);
	resources_initted=(kc==0);
	printf("pthread_key_create says: %d\n",kc);
	return kc;
}

#include <sys/socket.h>
#include <sys/un.h>

FILE *
_get_resource(const char *path,char *headers,int headerlength, int *isdirectory,const char *preferredverb,char *purpose,char *cachefilemode,int count)
{
	//handle back-end unix domain socket connection via thread-local storage.
	if(!resources_initted) { //is the TLS stuff initted?
		printf("Resources subsystem not initted. Aborting\n");
		exit(42);
	}
	brintf("GET_RESOURCE: %s %s (%s)\n",preferredverb,path,purpose);
	void *spec=pthread_getspecific(socketkey);
	if(spec==0) { //do we already have a connection for this thread?
		brintf("pthread_specific data is empty, opening connection to socket\n");
    int s, len;
    struct sockaddr_un remote;

    if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("thread specific socket()");
        exit(1);
    }

    printf("Trying to connect...\n");

    remote.sun_family = AF_UNIX;
    strcpy(remote.sun_path, "/tmp/crestfs.sock");
    len = strlen(remote.sun_path) + sizeof(remote.sun_family)+1; //+1??!!?
    if (connect(s, (struct sockaddr *)&remote, len) == -1) {
        perror("thread-specific socket connect()");
        exit(1);
    }
    printf("Connected.\n");
		spec=(void*)(long)s;
		pthread_setspecific(socketkey,spec);
	}
	//now we know we have a connection for this thread
	
	//send Socket Request...
	request_t req;
	strlcpy(req.resource,path,1024);
	strlcpy(req.purpose,purpose,128);
	if(strcmp(preferredverb,"HEAD")==0) {
		req.headers_only=1;
	} else {
		req.headers_only=0;
	}
	if(strcmp(cachefilemode,"r")==0) {
		req.read_only=1;
	} else if(strcmp(cachefilemode,"r+")==0) {
		req.read_only=0;
	} else {
		printf("Bizarre mode selected: '%s'\n",cachefilemode);
		exit(15);
	}
	int bytessent=send((int)(long)spec,&req,sizeof(req),0);
	if(bytessent!=sizeof(req)) {
		brintf("Sent %d bytes, wanted %ld\n",bytessent,sizeof(req));
	}
	
	//receive socket response...
	response_t resp;
	struct msghdr msg;
	struct iovec msg_iov;
	msg_iov.iov_base=&resp;
	msg_iov.iov_len=sizeof(resp);
	msg.msg_iov=&msg_iov;
	msg.msg_iovlen=1;
	char ancillary_element_buffer[CMSG_SPACE(sizeof(int))];
  msg.msg_control = ancillary_element_buffer;
  msg.msg_controllen = CMSG_SPACE(sizeof(int));
  
	int bytesrecv=recvmsg((int)(long)spec,&msg,/* MSG_CMSG_CLOEXEC*/0); //I don't know what that would mean
	if(bytesrecv==sizeof(resp)) {
		brintf("GET_RESOURCE RESPONSE FOR %s, Filetype: %d, Moddate: %d, Filesize: %ld\n",path,resp.filetype,resp.moddate,resp.size);
		struct cmsghdr *control_message = NULL;
		int sent_fd=-1;
		
		for(control_message = CMSG_FIRSTHDR(&msg);
       control_message != NULL;
       control_message = CMSG_NXTHDR(&msg, control_message))
		{
   		if( (control_message->cmsg_level == SOL_SOCKET) &&
       	(control_message->cmsg_type == SCM_RIGHTS) )
   		{
    		sent_fd = *((int *) CMSG_DATA(control_message));
   		}
  	}
		if(sent_fd!=-1) {
			brintf("GET_RESOURCE - File descriptor recieved, value is: %d, closing...",sent_fd);
			if(close(sent_fd)==0) {
				brintf("CLOSE OK!\n");
			} else {
				brintf("Close FAIL :%s\n",strerror(errno));
			}
		} else {
			brintf("File descriptor looks bad, value is: %d\n",sent_fd);
		}
	} else {
		brintf("GET_RESOURCE RESPONSE FAIL %s - Received %d bytes, wanted %ld\n",path,bytesrecv,sizeof(resp));
	}




	//legacy method, blocking - 
	//first, check cache. How's that looking?
	struct stat cachestat;
	char webresource[1024];
	char cachefilebase[1024];
	char headerfilename[1024];
	char selectedverb[80];
	FILE *headerfile=0;
	char etag[256]="";
	httpsocket mysocket;
	char *slashlessmetaprepend=0;
	int dirmode=0;
	int dontuseetags=0;
	
	if(count++ >= MAXTRIES) { //side-effect - we've now incremented COUNT
		brintf("%d is greater than count %d\n",count,MAXTRIES);
		return BADFILE;
	}
	
	char *headerbuf=calloc(65535,1);

	headerbuf[0]='\0';
	if(preferredverb==0) {
		preferredverb="GET";
	}
	
	datafile_for_path(path,cachefilebase,1024,0); //we're guessing non-directory to start with
	strncpy(webresource,path,1024); //default web resource corresponding...
	metafile_for_path(path,headerfilename,1024,0);

	strncpy(selectedverb,preferredverb,80);
	//if there is an etag, and the request is a HEAD, upgrade to a GET.
		
	*isdirectory=0; //we can say this because the ONLY way to really 'get' a directory is to GET directory+'/', which requires
			//a reinvoke of this function (witht the appropriate cache directory already in place)
			//if that happens, it will appropriately rewrite isdirectory.
			//if the resource you're looking for turns out to actually *be* a directory, this function will get reinvoked
			//this means we could get into an infinite loop if a you request a resource "foo" that returns a redirect to "foo/"
			//(which implies it's a directory), and then when you try to GET foo/ , you get a 404 - this will loop forever.
	//FIXME - better than this would be to see inthe directory listing somewher eif this entry exist
	//and has a / after it - if so, hint it to being a directory!!!!!!!!
	//that will improve 'raw' ls -lR performance
	char host[1024];
	char pathpart[1024];
	pathparse(path,host,pathpart,1024,1024);
	brintf("Dir test: Host: %s, pathpart: '%s'\n",host,pathpart);
	if((lstat(path+1,&cachestat)==0 && S_ISDIR(cachestat.st_mode)) || strcmp(pathpart,"/")==0) { 
		//note we don't use stat() time for anything!!! we're just checking for directory mode.
		//cache file/directory/link/whatever *does* exist, if it's a directory, push to 'directory mode'
		//that's all we do with this 'stat' value - the bulk of the logic is based on *header* data, not data-data.
		//NB - if the path part of the 'path' is exactly "/", then we *KNOW* this is a directory (the root one for that host.)
		//brintf("Cache entity seems to exist? (or we're the root of some host)\n");
		brintf("ENGAGING DIRECTORY MODE - because the cache entity *IS* a directory!\n");
		// resource we'll need to HEAD or GET will need '/' appended to it
		// cachefile with stuff in it we'll care about will be (path+1)+"/.crestfs_directory_cachenode"
		// with HTTP headers in .crestfs_metadata_rootnode/(path+1)+"/.crestfs_directory_cachenode"
		
		//and FURTHERMORE - the type we're assuming this resource is may no longer be true - 
		//a file could've converted into a directory or some such!
		datafile_for_path(path,cachefilebase,1024,1);
		strncat(webresource,"/",1024);
		metafile_for_path(path,headerfilename,1024,1);
		if(isdirectory) {
			*isdirectory=1;
		}
		dirmode=1; //regardless of *isdirectory, I need to know this if we 404 later.
		// I MIGHT LIKE TO DO THE 'MODE-escalation' and etags stuff here? But I haven't *READ* the headers yet...
		//don't matter. Whether or not I got etags, I want this directory's contents
		if(strcmp(preferredverb,"HEAD")==0) {
			strcpy(selectedverb,"GET");
		}
	} else {
		//if entity does not exist, so we do NOT want to start using etags for things!
		if(lstat(path+1,&cachestat)!=0) {
			dontuseetags=1;
		}
	}
	
/***** This chunk below seems all dedicated twoards figuring out whether or not we use the cachefile (if, of course, it's there) ***********/
	//see if we have a cachefile of some sort. how old is it? Check the st_mtime of the _headerfile_ (the metadata)
	//does this need to be LOCKed??????????  ************************  read LOCK ??? ***********************

	//Here's the new impossible file detection - wait till you had a chance to return cached data first
	//then check if the file is impossible at the LAST MINUTE - *RIGHT* before you'd hit the Internets
	file_plausibility is_plaus=is_plausible_file(path,(strcmp(selectedverb,"GET")==0)); //if you're doing a 'GET' then you want the data too
	switch(is_plaus) {
		case Fresh:
		brintf("File Plausibility for %s says FRESH\n",path);
		break;
		
		case Nonexistent:
		brintf("File Plausibility for %s says NONEXISTENT\n",path);
		break;
		
		case Unknown:
		brintf("File Plausibility for %s says UNKNOWN\n",path);
		break;
		
		default:
		brintf("File Plausibility for %s says something weird! (%d)",path,is_plaus);
	}
	if(is_plaus==Nonexistent) {
		if(headers && headerlength>0) {
			headers[0]='\0';
		}
		free(headerbuf);
		return 0;
	}

	headerfile=fopenr(headerfilename+1,"r+"); //the cachefile may not exist - we may have just had the 'real' file, or the directory
						//if so, that's fine, it will parse out as 'too old (1/1/1970)'
	if(headerfile) {
		struct stat statbuf; //I feel like this stat needs to happen before the fopenr - FIXME
		int s=-1;		//if fopenr() created the file, won't stat() make it seem 'new'? FIXME
		safe_flock(fileno(headerfile),LOCK_SH,headerfilename+1);
		s=stat(headerfilename+1,&statbuf); //assume infallible
		//read up the file and possibly fill in the headers buffer if it's been passed and it's not zero and headerlnegth is not 0
		int hdrbytesread=fread(headerbuf,1,65535,headerfile); //and this is big by one? FIXME
		headerbuf[hdrbytesread+1]='\0'; //I think this is off by one!?!? FIXME
		//brintf("Headers from cache are: %s, statresults is: %d\n",headerbuf,s);
		if(!dontuseetags) {
			//only time we DONT want to use etags is if the original cache entity (the data file)
			//didn't exist at all. an etags request would wipe out additional header info that could be useful
			//e.g. content-length. Wiping out that info is totally fine if you have the actual data - you can just
			//seek to the end of it to see how big it is. But if you only HEAD'ed the file in the past, it sucks.
			fetchheader(headerbuf,"etag",etag,256);
		}

		if(strlen(etag)>0 && strcmp(selectedverb,"HEAD")==0) {
			//we asked to HEAD this file, but we have an etag on file. We'll upgrade to a GET.
			strncpy(selectedverb,"GET",80);
		}
		
		int has_file_been_PUT=check_put(path);
		int is_directory_pending_uploads=dirmode & (statbuf.st_nlink>1);
		brintf("Has the file been PUT? Answer: %d\n",has_file_been_PUT);
		brintf("Is directory pending for uploads? %d\n",is_directory_pending_uploads);
		
		if( is_directory_pending_uploads || is_plaus==Fresh ) { //why do I say "is directory pending uploads?" maybe if it's a deferred dir?
			//our cachefile is recent enough and big enough, or there's been an intervening PUT
			//or file-plausibility says that an intervening directory had etags matches and you're All Good from there
			FILE *tmp=0;
			/* If your status isn't 200 or 304, OR it actually is, but you can succesfully open your cachefile, 
					then you may return succesfully. Or if it's a HEAD reequest (where you don't care about such things)
			*/
			/* okay, there's a bug here and I don't know what to do about ti:
				if you fetch a file, and your cache is old, you will call out a new If-none-match request, and update your 
				metadata cache. Great. If you then ls -al the file BEFORE the cache has expired, it will show up as having
				0 bytes. Why? Because your second request went through this short-circuit routine. This if-clause saw
				that your selected verb was 'HEAD' (you're asking for getattr when you ls -al a file), so it never bothers
				to open a file. When getattr gets its data back, first thing it tries is to look at the headers for a 
				content-length. There ain't one, because the last fetch you did just returned a 304. So next it tries to run
				to the end (using fseek/ftell) of the data-file pointer. Which is zero. Then it just gives up.
				
				one fix is to make crest_getattr always GET instead of HEAD - then it would have data to rely on.
				
				But then that means if you do an ls -al in a big directory, you'll be GET'ing all those files, even
				though you don't want them.
				
				I think the 'real' solution is to do the etags-fetching business (below) up here somewhere instead, so we've
				already 'upgraded' the selectedverb to GET. Yeah, that sounds good?
			*/
			
			brintf("NEW CACHE - COMPLICATED QUESTION: file: %s verb: %s, status: %d headers: '%s'\n",headerfilename+1,selectedverb,fetchstatus(headerbuf),headerbuf);
			
			/* problem - this is *not* using cached data for HEAD's that don't return files - 
			e.g. - I stat() a file without grabbing it. That causes a HEAD.
			I don't ever fetch the file. Later, I stat() it again. That result should be cached
			But it isn't (because there's no file-file behind it).
			*/
			
			int mystatus=fetchstatus(headerbuf);
			if(!S_ISLNK(cachestat.st_mode) && ( // NEVER open a symlink... AND....
				strcmp(selectedverb,"GET")==0 || //if you asked for a GET, or
				(mystatus>=200 && mystatus<=299) ||  //you got a 200 series code...
				mystatus==304 || //or a 304 etag-match message...  
//				MAKE SURE TO HANDLE 302 for symlinks! Otherwise we keep slamming the server to refresh the /bin/ directory!!!
				has_file_been_PUT))   //or file was recently PUT
			{
				//THEN you need a real live file pointer returned to you
				tmp=fopen(cachefilebase+1,cachefilemode);
				//if it was a HEAD request, it's okay if you couldn't get the file
				if(tmp || strcmp(selectedverb,"HEAD")==0) { 
					if(headers && headerlength>0) {
						strncpy(headers,headerbuf,headerlength);
					}
					safe_fclose(headerfile); //RELEASE LOCK!!!!!
					/* this is enormously bad. Do'nt do this.
					if(dirmode) {
						brintf("FRESHENING OFF OF CACHE HIT! path: %s, headers: %s, ptr: %p\n",path,headerbuf,tmp);
						directory_freshen(path,headerbuf,tmp);
					}
					*/
					free(headerbuf);
					return tmp;
				} else {
					brintf("You tried to open the cachefile %s in mode %s and FAILED, so fallthrough.\n",
						cachefilebase+1,cachefilemode);
				}
			} else {
				//you don't need no stinkin' files!
				brintf("No files are needed, allegedly. returning probably a nil for file pointer\n");
				if(headers && headerlength >0) {
					strncpy(headers,headerbuf,headerlength);
				}
				safe_fclose(headerfile); //releasing that same lock...
				free(headerbuf);
				return 0;
			}
			brintf("I guess I couldn't open my cachefile...going through default behavior (bleh)\n");
			dontuseetags=1;
			
		} else {
			brintf("Cache file is too old (or too small); continuing with normal behavior. cachefilename: %s Age: %ld maxcacheage: %d, filesize: %d\n",
				headerfilename+1,(int)time(0)-statbuf.st_mtime,maxcacheage,(int)statbuf.st_size);
		}
	} else {
		//no cachefile exists, we ahve to create one while watching out for races.
		/**** EXCLUSIVE WRITE LOCK! *****/
		//RACE Condition - new file that's never been seen before, when we get *here*, we will have no header cache nor data cache file upon
		//which one might flock() ... but creating said file - when it might be a directory - could be bad. What to do, what to do?
		//fock()
		brintf("Instantiating non-existent metadata cachefile for something.\n");
		if(!(headerfile=fopenr(headerfilename+1,"w+x"))) {
			brintf("RACE CAUGHT ON FILE CREATION for %s! NOt sure what to do about it though... reason: %s. How bout we just fail it?\n",headerfilename+1,strerror(errno));
			free(headerbuf);
			return BADFILE;
			//headerfile=fopenr(headerfilename,"w"); //this means a race was found and prevented(?)
		}
		safe_flock(fileno(headerfile),LOCK_SH,headerfilename+1); //shared lock will immediately be upgraded...
	}
	
	//either there is no cachefile for the headers, or it's too old.
	//the weird thing is, we have to be prepared to return our (possibly-outdated) cache, if we can't get on the internet
	
/***** end all the cache fiddling - if the cache was good, we used it, if not, we are continuing on. Should that all be extracted out? *****/	
	
	char acceptheader[80]="";
	if(dirmode) {
		strcpy(acceptheader,"Accept: x-vnd.bespin.corp/directory-manifest, */*; q=0.5");
	}
	mysocket=http_request(webresource,selectedverb,etag,purpose,acceptheader,0); //extraheaders? (first param)
	
	//brintf("Http request returned socket: %d\n",mysocket);
	
	if(http_valid(mysocket)) {
		char *received_headers=0;
		if(recv_headers(&mysocket,&received_headers)) {
			FILE *datafile=0;

			brintf("Here's the headers, btw: %s\n",received_headers);

			char location[1024]="";
			char tempfile[1024];
			char metafile[1024];
			struct stat st;
			int tmpfd=-1;
			//special case for speed - on a 304, the headers probably haven't changed
			//so don't rewrite them (fast fast!)
			// We also want to skip the anti-starbucksing protocol (underneath)
			// because it's too hard to override apache's conservative view of which headers are 'allowed'
			// in a 304 response. expletive expletive.
			int status=fetchstatus(received_headers);
			switch(status) {
				case 304:
				brintf("FAST 304 Etags METHOD! Not touching much (just utimes)\n");
				utime(headerfilename+1,0);
				if(headers && headerlength>0 ) {
					//So - why do we do this? Why not use the on-file headers instead?
					strlcpy(headers,headerbuf,headerlength);
				}
				if(fetchstatus(headerbuf)==200) {
					brintf("Original file was a file, gonna fopen the datafile\n");
					//this can be problematic if we were a symlink. But if we were, it wouldn't be a 200
			
					datafile=fopen(cachefilebase+1,cachefilemode); //use EXISTING basefile...
					brintf("FOPENED!\n");
				} else {
					datafile=0; //this could've been a symlink, don't try to open it
				}
				if(dirmode) {
					brintf("WE ARE IN DIRMODE - GOING TO TRY TO FRESHEN!\n");
					brintf("(HEADERBUF SEZ: %s)\n",headerbuf);
					directory_freshen(path,headerbuf,datafile);
				}
				brintf("Bout to safe_fclose\n");
				safe_fclose(headerfile); // RELEASE LOCK
				free(received_headers);
				free(headerbuf);
				http_close(&mysocket);
				brintf("Ready to return..\n");
				return datafile;
				break;
			
				case 200:
				//ONE THING WE NEED TO NOTE - if this was a HEAD instead of a GET,
				//we need to be sensitive about the cachefile?
				if(!crestfs_header_check(received_headers)) {
					break;
				}
				rewrite_headers(headerfile,headerfilename,received_headers, headerlength, headers);
				if(strcmp(selectedverb,"HEAD")==0) {
					safe_fclose(headerfile); //RELEASE LOCK!
					free(received_headers);
					struct stat havetounlink;
					if(lstat(cachefilebase+1,&havetounlink)==0) {
						brintf("DUDE TOTALLY SUCKY!!!! Somebody HEAD'ed a resource and we *had* to unlink its data file, because we had one before!.\n");
						unlink(cachefilebase+1);
					}
					http_close(&mysocket);
					free(headerbuf);
					return 0;// NO CONTENTS TO DEAL WITH HERE!
				}
				directoryname(cachefilebase,tempfile,1024,0,0);
				strncat(tempfile,"/.tmpfile.XXXXXX",1024);
				redirmake(tempfile+1); //make sure intervening directories exist, since we can't use fopenr()
				tmpfd=mkstemp(tempfile+1); //dumbass, this creates the file.
				brintf("tempfile will be: %s, has fd: %d\n",tempfile,tmpfd);
				datafile=fdopen(tmpfd,"w+");
				if(!datafile) {
					brintf("Cannot open datafile for some reason?: %s",strerror(errno));
				}
			
				contents_handler(mysocket,datafile);
				if(rename(tempfile+1,cachefilebase+1)) {
					brintf("Could not rename file to %s because: %s\n",cachefilebase+1,strerror(errno));
				}
				chmod(cachefilebase+1,0777);
				datafile=freopen(cachefilebase+1,"r",datafile);
				if(!datafile) {
					brintf("Failed to reopen datafile?!: %s\n",strerror(errno));
				}
				rewind(datafile);
				if(dirmode) {
					brintf("DIRMODE - FRSHEN %s!\n",path);
					directory_freshen(path,received_headers,datafile);
				}
				safe_fclose(headerfile); //RELEASE LOCK
				free(received_headers);
				free(headerbuf);
				http_close(&mysocket);
				return datafile;
				break;

				case 301:
				case 302:
				case 303:
				case 307:
				if(!crestfs_header_check(received_headers)) {
					break;
				}
				//check for directory
				//IF SO: make a cache dir to represent this directory, AND RETRY REQUEST!
				//IF NOT! Treat as symlink(?!). Write headers to headerfile. return no data (or empty file?)
				//NB. Requires a change to readlink()
				//there is NO datafile to work with here, but we don't return 0...how's that gonna work?
				wastebody(mysocket);
				http_close(&mysocket);

				fetchheader(received_headers,"location",location,1024);
			
				if( (strncmp(location,"/",1)==0 && //Host-relative url (starting with '/')
					strncmp(location,pathpart,strlen(pathpart))==0 && 
					strlen(location)==strlen(pathpart)+1 &&
					location[strlen(location)-1]=='/'
				      ) || 
				      (	strncmp(location,"http://",7)==0 &&  //absolute URL
					strncmp(location+7,path,strlen(path))==0 && 
					strlen(location+7)==strlen(path)+1 && 
					location[strlen(location)-1]=='/')) 
				{
					brintf("Location discovered to be: %s, assuming DIRECTORY and rerunning!\n",location);
					//assume this must be a 'directory', but we requested it as if it were a 'file' - rerun!
					redirmake(cachefilebase+1); //make enough of a path to hold what will go here
					unlink(cachefilebase+1); //if it's a file, delete the file; it's not a file anymore
					mkdir(cachefilebase+1,0700); 
					unlink(headerfilename+1); //the headerfile being a file will mess things up too
								//and besides, it's just going to say '301', which isn't helpful
					safe_fclose(headerfile); //RELEASE LOCK
					free(received_headers);
					char modpurpose[1024];
					strlcpy(modpurpose,purpose,1024);
					strlcat(modpurpose,"+directoryrefetch",1024);
					free(headerbuf);
					//TEMP WORKAROUND FIX FOR POSSIBLE BUG ISOLATION STUFF?!
					//delete_keep(mysocket);
					//close(mysocket);
					return _get_resource(path,headers,headerlength,isdirectory,preferredverb,modpurpose,cachefilemode,count);
				} else {
					//otherwise (no slash at end of location path), we must be a plain, boring symlink or some such.
					//yawn.
					brintf("We are thinking this is a symlink. location: %s, pathpart: %s, strlen(loc): %zd, strlen(pathpart): %zd",location,pathpart,strlen(location),strlen(pathpart));
					rewrite_headers(headerfile,headerfilename,received_headers, headerlength, headers);						
					char location[4096]="";
					fetchheader(received_headers,"Location",location,4096);
					brintf("Header location value is: %s\n",location);
					int bufsize=4096;
					char buf[4096];

					//from an http://www.domainname.com/path/stuff/whatever
					//we must get to a local path.
					if(strncmp(location,"http://",7)!=0) {
						//relative symlink. Either relative to _SERVER_ root, or relative to dirname...
						if(location[0]=='/') {
							brintf("No absolute directory-relative symlinks yet, sorry\n");
							safe_fclose(headerfile);
							return BADFILE;
						} else if(strlen(location)>0) {
							strlcpy(buf,rootdir,bufsize); //fs cache 'root'
							char dn[1024];
							directoryname(path,dn,1024,0,0);
							strlcat(buf,dn,bufsize); //may modify argument, hence the copy
							strlcat(buf,"/",bufsize);
							strlcat(buf,location,bufsize);

						} else {
							strlcpy(buf,rootdir,bufsize); //fs cache 'root'
							brintf("Unsupported protocol, or missing 'location' header: %s\n",location);
							safe_fclose(headerfile);
							return BADFILE;
						}
					} else {
						strlcpy(buf,rootdir,bufsize); //fs cache 'root'
						strlcat(buf,"/",bufsize);
						strlcat(buf,location+7,bufsize);
					}

					struct stat st;
					int stres=lstat(path+1,&st);
					char linkbuf[1024]="";
					if(stres==0 && S_ISLNK(st.st_mode)) {
						int linklen=readlink(path+1,linkbuf,1024);
						if(linklen>1023) {
							brintf("Too long of a link ...\n");
							exit(54); //buffer overflow
						}
						linkbuf[linklen+1]='\0';
					}
					if(strncmp(linkbuf,buf,1024)==0) {
						//brintf("Perfect match on symlink! Not doing anything\n");
					} else {
						brintf("Link doesn't exist or is badly made, it is currently: '%s'\n",linkbuf);
						brintf("I will unlink: %s, and point it to: %s\n",path+1,buf);
						int unlinkstat=unlink(path+1); (void)unlinkstat;
						int linkstat=symlink(buf,path+1); (void)linkstat;
						brintf("Going to return link path as: %s (unlink status: %d, link status: %d)\n",buf,unlinkstat,linkstat);
					}
					safe_fclose(headerfile); //release lock
					free(received_headers);
					free(headerbuf);
					return 0; //do we return a filepointer to an empty file or 0? NOT SURE!
				}
				break; //300-series clause (http_close was called up near the top)

				case 404:
				if(!crestfs_header_check(received_headers)) {
					break;
				}
				//Only weird case we got is *IF* we 'accelerated' this into a directory request, and we 404'ed,
				//it may be a regular file now. DELETE THE DIRECTORY, possibly the directoryfilenode thing,
				//and RETRY REQUEST!!!

				//be prepared to drop a 404 cachefile! This will prevent repeated requests for nonexistent entities
				if(dirmode) {
					//we 404'ed (or 403'ed) in dirmode. Shit.
					char headerdirname[1024];
					brintf("Directory mode resulted in 404. Retrying as regular file...\n");
					unlink(cachefilebase+1);
					unlink(headerfilename+1);
					rmdir(path+1);
					strncpy(headerdirname,slashlessmetaprepend,1024);
					strncat(headerdirname,path,1024);
					rmdir(headerdirname);
					brintf("I should be yanking directories %s and %s\n",path+1,headerdirname);
					free(received_headers);
					char modpurpose[1024];
					strlcpy(modpurpose,purpose,1024);
					strlcat(modpurpose,"+plainrefetch",1024);
					wastebody(mysocket);
					http_close(&mysocket);
					free(headerbuf);
					if (headerfile) safe_fclose(headerfile); //need this?! release lock?
					return _get_resource(path,headers,headerlength,isdirectory,preferredverb,modpurpose,cachefilemode,count);
				}
				//file mode - must invalidate parent directory if it said I should be here.
				//if I stat the parent directory metadata and it's recent enough to know better, then invalidate it by
				//stomping its etags and timing it 'old'
				directoryname(cachefilebase,tempfile,1024,0,0);
				metafile_for_path(tempfile,metafile,1024,1);
				stat(metafile+1,&st);
				if(time(0)-st.st_mtime<=maxcacheage) {
					brintf("Parents should've known better. Invalidating");
					invalidate_metadata(metafile+1);
				}
				//NOTE! We are *NOT* 'break'ing after this case! We are deliberately falling into the following case
				//which basically returns a 0 after clearning out everything that's bust.

				case 403: //forbidden
				case 401: //requires authentication
				if(!crestfs_header_check(received_headers)) {
					break;
				}
				rewrite_headers(headerfile,headerfilename,received_headers, headerlength, headers);
				wastebody(mysocket);
				brintf("404/403/401 mode, I *may* be closing the cache header file...\n");
				if(headerfile) {
					int closeresults=safe_fclose(headerfile); //release lock!
					(void)closeresults;
					brintf(" Results: %d\n",closeresults); //Need to release locks on 404's too!
				} else {
					brintf(" No headerfile to close\n");
				}
				free(received_headers);
				http_close(&mysocket);
				free(headerbuf);
				return 0; //nonexistent file, return 0, hold on to cache, no datafile exists.
				break;

				default:
				//we don't want to rewrite the headers in this case.
				brintf("Unknown HTTP status (%d): %s\n",status,received_headers);
				http_destroy(&mysocket);
				//return_keep(mysocket);
				free(headerbuf);
				free(received_headers);
				if(headerfile) {
					safe_fclose(headerfile); //release lock!if you have it...
				}
				if(status>=500 && status<600) {
					brintf("Retrying 500-series error: %d - attempt #%d\n",status,count);
					char modpurpose[1024];
					strlcpy(modpurpose,purpose,1024);
					strlcat(modpurpose,"+retry",1024);
					return _get_resource(path,headers,headerlength,isdirectory,preferredverb,modpurpose,cachefilemode,count);
				}
				return BADFILE;
				break;
			
			} //end big giant switch/case statement that handles different status codes
			/* problems with the anti-starbucksing protocol - need to handle redirects and 404's
				without at least the 404 handling, you can't negatively-cache nonexistent files.
			*/
			brintf("Past block for receiving data, strerror says: %s\n",strerror(errno));
			free(received_headers);
		} else { //end if clause as to whether we got a valid response on recv_headers
			//we have a *VALID* http_socket, but we *don't* have valid headers. Let's toss this connection
			http_destroy(&mysocket);
		}
	}  //end whether or not http conneciton is valid 
	//if things went remotely well and internet-connectedly, you shouldn't end up here.
	//if you did, something screwed up. Try to at least return a cachefile or something.
	//try to return the cached stuff?
	//it could be stale.
	
	// OPEN QUESTION - how does this work for NONEXISTENT files when the internet isn't there?! How can I tell the difference?!
	FILE *staledata=0;
	brintf("BAD INTERNET CONNECTION, returning possibly-stale cache entries for webresource: %s\n",webresource);
	brintf("Headers - which I woudl think wouldn't exist for nonexistent files - are: %s len(%zd)\n",headerbuf,strlen(headerbuf));
	if(headers && headerlength>0) {
		brintf("Headerbuf has been filled, copying it to result headers\n");
		brintf("Current headers are: %s",headerbuf);
		//brintf("Did that crash us or something?\n");
		strlcpy(headers,headerbuf,headerlength);
	}
	free(headerbuf);
	//brintf("Header buffer freed\n");
	brintf("The cache file base we'd _like_ to have open will be: %s\n",cachefilebase+1);
	//careful, doing stat() here and not lstat - if it's a symlink, just follow it.
	if(lstat(cachefilebase+1,&cachestat)==0 && (S_ISREG(cachestat.st_mode))) { //what in the hell was I doing - don't follow symlinks for stale data!
		staledata=fopen(cachefilebase+1,cachefilemode); //could be 0
		if(!staledata) {
			brintf("Crap, coudln't open datafile even though we could stat it. Why?!?! %s\n",strerror(errno));
		} else {
			brintf("Data file opened (%p)\n",staledata);
		}
	} else {
		brintf("Either couldn't stat cache or cache ain't a plain file: %s\n",strerror(errno));
	}
	safe_fclose(headerfile); //RELEASE LOCK!
	brintf("safe_fclose'd headerfile\n");
	brintf("About to return data file: %p\n",staledata);
	http_close(&mysocket);
	return staledata;
}

#endif