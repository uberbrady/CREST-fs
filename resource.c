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
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <libgen.h> //for 'dirname'
#include <utime.h>

#include "resource.h"
#include "common.h"
//path will look like "/domainname.com/directoryname/file" - with leading slash

/*
	examples:
	
	desk.nu/infinix <--resource is actually a directory.
	
	check caches
*/

#include <time.h>

int
impossible_file(const char *origpath)
{
	char path[1024];
	strlcpy(path,origpath,1024);
	strlcat(path,"/",1024); //just to fool the while loop, I know it's ugly, I'm sorry.
	char *slashloc=(char *)path+1;
	
	//first, is file on the upload list? If so, cannot be impossible.
	if(check_put(origpath)) {
		return 0; //cannot discount a newly-put file as 'impossible'
	}

	//brintf("TESTIG FILE IMPOSSIBILITY FOR: %s\n",slashloc);
	
	while((slashloc=strchr(slashloc,'/'))!=0) {
		char foldbuf[1024];
		char metafoldbuf[1024];
		char dirnamebuf[1024];
		char basenamebuf[1024];
		char *dn=0;
		char *bn=0;
		int slashoffset=slashloc-path;
		FILE *metaptr;
		FILE *dataptr;
		
		strlcpy(foldbuf,path,1024);
		foldbuf[slashoffset]='\0'; //why? I guess the pointer is being advanced PAST the slash?
		//brintf("Testing component: %s, ",foldbuf);
		strlcpy(dirnamebuf,foldbuf,1024);
		dn=dirname(dirnamebuf);
		
		strlcpy(basenamebuf,foldbuf,1024);
		bn=basename(basenamebuf);

		//brintf("Component: %s, basename: %s, dirname: %s\n",foldbuf,bn,dn);
		
		strlcat(dirnamebuf,DIRCACHEFILE,1024);
		strlcpy(metafoldbuf,METAPREPEND,1024);
		strlcat(metafoldbuf,"/",1024);
		strlcat(metafoldbuf,dirnamebuf,1024);
		
		//brintf("metafolder: %s, dirname is: %s\n",metafoldbuf+1,dirnamebuf+1);
		
		if((metaptr=fopen(metafoldbuf+1,"r"))) {
			//ok, we opened the metadata for the directory..
			char headerbuf[65535]="";
			struct stat statbuf;
			
			fstat(fileno(metaptr),&statbuf);
			int hdrlen=fread(headerbuf,1,65535,metaptr);
			fclose(metaptr);
			headerbuf[hdrlen+1]='\0';
			//brintf("Buffer we are checking out is: %s",headerbuf);
			if(time(0) - statbuf.st_mtime <= maxcacheage && statbuf.st_size > 8) {
				//okay, the metadata is fresh...
				//brintf("Metadata is fresh enough!\n");
				if((dataptr=fopen(dirnamebuf+1,"r"))) {
					//okay, we managed to open the directory data...
					char foundit=0;
					char filename[1024];
					directory_iterator iter;
					init_directory_iterator(&iter,headerbuf,dataptr);
					while(directory_iterate(&iter,filename,1024,0,0)) {
						brintf("Comparing %s to %s\n",filename,bn);
						if(strcmp(filename,bn)==0) {
							foundit=1;
							break;
						}
					}
					free_directory_iterator(&iter);
					fclose(dataptr);
					if(foundit==0) {
						brintf("The file %s is impossible",origpath);
						return 1;
					}
					brintf("The file %s doesn't seem impossible by %s",origpath,bn);	
				} else {
					//brintf("Can't open directory contents file.\n");
				}
			} else {
				//brintf("Metadata file is too stale to be sure\n");
			}
		} else {
			//brintf("Could not open metadata file %s\n",metafoldbuf+1);
		}

		slashloc+=1;//iterate past the slash we're on
	}
	//brintf("File '%s' appears not to be impossible, move along...\n",origpath);
	return 0; //must not be impossible, or we would've returned previously
}

/* keepalive support */

struct keepalive {
	char *host;
	int fd;
	int inuse;
	//some sort of time thing?
};


#include <pthread.h>

pthread_mutex_t keep_mut = PTHREAD_MUTEX_INITIALIZER;

#define MAXKEEP 32
//we want this normaly to be higher - say, 32 or so? But I lowered it to look at a bug

struct keepalive keepalives[MAXKEEP];
int curkeep=0;

int
find_keep(char *hostname)
{
	int i;
	pthread_mutex_lock(&keep_mut);
	for(i=0;i<curkeep;i++) {
		if(keepalives[i].host && strcasecmp(hostname,keepalives[i].host)==0) {
			//int newfd=-1;
			int *inuse=&keepalives[i].inuse;
			//brintf("Found a valid keepalive at index: %d\n",i);
			//fcntl(poo)
			if(*inuse==0) {		//This should be an atomic test-and-set
				(*inuse)++;	//But I don't know how to do that ...
				pthread_mutex_unlock(&keep_mut);
				return keepalives[i].fd;
				/*
				newfd=dup(keepalives[i].fd);
				if(newfd==-1) {
					brintf("keepalive dup failed, blanking and closing that entry(%i)\n",i);
					free(keepalives[i].host);
					keepalives[i].host=0;
					close(keepalives[i].fd);
					keepalives[i].fd=0;
					return -1; 
				}
				return newfd; */
			} else {
				brintf("Well, I found a good keepalive candidate, but it's already in use: %d\n",*inuse);
			}
		}
	}
	brintf("NO valid keepalive available for %s, current entry count: %d\n",hostname,curkeep);
	pthread_mutex_unlock(&keep_mut);
	return -1;
}

int
insert_keep(char *hostname,int fd)
{
	//re-use empty slots
	int i;
	brintf("Inserting keepalive for hostname: %s\n",hostname);
	for(i=0;i<curkeep;i++) {
		if(keepalives[i].host==0) {
			brintf("Reusing keepalive slot %d for insert for host %s\n",i,hostname);
			keepalives[i].host=strdup(hostname);
			keepalives[i].fd=fd;
			keepalives[i].inuse=1;
			return 1;
		}
	}
	if(curkeep<MAXKEEP) {
		keepalives[curkeep].host=strdup(hostname);
		keepalives[curkeep].fd=fd;
		keepalives[curkeep].inuse=1;
		curkeep++;
		return 1;
	}
	return 0;
}

int
delete_keep(int fd)
{
	int i;
	for(i=0;i<curkeep;i++) {
		if(keepalives[i].fd == fd) {
			brintf("Found the keepalive to delete for host %s (%d).\n",keepalives[i].host,i);
			free(keepalives[i].host);
			keepalives[i].host=0;
			//close(keepalives[i].fd); //whoever else is using this FD can keep using it, but we don't wanna keep it open anymore
			//whoever's deleting can close it, but I ain't gon do it
			keepalives[i].fd=0;
			keepalives[i].inuse=0;
			return 1;
		}
	}
	return 0;
}

void
return_keep(int fd)
{
	int i;
	pthread_mutex_lock(&keep_mut);
	for(i=0;i<curkeep;i++) {
		if(keepalives[i].fd==fd) {
			keepalives[i].inuse--;
			brintf("Returning a keepalive for host %s, inuse is now: %d\n",keepalives[i].host,keepalives[i].inuse);
			pthread_mutex_unlock(&keep_mut);
			return;
		}
	}
	brintf("Someone tried to return keepalive with FD: %d and I couldn't find it.\n",fd);
	pthread_mutex_unlock(&keep_mut);
}
/* end keepalive support */

#include <resolv.h>

//http_request - extra headers are optional (0 is ok), and so is the body param (0)
//http_request does *NOT* know which headers to add - e.g. content-length (!) so 
//that's your problem, not its. It's got enough to do already.
//extra-headers is full HTTP headers separated by \r\n. It will add the last \r\n for you, so just keep it like:
//	"foo: bar\r\nbaz: bif"
//The 'body', if set, will be transmitted after the headers. Closing the body file pointer is your problem.

int
http_request(const char *fspath,char *verb,char *etag, char *referer,char *extraheaders,FILE *body)
{
	char hostpart[1024]="";
	char pathpart[1024]="";
	pathparse(fspath,hostpart,pathpart,1024,1024);
	int sockfd=-1;
	char *reqstr=0;
	char extraheadersbuf[16384];
	long start=0;
	int keptalive=0;
	
	//brintf("Hostname is: %s, path is: %s\n",hostpart,pathpart);
	
	brintf("http_request: VERB: %s, URL: %s, referer: %s, extraheaders: %s, body pointer: %p\n",verb,fspath,referer,extraheaders,body);
	start=time(0);
	//brintf("Getaddrinfo timing test: BEFORE: %ld\n",start);
	
	sockfd=find_keep(hostpart);
	brintf("finished find keep: %d\n",sockfd);
	if(sockfd==-1) {
		brintf("Sockfd was -1\n");
		int rv;
		struct addrinfo hints, *servinfo=0, *p=0;
		memset(&hints, 0, sizeof hints);
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;

		if ((rv = getaddrinfo(hostpart, "80", &hints, &servinfo)) != 0) {
			brintf("Getaddrinfo timing test: FAIL-AFTER: %ld - couldn't lookup %s because: %s\n",
				time(0)-start,hostpart,gai_strerror(rv));
			return -1;
		}
		brintf("Got getaddrinfo()...GOOD-AFTER: %ld\n",time(0)-start);

		// loop through all the results and connect to the first we can
		for(p = servinfo; p != NULL; p = p->ai_next) {
			if ((sockfd = socket(p->ai_family, p->ai_socktype,
					p->ai_protocol)) == -1) {
				perror("client: socket");
				continue;
			}
			struct timeval to;
			memset(&to,0,sizeof(to));
			to.tv_sec = 3;
			setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
			setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));

			if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
				close(sockfd);
				perror("client: connect");
				continue;
			}

			break;
		}

		if (p == NULL) {
			brintf("client: failed to connect\n");
			close(sockfd);
			return -1;
		}

		brintf("Okay, connectication has occurenced connect-AFTER: %ld\n",time(0)-start);
		insert_keep(hostpart,sockfd);
		freeaddrinfo(servinfo); // all done with this structure
	} else {
		brintf("Using kept-alive connection... keep-AFTER: %ld\n",time(0)-start);
		keptalive=1;
	}

/*     getsockname(sockfd, s, sizeof s);
    brintf("client: connecting to %s\n", s);
*/
	//extraheadersbuf should either be blank, or be terminated with \r\n
	if(etag && strcmp(etag,"")!=0) {
		strcpy(extraheadersbuf,"If-None-Match: ");
		strlcat(extraheadersbuf,etag,16384);
		strlcat(extraheadersbuf,"\r\n",16384);
	} else {
		extraheadersbuf[0]='\0';
	}
	if(extraheaders && strlen(extraheaders)>0) {
		strlcat(extraheadersbuf,extraheaders,16384);
		strlcat(extraheadersbuf,"\r\n",16384);
	}
	brintf("Checking to see if authorization is desired for %s: %s\n",fspath,wants_auth(fspath));
	if(wants_auth(fspath)) {
		char authstring[80]="\0";
		fill_authorization(fspath,authstring,80);
		strlcat(extraheadersbuf,authstring,16384);
		strlcat(extraheadersbuf,"\r\n",16384);
	}
	asprintf(&reqstr,"%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: CREST-fs/1.0\r\nReferer: %s\r\n%s\r\n",verb,pathpart,hostpart,referer,extraheadersbuf);
	brintf("REQUEST: %s\n",reqstr);
	int sendresults=send(sockfd,reqstr,strlen(reqstr),0);
	free(reqstr);
	brintf("from start to SEND-AFTER delay was: %ld\n",time(0)-start);
	int bodysendresults=1;
	if(body) {
		brintf("We are given an entity-body in http_request! Sending it along...\n");
		char bodybuf[1024];
		int bytesread=-1;
		while(!feof(body) && !ferror(body)) {
			bytesread=fread(bodybuf,1,1024,body);
			//brintf("Not at end yet, read this many bytes: %d\n",bytesread);
			if(send(sockfd,bodybuf,bytesread,0)==-1) {
				brintf("Error in sending (got -1)...think that is a big fail-o\n");
				bodysendresults=-1;
				break;
			}
		}
		send(sockfd,"\r\n",2,0); // I can't see why in the spec this is necessary, but it seems to be how it works...
	}
	
	//brintf("Sendresults on socket are: %d, keptalive is: %d\n",sendresults,keptalive);
	char peekbuf[8];
	int peekresults=0;
	do {
		peekresults=recv(sockfd, peekbuf, sizeof(peekbuf), MSG_PEEK);
	} while (peekresults==-1 && errno== EAGAIN );
	if(peekresults>0) {
		//brintf("Buffer peek says: %c%c%c%c\n",peekbuf[0],peekbuf[1],peekbuf[2],peekbuf[3]);
	} else {
		brintf("peek failed :(\n");
	}
	brintf("Recv delay from start - RECV-AFTER was: %ld\n",time(0)-start);
	if(sendresults<=0 || peekresults<=0 || bodysendresults<=0) {
		brintf("Bad socket?! BELETING! sendresults: %d, peekresults: %d bodysendresults: %d(Reason: %s)\n",sendresults,peekresults,bodysendresults,strerror(errno));
		//either we GOT this from the keepalive pool, or we INSERTED it into the pool - either way, pull it out now!!!
		delete_keep(sockfd);
		close(sockfd);
		if(keptalive) {
			//now that we've deleted the keepalive we came from, retry the request
			//hopefully it will take the 'no keepalive found' path...and not infinite loop.
			//which would be bad.
			char modrefer[1024];
			strlcpy(modrefer,referer,1024);
			strlcat(modrefer,"+refetch",1024);
			if(body) {
				rewind(body);
			}
			return http_request(fspath,verb,etag,modrefer,extraheaders,body);
		} else {
			return -1;
		}
	}

	return sockfd;
}

int
straight_handler(int contentlength,int fdesc,FILE *datafile)
{
	int blocklen=65535;
	int bytes=-1;
	int bodybytes=0;
	if(contentlength < 65535) {
		blocklen=contentlength;
	}
	brintf("Blockeln is: %d. File descriptor is: %d\n",blocklen,fdesc);
	if(blocklen>0) {
		char *mybuffer=malloc(blocklen);
		while((bytes = recv(fdesc,mybuffer,blocklen,0))) { //re-use existing buffer
			if(bytes==-1 && (errno==EAGAIN || errno==EINTR)) {
				brintf("Recv reported EAGAIN or EINTR\n");
				continue;
			}
			if(bytes<=0) {
				brintf("Recv returned 0 or -1. FAIL?: cause: %s\n",strerror(errno));
				return 0; //maybe?!?!!?
				break;
			}
			brintf("okay, we finished with teh stupid eagains. What's going on?\n");
			fwrite(mybuffer,bytes,1,datafile);
			bodybytes+=bytes;
			brintf("Read %d bytes, expecting total of %d, curerently at: %d\n",bytes,contentlength,bodybytes);
			if(bodybytes>=contentlength) {
				brintf("Okay, read enough data, bailing out of recv loop. Read: %ld, expecting: %d\n",ftell(datafile),contentlength);
				free(mybuffer);
				mybuffer=0;
				break;
			}
			if(contentlength-bodybytes>65535) {
				blocklen=65535;
			} else {
				blocklen=contentlength-bodybytes;
			}
		}
		if (mybuffer) {
			free(mybuffer);
		}
	}
	return 1;
}

int
chunked_handler(int fdesc,FILE *datafile)
{
	int fd2=dup(fdesc);
	FILE *webs=fdopen(fd2,"r");
	int chunkbytes=-1;
	if(!webs) {
		brintf("I could not open webs from file descriptor :%d\n",fdesc);
	}
	brintf("Beginning Chunk HAndlings!\n");
	while(chunkbytes!=0) {
		char lengthline[16]="";
		char *badchar=0;
		if(fgets(lengthline,16,webs)==0) {
			if(feof(webs))
				brintf("EOF\n");
			if(ferror(webs))
				brintf("FERROR\n");
			fclose(webs);
			return 0; //FAIL
		}
		chunkbytes=strtoul(lengthline,&badchar,16);
		brintf("Starting a chunk, lengthline is: '%s', numerically that's: %d And 'badchar' portion of string is: '%s', badchar strlen is: %d, [%hhd,%hhd]\n",
			lengthline,chunkbytes,badchar,strlen(badchar),badchar[0],badchar[1]);
			
		if(badchar==lengthline) {
			brintf("We handled NOTHING. This shit is TOTALLY MESSED UP\n");
			brintf("chunked - we expected a size, and got something that wouldn't parse at all\n");
			brintf("we should exit, but why don't we continue instead and read another line for fun?\n");
			chunkbytes=-1;
			continue;
		}
		
		int readbytes=0;
		char *buffer=malloc(chunkbytes);
		while(readbytes<chunkbytes) {
			int bytes=fread(buffer,1,chunkbytes,webs);
			int writtenbytes=fwrite(buffer,1,chunkbytes,datafile);
			brintf("Reading some bytes for this chunk, wanted %d, got %d\n",chunkbytes,bytes);
			//decrement some bytes...
			if(bytes!=writtenbytes) {
				brintf("ERROR - wrote fewr bytes than we read!!!!! read: %d, wrote: %d\n",bytes,writtenbytes);
			}
			readbytes+=bytes;
			if(readbytes==chunkbytes) {
				brintf("YAY! end of chunk!");
				char crlf[2];
				int r=fread(crlf,1,2,webs); (void)r;
				if(crlf[0]!='\r' || crlf[1]!='\n') {
					brintf("CRLF fail! We expected CRLF (\\r \\n, 13 10), and got (%hhd %hhd). read said: %d\n",crlf[0],crlf[1],r);
				}
				chunkbytes=-1;
			}
		}
		free(buffer);
	}
	fclose(webs);
	return 1;
}

void
wastebody(char *selectedverb,int mysocket,char *received_headers)
{
	if(strcasecmp(selectedverb,"HEAD")==0) {
		//All responses to the HEAD Method MUST NOT include a message body...
		return;
	}
	int httpstatus=fetchstatus(received_headers);
	if((httpstatus>=100 && httpstatus <200) || httpstatus==204 || httpstatus==304) {
		//All 1xx, 204, and 304 responses MUST NOT include a message-body..
		return;
	} else {
		FILE *waster=fopen("/dev/null","w");
		char transferencoding[1024];
		fetchheader(received_headers,"transfer-encoding",transferencoding,1024);
		if(strncasecmp(transferencoding,"chunked",1024)==0) {
			chunked_handler(mysocket,waster);
		} else {
			char contentlength[1024];
			fetchheader(received_headers,"content-length",contentlength,1024);
			straight_handler(strtol(contentlength,0,10),mysocket,waster);
		}
		fclose(waster);
	}
}

#include <utime.h>
#include <sys/stat.h>

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
			char direlem[2048];
			snprintf(direlem,2048,"%s%s/%s" ,METAPREPEND,path,filename);
			if(direlem[strlen(direlem)-1]=='/') {
				direlem[strlen(direlem)-1]='\0';
				strncat(direlem,DIRCACHEFILE,2048); //already has a slash in front of it...GRRR
			}
			FILE *metafile=fopen(direlem+1,"r"); //lock your damned self you ingrate
			brintf("I would like to open: %s, please? Results: %p\n",direlem+1,metafile);
			if(metafile) {
				int hbytes=fread(headerbuf,1,8192,metafile);
				headerbuf[hbytes+1]='\0';
				fclose(metafile);
				fetchheader(headerbuf,"etag",fileetag,1024);
				struct stat oldtime;
				stat(direlem+1,&oldtime);
				stamp.actime=oldtime.st_atime; //always leave atimes alone! This says nothing about 'access'!
				if(strcmp(etag,fileetag)==0) {
					stamp.modtime=time(0); //NOW!
					brintf("%s UTIMING NEW!\n",filename);
					//utime(direlem+1,&stamp); //Match! So this file is GOOD as of right now!
					utime(direlem+1,0);
				} else {
					stamp.modtime=0; //EPOCH! LONG TIME AGO!
					brintf("%s UTIMING OLD!\n",filename);
					utime(direlem+1,&stamp); //unmatch! This file is BAD as of right now, set its time BACK!
				}
			} else {
				brintf("NO METAFILE I COULD OPEN FOR %s - doing *NOTHING!!!*\n",direlem+1);
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

char *okstring="HTTP/1.1 200 OK\r\nEtag: \"\"\r\n\r\n";

int dont_fclose(FILE *f)
{
	brintf("actually fclosing File: %p\n",f);
	return fclose(f);
}

FILE *
get_resource(const char *path,char *headers,int headerlength, int *isdirectory,const char *preferredverb,char *purpose,char *cachefilemode)
{
	//first, check cache. How's that looking?
	struct stat cachestat;
	char webresource[1024];
	char cachefilebase[1024];
	char headerfilename[1024];
	char selectedverb[80];
	FILE *headerfile=0;
	char etag[256]="";
	int mysocket=-1;
	char *slashlessmetaprepend=0;
	int dirmode=0;
	int dontuseetags=0;
	
	char *headerbuf=calloc(65535,1);

	headerbuf[0]='\0';
	if(preferredverb==0) {
		preferredverb="GET";
	}
	
	strncpy(cachefilebase,path+1,1024); //default non-directory path for a cache file...
	strncpy(webresource,path,1024); //default web resource corresponding...
	slashlessmetaprepend=METAPREPEND;
	slashlessmetaprepend++;
	strncpy(headerfilename,slashlessmetaprepend,1024);
	strncat(headerfilename,path,1024); //need the prepended '/' from path, not cachefilebase

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
		strncat(cachefilebase,DIRCACHEFILE,1024); //need to append the "/.crestfs_directory_cachenode"	
		strncat(webresource,"/",1024);
		strncat(headerfilename,DIRCACHEFILE,1024); //append same to metadata filename?
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
	
	//see if we have a cachefile of some sort. how old is it? Check the st_mtime of the _headerfile_ (the metadata)
	//does this need to be LOCKed??????????  ************************  read LOCK ??? ***********************
	headerfile=fopenr(headerfilename,"r+"); //the cachefile may not exist - we may have just had the 'real' file, or the directory
						//if so, that's fine, it will parse out as 'too old (1/1/1970)'
	if(headerfile) {
		struct stat statbuf;
		int s=-1;
		safe_flock(fileno(headerfile),LOCK_SH,headerfilename);
		s=stat(headerfilename,&statbuf); //assume infallible
		//read up the file and possibly fill in the headers buffer if it's been passed and it's not zero and headerlnegth is not 0
		int hdrbytesread=fread(headerbuf,1,65535,headerfile);
		headerbuf[hdrbytesread+1]='\0';
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
		brintf("Has the file been PUT? Answer: %d\n",has_file_been_PUT);
		
		if((time(0) - statbuf.st_mtime <= maxcacheage && statbuf.st_size > 8) || has_file_been_PUT) {
			//our cachefile is recent enough and big enough, or there's been an intervening PUT
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
			
			/* translation of this 'if' statement:
				Either - 
					you're doing a 'HEAD' - OR
					the status isn't 2xx or 304 (or 302?)
					-OR you can open the file
					
			*/
			if(has_file_been_PUT) {
				//pseudo up the headers for a put file
				strncpy(headerbuf,okstring,65535);
			}
			brintf("COMPLICATED QUESTION: verb: %s, status: %d\n",selectedverb,fetchstatus(headerbuf));
			
			/* problem - this is *not* using cached data for HEAD's that don't return files - 
			e.g. - I stat() a file without grabbing it. That causes a HEAD
			I don't ever fetch the file. Later, I stat() it again. That result should be cached
			But it isn't (because there's no file-file behind it).
			*/
			
			if(strcmp(selectedverb,"GET")==0 || //if you asked for a GET, or
				(fetchstatus(headerbuf)>=200 && fetchstatus(headerbuf)<=299) ||  //you got a 200 series code...
				fetchstatus(headerbuf)==304 || //or a 304 etag-match message...  
				has_file_been_PUT ) //or file was recently PUT
			{
				//THEN you need a real live file pointer returned to you
				tmp=fopen(cachefilebase,cachefilemode);
				//if it was a HEAD request, it's okay if you couldn't get the file
				if(tmp || strcmp(selectedverb,"HEAD")==0) { 
					if(headers && headerlength>0) {
						strncpy(headers,headerbuf,headerlength);
					}
					dont_fclose(headerfile); //RELEASE LOCK!!!!!
					free(headerbuf);
					return tmp;
				} else {
					brintf("You tried to open the cachefile %s in mode %s and FAILED, so fallthrough.\n",
						cachefilebase,cachefilemode);
				}
			} else {
				//you don't need no stinkin' files!
				brintf("No files are needed, allegedly. returning probably a nil for file pointer\n");
				if(headers && headerlength >0) {
					strncpy(headers,headerbuf,headerlength);
				}
				dont_fclose(headerfile); //releasing that same lock...
				free(headerbuf);
				return 0;
			}
			brintf("I guess I couldn't open my cachefile...going through default behavior (bleh)\n");
			dontuseetags=1;
			
		} else {
			brintf("Cache file is too old (or too small); continuing with normal behavior. cachefilename: %s Age: %ld maxcacheage: %d, filesize: %d\n",
				headerfilename,(int)time(0)-statbuf.st_mtime,maxcacheage,(int)statbuf.st_size);
		}
	} else {
		//no cachefile exists, we ahve to create one while watching out for races.
		/**** EXCLUSIVE WRITE LOCK! *****/
		//RACE Condition - new file that's never been seen before, when we get *here*, we will have no header cache nor data cache file upon
		//which one might flock() ... but creating said file - when it might be a directory - could be bad. What to do, what to do?
		//fock()
		brintf("Instantiating non-existent metadata cachefile for something.\n");
		if(!(headerfile=fopenr(headerfilename,"w+x"))) {
			brintf("RACE CAUGHT ON FILE CREATION for %s! NOt sure what to do about it though... reason: %s. How bout we just fail it?\n",headerfilename,strerror(errno));
			free(headerbuf);
			return 0;
			//headerfile=fopenr(headerfilename,"w"); //this means a race was found and prevented(?)
		}
		safe_flock(fileno(headerfile),LOCK_SH,headerfilename); //shared lock will immediately be upgraded...
	}
	
	//either there is no cachefile for the headers, or it's too old.
	//the weird thing is, we have to be prepared to return our (possibly-outdated) cache, if we can't get on the internet
	
	//Here's the new impossible file detection - wait till you had a chance to return cached data first
	//then check if the file is impossible at the LAST MINUTE - *RIGHT* before you'd hit the Internets	
	if(impossible_file(path)) {
		if(headers && headerlength>0) {
			headers[0]='\0';
		}
		free(headerbuf);
		dont_fclose(headerfile);
		return 0;
	}
	char acceptheader[80]="";
	if(dirmode) {
		strcpy(acceptheader,"Accept: x-vnd.bespin.corp/directory-manifest, */*; q=0.5");
	}
	mysocket=http_request(webresource,selectedverb,etag,purpose,acceptheader,0); //extraheaders? (first param)
	
	//brintf("Http request returned socket: %d\n",mysocket);
	
	if(mysocket > 0) {
		char *received_headers=0;
		if(recv_headers(mysocket,&received_headers)) {
			char connection[1024]="";
			char crestheader[1024]="";
			FILE *datafile=0;

			brintf("Here's the headers, btw: %s\n",received_headers);

			fetchheader(received_headers,"Connection",connection,1024);
			if(strcasecmp(connection,"close")==0) {
				brintf("HTTP/1.1 pipeline connection CLOSED, yanking from list.");
				delete_keep(mysocket);
			}

			//special case for speed - on a 304, the headers probably haven't changed
			//so don't rewrite them (fast fast!)
			// We also want to skip the anti-starbucksing protocol (underneath)
			// because it's too hard to override apache's conservative view of which headers are 'allowed'
			// in a 304 response. expletive expletive.
			if(fetchstatus(received_headers)==304) {
				brintf("FAST 304 Etags METHOD! Not touching much (just utimes)\n");
				utime(headerfilename,0);
				if(headers && headerlength>0 ) {
					//So - why do we do this? Why not use the on-file headers instead?
					strlcpy(headers,headerbuf,headerlength);
				}
				if(fetchstatus(headerbuf)==200) {
					brintf("Original file was a file, gonna fopen the datafile\n");
					//this can be problematic if we were a symlink
					
					datafile=fopen(cachefilebase,cachefilemode); //use EXISTING basefile...
					brintf("FOPENED!\n");
				} else {
					datafile=0; //this could've been a symlink, don't try to open it
				}
				if(dirmode) {
					brintf("WE ARE IN DIRMODE - GOING TO TRY TO FRESHEN!\n");
					brintf("(HEADERBUF SEZ: %s)\n",headerbuf);
					directory_freshen(path,headerbuf,datafile);
				}
				brintf("Bout to dont_fclose\n");
				dont_fclose(headerfile); // RELEASE LOCK
				free(received_headers);
				if(strcasecmp(connection,"close")==0)
					close(mysocket);
				free(headerbuf);
				return_keep(mysocket);
				brintf("Ready to return..\n");
				return datafile;
			}

			//brintf("And I think the headers ARE: %s",mybuffer);
			fetchheader(received_headers,"x-bespin-crest",crestheader,1024);
			if(strlen(crestheader)==0) {
				brintf("COULD NOT Find Crest-header - you have been STARBUCKSED. Going to cache!\n");
				brintf("Busted headers are: %s\n",received_headers);
			} else {
				int truncatestatus=0;
				char contentlength[1024]="";
				char location[1024]="";
				char dn[1024];
				char tempfile[1024];
				int readlen=-1;
				int tmpfd=-1;
				char trancode[1024];
				int chunked=0;
				/* problems with the anti-starbucksing protocol - need to handle redirects and 404's
					without at least the 404 handling, you can't negatively-cache nonexistent files.
				*/
				//brintf("We should be fputsing it to: %p\n",headerfile);
				//need to upgrade read-lock to WRITE lock
				safe_flock(fileno(headerfile),LOCK_EX,headerfilename); //lock upgrade!
				truncatestatus=ftruncate(fileno(headerfile),0); // we are OVERWRITING THE HEADERS - we got new headers, they're good, we wanna use 'em
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
				//unlock?!!? ***** UNlocK ******
				//rename() file into place, rewind file pointer thingee,
				//freopen file pointer thingee to be read-only, and then
				//return the file buffer
				//write the remnants of the recv'ed header...
				//brintf("HTTP Header is: %d\n",fetchstatus(received_headers));
				fetchheader(received_headers,"content-length",contentlength,1024);
				fetchheader(received_headers,"transfer-encoding",trancode,1024);
				chunked=(strncasecmp(trancode,"chunked",1024)==0);
				//brintf("Content length is: %s\n",contentlength);
				readlen=atoi(contentlength);

				switch(fetchstatus(received_headers)) {
					case 200:
					//ONE THING WE NEED TO NOTE - if this was a HEAD instead of a GET,
					//we need to be sensitive about the cachefile?
					if(strcmp(selectedverb,"HEAD")==0) {
						dont_fclose(headerfile); //RELEASE LOCK!
						free(received_headers);
						brintf("DUDE TOTALLY SUCKY!!!! Somebody HEAD'ed a resource and we had to unlink its data file.\n");
						unlink(cachefilebase);
						if(strcasecmp(connection,"close")==0)
							close(mysocket);
						return_keep(mysocket);
						free(headerbuf);
						return 0;// NO CONTENTS TO DEAL WITH HERE!
					}
					strncpy(dn,cachefilebase,1024);
					dirname(dn);
					strncpy(tempfile,dn,1024);
					strncat(tempfile,"/.tmpfile.XXXXXX",1024);
					redirmake(tempfile); //make sure intervening directories exist, since we can't use fopenr()
					tmpfd=mkstemp(tempfile); //dumbass, this creates the file.
					//brintf("tempfile will be: %s, it's in dirname: %s\n",tempfile,dn);
					datafile=fdopen(tmpfd,"w+");
					if(!datafile) {
						brintf("Cannot open datafile for some reason?: %s",strerror(errno));
					}
					
					if(chunked) {
						chunked_handler(mysocket,datafile);
					} else {
						straight_handler(readlen,mysocket,datafile);
					}
					
					if(rename(tempfile,cachefilebase)) {
						brintf("Could not rename file to %s because: %s\n",cachefilebase,strerror(errno));
					}
					chmod(cachefilebase,0777);
					datafile=freopen(cachefilebase,"r",datafile);
					if(!datafile) {
						brintf("Failed to reopen datafile?!: %s\n",strerror(errno));
					}
					rewind(datafile);
					if(dirmode) {
						brintf("DIRMODE - FRSHEN!\n");
						directory_freshen(path,received_headers,datafile);
					}
					dont_fclose(headerfile); //RELEASE LOCK
					free(received_headers);
					if(strcasecmp(connection,"close")==0)
						close(mysocket);
					free(headerbuf);
					return_keep(mysocket);
					return datafile;
					break;

					case 301:
					case 302:
					case 303:
					case 307:
					//check for directory
					//IF SO: make a cache dir to represent this directory, AND RETRY REQUEST!
					//IF NOT! Treat as symlink(?!). Write headers to headerfile. return no data (or empty file?)
					//NB. Requires a change to readlink()
					//there is NO datafile to work with here, but we don't return 0...how's that gonna work?
					wastebody(selectedverb,mysocket,received_headers);

					fetchheader(received_headers,"location",location,1024);
					if(strcmp(location,"")!=0 && location[strlen(location)-1]=='/') {
						brintf("Location discovered to be: %s, assuming DIRECTORY and rerunning!\n",location);
						//assume this must be a 'directory', but we requested it as if it were a 'file' - rerun!
						redirmake(cachefilebase); //make enough of a path to hold what will go here
						unlink(cachefilebase); //if it's a file, delete the file; it's not a file anymore
						mkdir(cachefilebase,0700); 
						unlink(headerfilename); //the headerfile being a file will mess things up too
									//and besides, it's just going to say '301', which isn't helpful
						dont_fclose(headerfile);
						free(received_headers);
						if(strcasecmp(connection,"close")==0)
							close(mysocket);
						char modpurpose[1024];
						strlcpy(modpurpose,purpose,1024);
						strlcat(modpurpose,"+directoryrefetch",1024);
						free(headerbuf);
						return_keep(mysocket);
						//TEMP WORKAROUND FIX FOR POSSIBLE BUG ISOLATION STUFF?!
						//delete_keep(mysocket);
						//close(mysocket);
						return get_resource(path,headers,headerlength,isdirectory,preferredverb,modpurpose,cachefilemode);
					}
					//otherwise (no slash at end of location path), we must be a plain, boring symlink or some such.
					//yawn.
					brintf("Not a directory, treating as symlink...\n");
					dont_fclose(headerfile); //release lock
					free(received_headers);
					if(strcasecmp(connection,"close")==0)
						close(mysocket);
					free(headerbuf);
					return_keep(mysocket);
					return 0; //do we return a filepointer to an empty file or 0? NOT SURE!
					break;

					case 404:
					//Only weird case we got is *IF* we 'accelerated' this into a directory request, and we 404'ed,
					//it may be a regular file now. DELETE THE DIRECTORY, possibly the directoryfilenode thing,
					//and RETRY REQUEST!!!

					//be prepared to drop a 404 cachefile! This will prevent repeated requests for nonexistent entities
					if(dirmode) {
						wastebody(selectedverb,mysocket,received_headers);
						//we 404'ed (or 403'ed) in dirmode. Shit.
						char headerdirname[1024];
						brintf("Directory mode resulted in 404. Retrying as regular file...\n");
						unlink(cachefilebase);
						unlink(headerfilename);
						rmdir(path+1);
						strncpy(headerdirname,slashlessmetaprepend,1024);
						strncat(headerdirname,path,1024);
						rmdir(headerdirname);
						brintf("I should be yanking directories %s and %s\n",path+1,headerdirname);
						if(strcasecmp(connection,"close")==0)
							close(mysocket);
						free(received_headers);
						char modpurpose[1024];
						strlcpy(modpurpose,purpose,1024);
						strlcat(modpurpose,"+plainrefetch",1024);
						return_keep(mysocket);
						free(headerbuf);
						if (headerfile) dont_fclose(headerfile); //need this?! release lock?
						return get_resource(path,headers,headerlength,isdirectory,preferredverb,modpurpose,cachefilemode);
					}
					//NOTE! We are *NOT* 'break'ing after this case! We are deliberately falling into the following case
					//which basically returns a 0 after clearning out everything that's bust.

					case 403: //forbidden
					case 401: //requires authentication
					wastebody(selectedverb,mysocket,received_headers);
					brintf("404/403/401 mode, I *may* be closing the cache header file...\n");
					if(headerfile) {
						brintf(" Results: %d\n",dont_fclose(headerfile)); //Need to release locks on 404's too! //BAD
					} else {
						brintf(" No headerfile to close\n");
					}
					if(strcasecmp(connection,"close")==0)
						close(mysocket);
					free(received_headers);
					return_keep(mysocket);
					free(headerbuf);
					return 0; //nonexistent file, return 0, hold on to cache, no datafile exists.
					break;

					default:
					brintf("Unknown HTTP status (%d): %s\n",fetchstatus(received_headers),received_headers);
					close(mysocket);
					//return_keep(mysocket);
					delete_keep(mysocket);
					free(received_headers);
					free(headerbuf);
					if(headerfile) {
						dont_fclose(headerfile); //release lock!if you have it...
					}
					return 0;
					break;
				} //end switch on http status
			} //end 'else' clause about whether we have CREST headers or not
			if(strcasecmp(connection,"close")==0)
				close(mysocket);
		} //end if clause as to whether we got a valid response on recv_headers
		brintf("Past block for receiving data, strerror says: %s\n",strerror(errno));
		free(received_headers);
	} 
	//if things went remotely well and internet-connectedly, you shouldn't end up here.
	//if you did, something screwed up. Try to at least return a cachefile or something.
	//try to return the cached stuff?
	//it could be stale.
	
	// OPEN QUESTION - how does this work for NONEXISTENT files when the internet isn't there?! How can I tell the difference?!
	FILE *staledata=0;
	brintf("BAD INTERNET CONNECTION, returning possibly-stale cache entries for webresource: %s\n",webresource);
	brintf("Headers - which I woudl think wouldn't exist for nonexistent files - are: %s len(%d)\n",headerbuf,strlen(headerbuf));
	if(headers && headerlength>0) {
		brintf("Headerbuf has been filled, copying it to result headers\n");
		brintf("Current headers are: %s",headerbuf);
		//brintf("Did that crash us or something?\n");
		strlcpy(headers,headerbuf,headerlength);
	}
	free(headerbuf);
	//brintf("Header buffer freed\n");
	brintf("The cache file base we'd _like_ to have open will be: %s\n",cachefilebase);
	//careful, doing stat() here and not lstat - if it's a symlink, just follow it.
	if(lstat(cachefilebase,&cachestat)==0 && (S_ISREG(cachestat.st_mode) || S_ISLNK(cachestat.st_mode))) {
		staledata=fopen(cachefilebase,cachefilemode); //could be 0
		if(!staledata) {
			brintf("Crap, coudln't open datafile even though we could stat it. Why?!?! %s\n",strerror(errno));
		} else {
			brintf("Data file opened (%p)\n",staledata);
		}
	} else {
		brintf("Either couldn't stat cache or cache ain't a plain file: %s\n",strerror(errno));
	}
	dont_fclose(headerfile); //RELEASE LOCK!
	brintf("dont_fclose'd headerfile\n");
	brintf("About to return data file: %p\n",staledata);
	return_keep(mysocket);
	return staledata;
}
