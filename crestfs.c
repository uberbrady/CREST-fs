/* For more information, see http://www.macdevcenter.com/pub/a/mac/2007/03/06/macfuse-new-frontiers-in-file-systems.html. */ 

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <dirent.h>

#include <regex.h>

#define FUSE_USE_VERSION  26
#include <fuse.h>
#include <sys/time.h>
//#include <xlocale.h>

#include <strings.h>


/*************************/
// Important #define's which describe all behavior as a whole
//#define MAXCACHEAGE		3600*2
#define MAXCACHEAGE		60

//global variable (another one is down there but it's use is jsut there.
char rootdir[1024]="";


//we may want this to be configurable in future - a command line option perhaps
/*************************/

//BSD-isms we have to replicate (puke, puke)

#if defined(__linux__) && !defined(__dietlibc__) && !defined(__UCLIBC__)
int 
strlcat(char *dest, const char * src, int size)
{
	strncat(dest,src,size-1);
	return strlen(dest);
}

int
strlcpy(char *dest,const char *src, int size)
{
	int initialsrc=strlen(src);
	int bytes=0;
	if(initialsrc>size-1) {
		bytes=size-1;
	} else {
		bytes=initialsrc;
	}
	//brintf("Chosen byteS: %d, initial src: %d\n",bytes,initialsrc);
	strncpy(dest,src,bytes);
	dest[bytes]='\0';
	
	return initialsrc;
}

#include <stdarg.h>
int
asprintf(char **ret, const char *format, ...)
{
	va_list v;
	*ret=malloc(32767);
	
	va_start(v,format);
	return vsnprintf(*ret,32767,format,v);
}

char * strptime(char *,char *,struct tm *);
#else
#include <stdarg.h>
#endif

/* utility functions for debugging, path parsing, fetching, etc. */

void brintf(char *format,...) __attribute__ ((format (printf, 1,2)));

void brintf(char *format,...)
{
	va_list whatever;
	va_start(whatever,format);
	vprintf(format,whatever);
	fflush(NULL);
}

void
pathparse(const char *path,char *hostname,char *pathonly,int hostlen,int pathlen)
{
	char *tokenatedstring;
	char *tmphostname;

	if(path[0]=='/') {
		path+=1;
	}

	tokenatedstring=strdup(path);
	tmphostname=strsep(&tokenatedstring,"/");

	strncpy(hostname,tmphostname,hostlen);
	strcpy(pathonly,"/"); //pathonly always starts with slash...
	if(tokenatedstring) {
		strlcat(pathonly,tokenatedstring,pathlen);
	}
	free(tmphostname);
}

/* keepalive support */

struct keepalive {
	char *host;
	int fd;
	//some sort of time thing?
};

#define MAXKEEP 3
//we want this normaly to be higher - say, 32 or so? But I lowered it to look at a bug

struct keepalive keepalives[MAXKEEP];
int curkeep=0;

int
find_keep(char *hostname)
{
	int i;
	for(i=0;i<curkeep;i++) {
		if(keepalives[i].host && strcasecmp(hostname,keepalives[i].host)==0) {
			int newfd=-1;
			brintf("Found a valid keepalive at index: %d\n",i);
			//fcntl(poo)
			newfd=dup(keepalives[i].fd);
			if(newfd==-1) {
				brintf("keepalive dup failed, blanking and closing that entry(%i)\n",i);
				free(keepalives[i].host);
				keepalives[i].host=0;
				close(keepalives[i].fd);
				keepalives[i].fd=0;
				return -1; 
			}
			return newfd;
		}
	}
	brintf("NO valid keepalive available\n");
	return -1;
}

int
insert_keep(char *hostname,int fd)
{
	//re-use empty slots
	int i;
	for(i=0;i<curkeep;i++) {
		if(keepalives[i].host==0) {
			brintf("Reusing keepalive slot %d for insert for host %s\n",i,hostname);
			keepalives[i].host=strdup(hostname);
			keepalives[i].fd=dup(fd);
			return 1;
		}
	}
	if(curkeep<MAXKEEP) {
		keepalives[curkeep].host=strdup(hostname);
		keepalives[curkeep].fd=dup(fd);
		curkeep++;
		return 1;
	}
	return 0;
}

int
delete_keep(char *hostname)
{
	int i;
	for(i=0;i<curkeep;i++) {
		if(keepalives[i].host && strcasecmp(hostname,keepalives[i].host)==0) {
			brintf("Found the keepalive to delete for host %s (%d).\n",hostname,i);
			free(keepalives[i].host);
			keepalives[i].host=0;
			close(keepalives[i].fd); //whoever else is using this FD can keep using it, but we don't wanna keep it open anymore
			keepalives[i].fd=0;
			return 1;
		}
	}
	return 0;
}
/* end keepalive support */

#define FETCHBLOCK 65535
#define HOSTLEN 64
#define PATHLEN 1024

#include <resolv.h>

int
http_request(char *fspath,char *verb,char *etag)
{
	char hostpart[1024];
	char pathpart[1024];
	pathparse(fspath,hostpart,pathpart,1024,1024);
	int sockfd;
	char *reqstr=0;
	char etagheader[1024];
	
	brintf("Hostname is: %s, path is: %s\n",hostpart,pathpart);
	
	brintf("Getaddrinfo timing test: BEFORE: %ld",time(0));
	
	sockfd=find_keep(hostpart);
	if(sockfd==-1) {
		int rv;
		struct addrinfo hints, *servinfo, *p;
		memset(&hints, 0, sizeof hints);
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;

		if ((rv = getaddrinfo(hostpart, "80", &hints, &servinfo)) != 0) {
			brintf("Getaddrinfo timing test: FAIL-AFTER: %ld",time(0));
			brintf("I failed to getaddrinfo: %s\n", gai_strerror(rv));
			return 0;
		}
		brintf("Got getaddrinfo()...GOOD-AFTER: %ld\n",time(0));

		// loop through all the results and connect to the first we can
		for(p = servinfo; p != NULL; p = p->ai_next) {
			if ((sockfd = socket(p->ai_family, p->ai_socktype,
					p->ai_protocol)) == -1) {
				perror("client: socket");
				continue;
			}
			struct timeval to;
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
			fprintf(stderr, "client: failed to connect\n");
			close(sockfd);
			return 0;
		}

		brintf("Okay, connectication has occurenced\n");
		insert_keep(hostpart,sockfd);
		freeaddrinfo(servinfo); // all done with this structure
	} else {
		brintf("Using kept-alive connection...\n");
	}

/*     getsockname(sockfd, s, sizeof s);
    brintf("client: connecting to %s\n", s);
*/
	if(strcmp(etag,"")!=0) {
		strcpy(etagheader,"If-None-Match: ");
		strlcat(etagheader,etag,1024);
		strlcat(etagheader,"\r\n",1024);
	} else {
		etagheader[0]='\0';
	}
	asprintf(&reqstr,"%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: CREST-fs/0.3\r\n%s\r\n",verb,pathpart,hostpart,etagheader);
	brintf("REQUEST: %s\n",reqstr);
	send(sockfd,reqstr,strlen(reqstr),0);
	free(reqstr);

	return sockfd;
	
}

void
reanswer(char *string,regmatch_t *re,char *buffer,int length)
{
	memset(buffer,0,length);
	//brintf("You should be copying...? %s\n",string+re->rm_so);
	strlcpy(buffer,string+re->rm_so,length);
	buffer[re->rm_eo - re->rm_so]='\0';
	//brintf("Your answer is: %s\n",buffer);
}

//retrieve the value of an HTTP header
//be warned the headers may not be a nul-terminated string
//so don't do stupid stuff like strlen(headers) because it 
//may not work

char mingie[65535]="";

void manglefinder(char *string,int lineno)
{
	if(strcmp(mingie,"")==0) {
		strlcpy(mingie,string,65535);
	} else {
		if(strcmp(string,mingie)!=0) {
			brintf("Mingie: %s\n",mingie);
			brintf("Mangled to: %s\n",string);
			brintf("Happend at line: %d\n",lineno);
			exit(-1);
		}
	}
}


//#define MANGLETOK manglefinder(headers,__LINE__);
#define MANGLETOK

void
fetchheader(char *headers,char *name,char *results,int length)
{
	char *cursor=headers;
	int blanklinecount=0;
	
	mingie[0]='\0';
	MANGLETOK;
	while(cursor[0]!='\0') {
		char *lineending=strstr(cursor,"\r\n"); //this is dangerous - cursor may not be null-terminated. But we're guaranteed to find \r\n
												//somewhere, or else it's not valid HTTP protocol contents.
												//we may need to change this out for \n in case we have invalid servers
												//out there somewhere...
		if(lineending==0) {
			brintf("Couldn't find a CRLF pair, bailing out of fetchheader()!\n");
			break;
		}
		//brintf("SEARCHING FROM: CHARACTERS: %c %c %c %c %c\n",cursor[0],cursor[1],cursor[2],cursor[3],cursor[4]);
		//brintf("Line ENDING IS: %p, %s\n",lineending,lineending);
		if(lineending==cursor) {
			//brintf("LINEENDING IS CURSOR! I bet this doesn't happen\n");
			break;
			blanklinecount++;
		} else {
			char line[1024];
			char *colon=0;
			int linelen=0;

			blanklinecount=0; //consecutive blank line counter must be reset
			MANGLETOK
			strlcpy(line,cursor,1024);
			MANGLETOK
			line[lineending-cursor]='\0';
			//brintf("my line is: %s\n",line);
			linelen=strlen(line);
			colon=strchr(line,':');
			if(colon) {
				int keylen=colon-line;
				//brintf("Colon onwards is: %s keylen: %d\n",colon,keylen);
				MANGLETOK
				if(strncasecmp(name,line,keylen)==0) {
					int nullen=0;
					while(colon[1]==' ') {
						//brintf("advanced pointer once\n");
						colon++;
					}
					//brintf("colon+1: %s, keylen %d, linelen: %d, linelen-keylen: %d, length!!! %d\n",colon+1,keylen,linelen,linelen-keylen,length);
					strncpy(results,colon+1,linelen-keylen);
					nullen=linelen-keylen+1;
					results[nullen>length-1 ? length-1 : nullen]='\0';
					//brintf("Well, results look like the will be: %s\n",results);
					MANGLETOK
					return;
				}
				//brintf("line is: %s\n",line);
			} else {
				//brintf("No colon found!!!!!!!!!\n");
				MANGLETOK
			}
		}
		//brintf("BLANK LINE COUNT IS: %d\n",blanklinecount);
		if(blanklinecount==2) {
			break;
		}
		MANGLETOK
		
		cursor=lineending+2;//advance past the carriage-return,newline
	}
	MANGLETOK
	//must not have found it or would've left already!
	results[0]='\0';
	MANGLETOK
}

int
fetchstatus(char *headers)
{
	char status[4];
	// HTTP/1.0 200 BLah
	
	if(strlen(headers)<strlen("HTTP/1.0 xyz")) {
		return 404;
	}
	strncpy(status,headers+9,3);
	status[3]='\0';
	return atoi(status);
}

int
parsedate(char *datestring)
{
	struct tm mytime;
	char *formatto=strptime(datestring,"%a, %e %b %Y %H:%M:%S %Z",&mytime);
	if(formatto==0) {
		return 0;
	} else {
		return mktime(&mytime);
	}
}

void redirmake(const char *path)
{
	char *slashloc=(char *)path;
	while((slashloc=strchr(slashloc,'/'))!=0) {
		char foldbuf[1024];
		int slashoffset=slashloc-path+1;
		int mkfold=0;
		strlcpy(foldbuf,path,1024);
		foldbuf[slashoffset-1]='\0'; //why? I guess the pointer is being advanced PAST the slash?
		mkfold=mkdir(foldbuf,0700);
		brintf("Folderbuffer is: %s, status is: %d\n",foldbuf,mkfold);
		if(mkfold!=0) {
			brintf("Here's why: %s\n",strerror(errno));
		}
		slashloc+=1;//iterate past the slash we're on
	}
}
/******************* END UTILITY FUNCTIONS< BEGIN ACTUALLY DOING OF STUFF!!!!! *********************/

#define METAPREPEND		"/.crestfs_metadata_rootnode"
#define METALEN 1024

#define HEADERLEN 65535
#define DIRCACHEFILE "/.crestfs_directory_cachenode"

FILE *
fopenr(char *filename,char*mode)
{	
	redirmake(filename);
	return fopen(filename,mode);
}

#include <libgen.h> //for 'dirname'


#define DIRBUFFER	1*1024*1024
// 1 MB

#define DIRREGEX	"<a[^>]href=['\"]([^'\"]+)['\"][^>]*>([^<]+)</a>"

#define TOOMANYFILES 1000

typedef struct {
	regex_t re;
	regmatch_t rm[3];
	int filecounter;
	int weboffset;
} iterator;

/**** DIRECTORY ITERATOR HELPERS ****/

//THESE ARE NOT YET IN USE!!!!
// But they should be used in 2, probably 3 places:
//	crest_readdir should use a while loop that runs through this function to spit out successive directory entries
//	The impossible-file-detection routine should loop through this function to see if it finds the file its looking for (or not)
//	And get_resource should use it instead of just lstat()'ing its cachefile to see if it can 'hint' as to what's a directory or not
//		(hint: if it ends in a slash, assume it's a directory!)

void
init_directory_iterator(iterator *iter)
{
	memset(iter,0,sizeof(iter));
	int status=regcomp(&iter->re,DIRREGEX,REG_EXTENDED|REG_ICASE);
	if(status!=0) {
		char error[80];
		regerror(status,&iter->re,error,80);
		brintf("ERROR COMPILING REGEX: %s\n",error);
		exit(98);
	}
}

int
directory_iterator(char *directoryfile,iterator *iter,char *buf,int buflen)
{
	char hrefname[255];
	char linkname[255];
	int status=0;

	brintf("Weboffset: %d\n",iter->weboffset);
	while(regexec(&iter->re,directoryfile+iter->weboffset,3,iter->rm,0)==0) {
		reanswer(directoryfile+iter->weboffset,&iter->rm[1],hrefname,255);
		reanswer(directoryfile+iter->weboffset,&iter->rm[2],linkname,255);
		iter->weboffset+=iter->rm[0].rm_eo;

		brintf("href: %s link: %s\n",hrefname,linkname);
		if(strcmp(hrefname,linkname)==0) {
			iter->filecounter++;
			brintf("ELEMENT: %s\n",hrefname);
			strlcpy(buf,hrefname,buflen);
			return 1;
		}
	}
	if(status!=0) {
		char error[80];
		regerror(status,&iter->re,error,80);
		brintf("Regex status is: %d, error is %s\n",status,error);
	}
	return 0;
}

//path will look like "/domainname.com/directoryname/file" - with leading slash

/*
	examples:
	
	desk.nu/infinix <--resource is actually a directory.
	
	check caches
*/
int
impossible_file(const char *origpath)
{
	char path[1024];
	strlcpy(path,origpath,1024);
	strlcat(path,"/",1024); //just to fool the while loop, I know it's ugly, I'm sorry.
	char *slashloc=(char *)path+1;
	char *dirbuffer=malloc(DIRBUFFER); //DIRBUFFER (1 MB) directory page
	regex_t re;

	brintf("TESTIG FILE IMPOSSIBILITY FOR: %s\n",slashloc);
	int status=regcomp(&re,DIRREGEX,REG_EXTENDED|REG_ICASE); //this can be globalized for performance I think.
	if(status!=0) {
		char error[80];
		regerror(status,&re,error,80);
		brintf("ERROR COMPILING REGEX: %s\n",error);
		//this is systemic failure, unmount and die.
		exit(-1);
	}
	
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

		brintf("Component: %s, basename: %s, dirname: %s\n",foldbuf,bn,dn);
		
		strlcat(dirnamebuf,DIRCACHEFILE,1024);
		strlcpy(metafoldbuf,METAPREPEND,1024);
		strlcat(metafoldbuf,"/",1024);
		strlcat(metafoldbuf,dirnamebuf,1024);
		
		brintf("metafolder: %s, dirname is: %s\n",metafoldbuf+1,dirnamebuf+1);
		
		if((metaptr=fopen(metafoldbuf+1,"r"))) {
			//ok, we opened the metadata for the directory..
			char headerbuf[65535];
			struct stat statbuf;
			
			fstat(fileno(metaptr),&statbuf);
			fread(headerbuf,1,65535,metaptr);
			fclose(metaptr);
			//brintf("Buffer we are checking out is: %s",headerbuf);
			if(time(0) - statbuf.st_mtime <= MAXCACHEAGE) {
				//okay, the metadata is fresh...
				brintf("Metadata is fresh enough!\n");
				if((dataptr=fopen(dirnamebuf+1,"r"))) {
					//okay, we managed to open the directory data...
					int failcounter=0;
					char foundit=0;
					regmatch_t rm[3];
					
					memset(rm,0,sizeof(rm));
					fread(dirbuffer,1,DIRBUFFER,dataptr);
					fclose(dataptr);

					//if(offset<++failcounter) filler(buf,"poople",NULL,failcounter);
					//return 0; //infinite loop?
					int weboffset=0;
					brintf("Able to look at directory contents, while loop starting...\n");
					while(status==0 && failcounter < TOOMANYFILES) {
						failcounter++;
						char hrefname[255];
						char linkname[255];

						weboffset+=rm[0].rm_eo;
						brintf("Weboffset: %d\n",weboffset);
						status=regexec(&re,dirbuffer+weboffset,3,rm,0); // ???
						if(status==0) {
							reanswer(dirbuffer+weboffset,&rm[1],hrefname,255);
							reanswer(dirbuffer+weboffset,&rm[2],linkname,255);

							//brintf("Href? %s\n",hrefname);
							//brintf("Link %s\n",linkname);
							brintf("href: %s link: %s\n",hrefname,linkname);
							if(strcmp(hrefname,linkname)==0) {
								char slashedname[1024];
								strlcpy(slashedname,bn,1024);
								strlcat(slashedname,"/",1024);
								brintf("ELEMENT: %s, comparing to %s\n",hrefname,bn);
								if(strcmp(hrefname,bn)==0 || strcmp(hrefname,slashedname)==0) {
									//file seems to exist, let's not bother here anymore
									brintf("FOUND IT! Moving to next one...\n");
									foundit=1;
									break;
								}
							}
						} else {
							char error[80];
							regerror(status,&re,error,80);
							brintf("Regex status is: %d, error is %s\n",status,error);
						}
						//brintf("staus; %d, 0: %d, 1:%d, 2: %d, href=%s, link=%s\n",status,rm[0].rm_so,rm[1].rm_so,rm[2].rm_so,hrefname,linkname);
						//filler?
						//filler(buf,rm[])
					}
					if(foundit==0) {
						//okay, you walked through a FRESH directory listing, looking at all the files
						//and did NOT see one of the components you've asked about. So this file is
						//*IMPOSSIBLE*
						brintf("This file seems pretty impossible to me.!\n");
						free(dirbuffer);
						return 1;
					}
					
				} else {
					brintf("Can't open directory contents file.\n");
				}
			} else {
				brintf("Metadata file is too stale to be sure\n");
			}
		} else {
			brintf("Could not open metadata file\n");
		}

		slashloc+=1;//iterate past the slash we're on
	}
	brintf("File appears not to be impossible, move along...\n");
	free(dirbuffer);
	return 0; //must not be impossible, or we would've returned previously
}

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

#include <sys/file.h> //for flock?


FILE *
get_resource(const char *path,char *headers,int headerlength, int *isdirectory,const char *preferredverb)
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
	if(impossible_file(path)) {
		if(headers && headerlength>0) {
			headers[0]='\0';
		}
		free(headerbuf);
		return 0;
	}
	//FIXME - better than this would be to see inthe directory listing somewher eif this entry exist
	//and has a / after it - if so, hint it to being a directory!!!!!!!!
	//that will improve 'raw' ls -lR performance
	if(lstat(path+1,&cachestat)==0) { //note we don't use stat() time for anything!!! we're just checking for directory mode
		//cache file/directory/link/whatever *does* exist, if it's a directory, push to 'directory mode'
		//that's all we do with this 'stat' value - the bulk of the logic is based on *header* data, not data-data.
		brintf("Cache entity seems to exist?\n");
		if(S_ISDIR(cachestat.st_mode)) {
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
		}
	} else {
		//entity does not exist, so we do NOT want to start using etags for things!
		dontuseetags=1;
	}
	
	//see if we have a cachefile of some sort. how old is it? Check the Date: header in its HTTP header data.
	//does this need to be LOCKed??????????  ************************  read LOCK ??? ***********************
	headerfile=fopenr(headerfilename,"r+"); //the cachefile may not exist - we may have just had the 'real' file, or the directory
						//if so, that's fine, it will parse out as 'too old (1/1/1970)'
	if(headerfile) {
		struct stat statbuf;
		int s=-1;
		flock(fileno(headerfile),LOCK_SH);
		s=stat(headerfilename,&statbuf); //assume infallible
		//read up the file and possibly fill in the headers buffer if it's been passed and it's not zero and headerlnegth is not 0
		fread(headerbuf,1,65535,headerfile);
		brintf("Headers from cache are: %s, statresults is: %d\n",headerbuf,s);
		if(time(0) - statbuf.st_mtime <= MAXCACHEAGE) {
			//our cachefile is recent enough
			FILE *tmp=0;
			printf("RECENT ENOUGH CACHE! SHORT_CIRCUIT! time: %ld, st_mtime: %ld, MAXCACHEAGE: %d, fileage: %ld\n",
				time(0),statbuf.st_mtime,MAXCACHEAGE,time(0)-statbuf.st_mtime);
			/* If your status isn't 200 or 304, OR it actually is, but you can succesfully open your cachefile, 
					then you may return succesfully.
			*/
			if((fetchstatus(headerbuf)!=200 && fetchstatus(headerbuf)!=304) || (tmp=fopen(cachefilebase,"r"))) {
				if(headers && headerlength>0) {
					strncpy(headers,headerbuf,headerlength);
				}
				fclose(headerfile); //RELEASE LOCK!!!!!
				free(headerbuf);
				return tmp;
			} else {
				brintf("I guess I couldn't open my cachefile...going through default behavior (bleh)\n");
			}
		} else {
			brintf("Cache file is too old; continuing with normal behavior. cachefilename: %s Date: %ld, now: %d, MAXCACHEAGE: %d\n",
				headerfilename,statbuf.st_mtime,(int)time(0),MAXCACHEAGE);
		}
	} else {
		//no cachefile exists, we ahve to create one while watching out for races.
		/**** EXCLUSIVE WRITE LOCK! *****/
		//RACE Condition - new file that's never been seen before, when we get *here*, we will have no header cache nor data cache file upon
		//which one might flock() ... but creating said file - when it might be a directory - could be bad. What to do, what to do?
		//fock()
		brintf("Instantiating non-existent metadata cachefile for something.\n");
		if(!(headerfile=fopenr(headerfilename,"w+x"))) {
			brintf("RACE CAUGHT ON FILE CREATION! NOt sure what to do about it though... reason: %s\n",strerror(errno));
			headerfile=fopenr(headerfilename,"w"); //this means a race was found and prevented(?)
		}
		flock(fileno(headerfile),LOCK_SH); //shared lock will immediately be upgraded...
	}
	if(!headerfile) {
		brintf("FAIL TO OPEN DE HEADER-CACHEFILE!");
		exit(9);
	}
	
	//either there is no cachefile for the headers, or it's too old.
	//the weird thing is, we have to be prepared to return our (possibly-outdated) cache, if we can't get on the internet
	
	//check parent directory for recentness, and if it is, and this file isn't in it, return as if 404? 
	//   (shortcuts lots of HEAD's for nonexistent resources)
	
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
	
	mysocket=http_request(webresource,selectedverb,etag);
	
	if(mysocket > 0) {
		char mybuffer[65535];
		int bytes=0;
		char *bufpointer=mybuffer;
		//first fetch headers into headerfile, then fetch body into body file
		//wait,we cant do that. if we're "accelerating" a supposed directory, or we're GET'ing an entity that turns out to be a directory,
		//we won't want to write the headerfiles here.
		
		//also, if this turned out to be a HEAD, we can't write to the data-cachefile
		while ((bytes = recv(mysocket,bufpointer,65535-(bufpointer-mybuffer),0)) && bufpointer-mybuffer < 65535) {
			char *headerend=0;
			brintf("RECV'ed: %d bytes, into %p",bytes,bufpointer);
			if(bytes==-1) {
				brintf("ERROR RECEIVING - timeout or something? strerrror says: %s\n",strerror(errno));
				break; //force pulling from cache
			}
			bufpointer=(bufpointer+bytes);
			
			//keep looking for \r\n\r\n in the header we've got...
			//once we've got that, check status
			if((headerend = strstr(mybuffer,"\r\n\r\n"))) {
				FILE *datafile=0;
				char tempfile[80];
				char dn[1024];
				char location[1024];
				int tmpfd=-1;
				int truncatestatus=0;
				char crestheader[1024]="";
				char contentlength[1024];
				int readlen=0;
				char connection[1024]="";
				
				//found the end of the headers!
				brintf("FOund the end of the headers! this many bytes: %d\n",headerend-mybuffer);
				headerend+=4;
				//brintf("And I think the headers ARE: %s",mybuffer);
				// HERE would be the check for X-Bespin-CREST or something if you want to make sure
				// you're not being Starbucksed.
				// FIXME
				fetchheader(mybuffer,"Connection",connection,1024);
				if(strcasecmp(connection,"close")==0) {
					char host[1024];
					char pathpart[1024];
					brintf("HTTP/1.1 pipeline connection CLOSED, yanking from list.");
					pathparse(path,host,pathpart,1024,1024);
					delete_keep(host);
				}
				/* fetchheader(mybuffer,"x-bespin-crest",crestheader,1024);
				if(strlen(crestheader)==0) {
					brintf("COULD NOT Find Crest-header - you have been STARBUCKSED. Going to cache!\n");
					break;
				} */
				/* problems with the anti-starbucksing protocol - need to handle redirects and 404's
					without at least the 404 handling, you can't negatively-cache nonexistent files.
				*/
				crestheader[0]='\0';
				brintf("We should be fputsing it to: %p\n",headerfile);
				truncatestatus=ftruncate(fileno(headerfile),0); // we are OVERWRITING THE HEADERS - we got new headers, they're good, we wanna use 'em
				rewind(headerfile); //I think I have to do this or it will re-fill with 0's?!
				//brintf(" Craziness - the number of bytes we should be fputsing is: %d\n",strlen(mybuffer));
				brintf("truncating did : %d",truncatestatus);
				if(truncatestatus) {
					brintf(", cuzz: %s\n",strerror(errno));
				}
				brintf("\n");
								
				if(headerend-mybuffer>=65535) {
					brintf("Header overflow, I bail.\n");
					exit(99);
				}
				strncpy(headerbuf,mybuffer,headerend-mybuffer);
				headerbuf[headerend-mybuffer]='\0';
				fputs(headerbuf,headerfile);
				if(headers && headerlength>0) {
					strncpy(headers,headerbuf,headerlength);
				}
				
				//copy the newly-found headers to the headerfile and keep fetching into the file buffer
				//unlock?!!? ***** UNlocK ******
				//rename() file into place, rewind file pointer thingee,
				//freopen file pointer thingee to be read-only, and then
				//return the file buffer
				//write the remnants of the recv'ed header...
				brintf("HTTP Header is: %d\n",fetchstatus(mybuffer));
				fetchheader(headerbuf,"content-length",contentlength,1024);
				readlen=atoi(contentlength);
				
				switch(fetchstatus(mybuffer)) {
					case 200:
					//ONE THING WE NEED TO NOTE - if this was a HEAD instead of a GET,
					//we need to be sensitive about the cachefile?
					if(strcmp(selectedverb,"HEAD")==0) {
						fclose(headerfile); //RELEASE LOCK!
						free(headerbuf);
						brintf("DUDE TOTALLY SUCKY!!!! Somebody HEAD'ed a resource and we had to unlink its data file.\n");
						unlink(cachefilebase);
						close(mysocket);
						return 0;// NO CONTENTS TO DEAL WITH HERE!
					}
					strncpy(dn,cachefilebase,1024);
					dirname(dn);
					strncpy(tempfile,dn,80);
					strncat(tempfile,"/.tmpfile.XXXXXX",80);
					redirmake(tempfile); //make sure intervening directories exist, since we can't use fopenr()
					tmpfd=mkstemp(tempfile); //dumbass, this creates the file.
					brintf("tempfile will be: %s, it's in dirname: %s\n",tempfile,dn);
					datafile=fdopen(tmpfd,"w+");
					if(!datafile) {
						brintf("Cannot open datafile for some reason?: %s",strerror(errno));
					}
					brintf("These are how many bytes are in the Remnant: %d out of %d read\n",bufpointer-headerend,bytes);
					brintf("Here's the data you should be writing: %hhd %hhd %hhd\n",headerend[0],headerend[1],headerend[2]);
					brintf("Datafile is: %p\n",datafile);
					if(bufpointer-headerend>=readlen) {
						brintf("Remnant write is enough to satisfy request, not going into While loop.\n");
						fwrite(headerend,readlen,1,datafile);
					} else {
						fwrite(headerend,(bufpointer-headerend),1,datafile);
						while((bytes = recv(mysocket,mybuffer,65535,0))) { //re-use existing buffer
							long int curbytes=0;
							if(bytes==-1) {
								brintf("Recv returned -1. FAIL?\n");
								break;
							}
							fwrite(mybuffer,bytes,1,datafile);
							curbytes=ftell(datafile);
							brintf("Read %d bytes, expecting total of %d(%s), curerently at: %ld",bytes,readlen,contentlength,curbytes);
							if(curbytes>=readlen) {
								brintf("Okay, read enough data, bailing out of recv loop. Read: %ld, expecting: %d\n",ftell(datafile),readlen);
								break;
							}
						}
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
					fclose(headerfile); //RELEASE LOCK
					free(headerbuf);
					close(mysocket);
					return datafile;
					break;
					
					case 304:
					//copy the newly found headers to the headerfile (to get the new Date: header),
					//return the EXISTING FILE buffer (no change)
					//we do *NOT* want any remnants from the netowrk'ed gets, we're going to let them die in the buffer
					datafile=fopen(cachefilebase,"r"); //use EXISTING basefile...
					fclose(headerfile); // RELEASE LOCK
					free(headerbuf);
					close(mysocket);
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
					
					fetchheader(headerbuf,"location",location,1024);
					if(strcmp(location,"")!=0 && location[strlen(location)-1]=='/') {
						brintf("Location discovered to be: %s, assuming DIRECTORY and rerunning!\n",location);
						//assume this must be a 'directory', but we requested it as if it were a 'file' - rerun!
						redirmake(cachefilebase); //make enough of a path to hold what will go here
						unlink(cachefilebase); //if it's a file, delete the file; it's not a file anymore
						mkdir(cachefilebase,0700); 
						unlink(headerfilename); //the headerfile being a file will mess things up too
									//and besides, it's just going to say '301', which isn't helpful
						fclose(headerfile);
						free(headerbuf);
						close(mysocket);
						return get_resource(path,headers,headerlength,isdirectory,preferredverb);	
					}
					//otherwise (no slash at end of location path), we must be a plain, boring symlink or some such.
					//yawn.
					brintf("Not a directory, treating as symlink...\n");
					fclose(headerfile); //release lock
					free(headerbuf);
					close(mysocket);
					return 0; //do we return a filepointer to an empty file or 0? NOT SURE!
					break;
					
					case 404:
					case 403: //permission denied/requires authentication
					//Only weird case we got is *IF* we 'accelerated' this into a directory request, and we 404'ed,
					//it may be a regular file now. DELETE THE DIRECTORY, possibly the directoryfilenode thing,
					//and RETRY REQUEST!!!
					
					//be prepared to drop a 404 cachefile! This will prevent repeated requests for nonexistent entities
					if(dirmode) {
						//we 404'ed in dirmode. Shit.
						char headerdirname[1024];
						brintf("Directory mode resulted in 404. Retrying as regular file...\n");
						unlink(cachefilebase);
						unlink(headerfilename);
						rmdir(path+1);
						strncpy(headerdirname,slashlessmetaprepend,1024);
						strncat(headerdirname,path,1024);
						rmdir(headerdirname);
						brintf("I should be yanking directories %s and %s\n",path+1,headerdirname);
						close(mysocket);
						return get_resource(path,headers,headerlength,isdirectory,preferredverb);
					}
					brintf("404 mode, I *may* be closing the cache header file...\n");
					brintf(" Results: %d",fclose(headerfile)); //Need to release locks on 404's too!
					close(mysocket);
					return 0; //nonexistent file, return 0, hold on to cache, no datafile exists.
					break;
					
					default:
					brintf("Unknown HTTP status?!");
					exit(23); //not implemented
					break;
				}
			} 
		}
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
	if(lstat(cachefilebase,&cachestat)==0 && S_ISREG(cachestat.st_mode)) {
		staledata=fopen(cachefilebase,"r"); //could be 0
		if(!staledata) {
			brintf("Crap, coudln't open datafile even though we could stat it. Why?!?! %s\n",strerror(errno));
		} else {
			brintf("Data file opened (%p)\n",staledata);
		}
	} else {
		brintf("Either couldn't stat cache or cache ain't a plain file: %s\n",strerror(errno));
	}
	fclose(headerfile); //RELEASE LOCK!
	brintf("fclose'd headerfile\n");
	brintf("About to return data file: %p\n",staledata);
	return staledata;
}

static int
crest_getattr(const char *path, struct stat *stbuf)
{
	char header[HEADERLEN];

	if (strcmp(path, "/") == 0) { /* The root directory of our file system. */
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 1; //set this to '1' to force find(1) to be able to iterate
	} else {
		char hostname[80];
		char pathpart[1024];
		pathparse(path,hostname,pathpart,80,1024);
		
		if(strcmp(pathpart,"/")==0 || strcmp(path,"/")==0) {
			//root of a host, MUST be a directory no matter what.
			stbuf->st_mode = S_IFDIR | 0755; //I am a a directory!
			stbuf->st_nlink = 1; //use '1' to allow find(1) to decend within...
		} else {
			FILE *cachefile=0;
			int isdirectory=-1;
/*			//zeroth: check dir STAT cache
			dircachestat=finddircache(path);
			if(dircachestat) {
				if((dircachestat->st_mode & S_IFMT) == 0) {
					//NAK stat
					return -ENOENT;
				} else {
					memcpy(stbuf,dircachestat,sizeof(struct stat));
					return 0;
				}
			} */
			
//			FILE *
//			get_resource(const char *path,char *headers,int headerlength, int *isdirectory,const char *preferredverb);
			
			/**** ALL WORK IS DONE WITH get_resource! ****/
			
			cachefile=get_resource(path,header,HEADERLEN,&isdirectory,"HEAD");
			
			/*** THAT DID A LOT! ****/
			
			if(isdirectory) {
				brintf("Getattr for directory mode detected.\n");
				stbuf->st_mode = S_IFDIR | 0755;
				stbuf->st_nlink = 1;
			} else {
				int httpstatus=fetchstatus(header);
				char date[80];
				char length[80]="";

				brintf("Status: %d\n",httpstatus);
				brintf("Header you got was: %s\n",header);
				switch(httpstatus) {
					case 200:
					case 304:
					brintf("It is a file\n");
					stbuf->st_mode = S_IFREG | 0755;
					brintf("Pre-mangulation headers: %s\n",header);
					fetchheader(header,"last-modified",date,80);
					fetchheader(header,"content-length",length,80);
					if(strlen(length)>0) {
						//use content-length in case this file was only HEAD'ed, otherwise seek to its end
						stbuf->st_size=atoi(length);
					} else {
						if(!cachefile) {
							brintf("WEIRD! No cachefile given on a %d, and couldn't find content-length!!! Let's see if there's anything interesting in errno: %s\n",httpstatus,strerror(errno));
							//exit(57);
							stbuf->st_size=0;
						} else {
							int seeker=fseek(cachefile,0,SEEK_END);
							if(seeker) {
								brintf("Can't seek to end of file! WTF!\n");
								exit(88);
							}
							stbuf->st_size= ftell(cachefile);
						}
					}
					//brintf("Post-mangulation headers is: %s\n",header+1);
					//brintf("Post-mangulation headers is: %s\n",header);
					//brintf("BTW, date I'm trying to format: %s\n",date);
					brintf("WEIRD...date: %s, length: %d\n",date,(int)stbuf->st_size);
					stbuf->st_mtime = parsedate(date);
					break;
					
					case 301:
					case 302:
					case 303:
					case 307:
					//http redirect, means either DIRECTORY or SYMLINK
					//start out by keeping it simple - if it ends in a
					//ZERO caching attempts, to start with...
					stbuf->st_mode = S_IFLNK;
					stbuf->st_nlink = 1;
					break;
					
					case 404:
					if(cachefile) {
						brintf("Why did I get a valid cachefile on a 404?!");
						fclose(cachefile);
					}
					return -ENOENT;
					break;
					
				}
				
			}
			
			if(cachefile)
				fclose(cachefile);
			else
				brintf("TRY TO CLOSE NO CACHEFILE!\n");
		}
	}

	return 0;
}

static int
crest_readlink(const char *path, char * buf, size_t bufsize)
{
	//we'll start with the assumption this actuallly IS a symlink.
	char location[4096]="";
	char header[HEADERLEN];
	FILE *cachefile;
	int is_directory=-1;
	
	cachefile=get_resource(path,header,HEADERLEN,&is_directory,"HEAD");
	
	if(is_directory) {
		if(cachefile) {
			fclose(cachefile);
		}
		return -EINVAL;
	}
			
	fetchheader(header,"Location",location,4096);
	
	//from an http://www.domainname.com/path/stuff/whatever
	//we must get to a local path.
	if(strncmp(location,"http://",7)!=0) {
		brintf("Unsupported protocol, or missing 'location' header: %s\n",location);
		return -ENOENT;
	}
	strlcpy(buf,rootdir,bufsize); //fs 'root'
	strlcat(buf,"/",bufsize);
	strlcat(buf,location+7,bufsize);
	
	brintf("I would love to unlink: %s, and point it to: %s\n",path+1,buf);
	int unlinkstat=unlink(path+1);
	int linkstat=symlink(buf,path+1);
	
	brintf("Going to return link path as: %s (unlink status: %d, link status: %d)\n",buf,unlinkstat,linkstat);
	if(cachefile) {
		brintf("Freaky, got a valid cachefile for a symlink?!\n");
		fclose(cachefile);
	}
	return 0;
}

static int
crest_open(const char *path, struct fuse_file_info *fi)
{
	FILE *rsrc=0;
	int is_directory=-1;
	if ((fi->flags & O_ACCMODE) != O_RDONLY) { /* Only reading allowed. */
		return -EACCES;
	}
	rsrc=get_resource(path,0,0,&is_directory,"GET");
	if(is_directory) {
		if(rsrc) {
			fclose(rsrc);
		}
		return -EISDIR;
	}
	if(rsrc) {
		brintf("Going to set filehandle to POINTER: %p\n",rsrc);
		fi->fh=(long int)rsrc;
		return 0;
	} else {
		brintf("Uh, no resource retrieved? Setting fh to zero?\n");
		fi->fh=0;
		return 0;
	}
	return -EACCES;
}

static int
crest_release(const char *path, struct fuse_file_info *fi)
{
	brintf("Closing filehandle %p: for file: %s\n",(FILE *)(unsigned int)fi->fh,path);
	return fclose((FILE *)(unsigned int)fi->fh);
}

static int
crest_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
              off_t offset, struct fuse_file_info *fi)
{
	FILE *dirfile=0;
	int is_directory=-1;
	if(fi) {
		brintf("Well, dunno why this is, but we have file info: %p, filehandle: %d\n",fi,(int)fi->fh);
	}

	//should do a directory listing in the domains directory (the first one)
	// but let's do that later.
	
	if(strcmp(path,"/")==0) {
		brintf("PATH IS /!!!!!\n");
		DIR *mydir;
		struct dirent *dp;
		int id=1;
		
		mydir=opendir(".");
		while((dp=readdir(mydir))!=0) {
			brintf("Potential dir is: %s, id: %d (vs offset: %d)\n",dp->d_name,id,(int)offset);
			if(id>offset) {
				if(filler(buf,dp->d_name,NULL,id)!=0) {
					break;
				}
			}
			id++;
		}
		brintf("Done with dir listing, returning...");
		return 0;
	}

	char *dirbuffer=malloc(DIRBUFFER);
	if(dirbuffer==0) {
		brintf("Great, can't malloc our dirbuffer. baililng.\n");
		exit(19);
	}
	dirbuffer[0]='\0';
	
	regex_t re;
	regmatch_t rm[3];
	
	memset(rm,0,sizeof(rm));
	
	dirfile=get_resource(path,0,0,&is_directory,"GET");
	if(!is_directory) {
		if(dirfile) {
			fclose(dirfile);
		}
		return -ENOTDIR;
	}

	fread(dirbuffer,1,DIRBUFFER,dirfile);
	fclose(dirfile);

	int status=regcomp(&re,DIRREGEX,REG_EXTENDED|REG_ICASE); //this can be globalized for performance I think.
	if(status!=0) {
		char error[80];
		regerror(status,&re,error,80);
		brintf("ERROR COMPILING REGEX: %s\n",error);
		//this is systemic failure, unmount and die.
		exit(-1);
	}
	int failcounter=0;
	if(offset<++failcounter) filler(buf, ".", NULL, failcounter); /* Current directory (.)  */
	if(offset<++failcounter) filler(buf, "..", NULL, failcounter);          /* Parent directory (..)  */
	//if(offset<++failcounter) filler(buf,"poople",NULL,failcounter);
	//return 0; //infinite loop?
	int weboffset=0;
	while(status==0 && failcounter < TOOMANYFILES) {
		failcounter++;
		char hrefname[255];
		char linkname[255];
		
		weboffset+=rm[0].rm_eo;
		brintf("Weboffset: %d\n",weboffset);
		status=regexec(&re,dirbuffer+weboffset,3,rm,0); // ???
		if(status==0) {
			reanswer(dirbuffer+weboffset,&rm[1],hrefname,255);
			reanswer(dirbuffer+weboffset,&rm[2],linkname,255);

			//brintf("Href? %s\n",hrefname);
			//brintf("Link %s\n",linkname);
			brintf("href: %s link: %s\n",hrefname,linkname);
			if(strcmp(hrefname,linkname)==0) {
				brintf("ELEMENT: %s\n",hrefname);
				if(offset<failcounter) 
					if(filler(buf, hrefname, NULL, failcounter)!=0) {
						brintf("Filler said not zero.\n");
						break;
					}
			}
		}
		//brintf("staus; %d, 0: %d, 1:%d, 2: %d, href=%s, link=%s\n",status,rm[0].rm_so,rm[1].rm_so,rm[2].rm_so,hrefname,linkname);
		//filler?
		//filler(buf,rm[])
	}
	brintf("while loop - complete\n");
	if(failcounter>=TOOMANYFILES) {
		brintf("Fail due to toomany\n");
	}
	free(dirbuffer);
	return 0; //success? or does that mean fail?
}

#define FILEMAX 64*1024*1024

static int
crest_read(const char *path, char *buf, size_t size, off_t offset,
           struct fuse_file_info *fi)
{
	FILE *cachefile;
	
	if(fi) {
		brintf("Good filehandle for path: %s, pointer:%p\n",path,(FILE *)(unsigned int)fi->fh);
	} else {
		brintf("BAD FILE*INFO* IS ZERO for path: %s\n",path);
		brintf("File was not properly opened?\n");
		return -EIO;
	}
	cachefile=(FILE *)(unsigned int)fi->fh;

	if(fseek(cachefile,offset,SEEK_SET)==0) {
		return fread(buf,1,size,cachefile);
	} else {
		//fail to seek - what'ssat mean?
		return 0;
	}
}

int	cachedir = 0;

static void *
crest_init(struct fuse_conn_info *conn)
{
	brintf("I'm not intending to do anything with this connection: %p\n",conn);
	fchdir(cachedir);
	close(cachedir);
	return 0;
}

static struct fuse_operations crest_filesystem_operations = {
    .init    = crest_init,    /* mostly to keep access to cache     */
    .getattr = crest_getattr, /* To provide size, permissions, etc. */
    .readlink= crest_readlink,/* to handle funny server redirects */
    .open    = crest_open,    /* To enforce read-only access.       */
    .release = crest_release, /* to hold on to a filehandle to the cache so that reads are consistent with one generation of the file */
    .read    = crest_read,    /* To provide file content.           */
    .readdir = crest_readdir, /* To provide directory listing.      */
};

/********************** Test routines **********************/

void pathtest(char *fullpath)
{
	char host[HOSTLEN];
	char path[PATHLEN];
	pathparse(fullpath,host,path,HOSTLEN,PATHLEN);
	brintf("Okay, from path '%s' host is: '%s' and path is: '%s'\n",fullpath,host,path);
}

void hdrtest(char *header,char *name)
{
	char pooh[90];
	fetchheader(header,name,pooh,80);
	brintf("Header name: %s: value: %s\n",name,pooh);
}

void strtest()
{
	char buffera[32];
	char bufferb[32];
	char bufferc[64];
	int result=0;

	result=strlcpy(bufferb,"Hello",32);
	brintf("strlcpy: %s, %d",bufferb,result);
	
	
	
	result=strlcpy(buffera,"HelloHelloHelloHelloHelloHelloHello",32);
	brintf("strlcpy: %s, %d",buffera,result);
	
	result= strlcat(bufferc,buffera,64);
	brintf("Strlcat: %s, %d",bufferc,result);

	result= strlcat(bufferc,buffera,64);
	brintf("strlcat: %s, %d",bufferc,result);

	result= strlcat(bufferc,buffera,64);
	brintf("strlcat: %s, %d",bufferc,result);
}


void pretest()
{
	char buf[DIRBUFFER];
/*	pathtest("/desk.nu");
	pathtest("/desk.nu/pooh.html");
	pathtest("/desk.nu/braydix");
	pathtest("/desk.nu/braydix/"); */
	//exit(0);
				
	//REGEX TESTING STUFF:
 /*	if(status) {     
		char error[80];
		regerror(status,&re,error,80);
		brintf("error: %s\n",error);
	} else {
		brintf("NO FAIL!!!!!!!!!!!\n");
	}
	exit(0);
	regfree(&re); */
	
	//webfetch("/desk.nu/braydix",buf,DIRBUFFER,"HEAD","");
	hdrtest(buf,"connection");
	hdrtest(buf,"Scheissen");
	hdrtest(buf,"etag");
	hdrtest(buf,"date");
	hdrtest(buf,"content-type");
	hdrtest(buf,"sErVeR");
	brintf("Header status: %d\n",fetchstatus(buf));
	brintf("Lookit: %s\n",buf);
	
	strtest();
	exit(0);
}

/********************** END TESTING **********************/

void
addparam(int *argc,char ***argv,char *string)
{
	char **previous=*argv;
	int newsize=(sizeof(char **))*(*argc+1);
	int i;
	//brintf("BEFORE\n");
	for(i=0;i<*argc;i++) {
		//brintf("[%d] %s, ",i,(*argv)[i]);
	}
	//brintf("\n");
	*argv=malloc(newsize);
	//brintf("malloc'ed: %d\n",newsize);
	if(*argc!=0) {
		int prevsize=(sizeof(char **))*(*argc);
		//brintf("Previous size: %d\n",prevsize);
		//brintf("previous sub-zero is: %s\n",previous[0]);
		memcpy(*argv,previous,prevsize);
		free(previous);
		(*argv)[*argc]=0;
	}
	//brintf("ABOUT TO ASSIGN '%s' to *argc: %d\n",string,*argc);
	(*argv)[*argc]=string;
	(*argc)++;
	for(i=0;i<*argc;i++) {
		//brintf("[%d] %s, ",i,(*argv)[i]);
	}
	//brintf("\n");
}

#ifndef TESTFRAMEWORK

int
main(int argc, char **argv)
{
	int myargc=0;
	char **myargs=0;
	int i=-1;
	
	//visible USAGE: 	./crestfs /tmp/doodle [cachedir]
//	pretest();
	
	//INVOKE AS: 	./crestfs /tmp/doodle /tmp/cacheplace -r -s -d -o nolocalcaches
	
	// single user, read-only. NB!
	
	if(argc<3) {
		brintf("Not right number of args, you gave me %d\n",argc);
		brintf("Usage: %s mountdir cachedir [options]\n",argv[0]);
		exit(1);
	}
	//brintf("Decent. Arg count: %d\n",argc);
	addparam(&myargc,&myargs,argv[0]);
	//myargs[0]=argv[0];
	addparam(&myargc,&myargs,argv[1]);
	if(argv[1][0]!='/') {
		char *here=getcwd(NULL,0);
		strlcpy(rootdir,here,1024);
		strlcat(rootdir,"/",1024);
	}
	strlcat(rootdir,argv[1],1024); //first parameter, is mountpoint
	cachedir= open(argv[2], O_RDONLY);
	if(cachedir==-1) {
		brintf("%d no open cachedir\n",cachedir);
	}
	for(i=3;i<argc;i++) {
		addparam(&myargc,&myargs,argv[i]);
	}
	addparam(&myargc,&myargs,"-r");
	// -s ? -f ? -d ? 
	#ifdef __APPLE__
	addparam(&myargc,&myargs,"-o");
	addparam(&myargc,&myargs,"nolocalcaches");
	#else
	addparam(&myargc,&myargs,"-o");
	addparam(&myargc,&myargs,"nonempty");
	#endif
/* 	#ifdef __APPLE__
	char *myargs[]= {0, 0, "-r", "-s", "-f", "-d","-o","nolocalcaches", 0 };
	#define ARGCOUNT 8
	#else
	char *myargs[]= {0, 0, "-r", "-s", "-f", "-d", "-o", "nonempty", 0 };
	#define ARGCOUNT 8
	#endif */
	return fuse_main(myargc, myargs, &crest_filesystem_operations, NULL);
}
#else
int
main(int argc, char **argv)
{
	char headers[65535];
	char teenybuffer;
	FILE *resource=0;
	int isdirectory=-1;
	void *stupid=crest_filesystem_operations.init; //just to keep from complaining
	//FILE *get_resource(const char *path,char *headers,int headerlength, int *isdirectory,const char *preferredverb)
	resource=get_resource(argv[1],headers,65535,&isdirectory,argv[2]);
	printf("IS Directory?: %d\n",isdirectory);
	if(resource) {
		while(fread(&teenybuffer,1,1,resource)) {
			printf("%c",teenybuffer);
		}
	} else {
		printf("NO RESOURCE FOUND. 'k?");
	}
	isdirectory=(int)stupid;
	return 0;
}
#endif