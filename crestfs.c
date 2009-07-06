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

#define _FILE_OFFSET_BITS 64
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


#define FETCHBLOCK 65535
#define HOSTLEN 64
#define PATHLEN 1024

char *flagfilename="/tmp/crestnetworking";
int networkmode=0;

/* GIVEN: a 'path' where the first element is a hostname, the rest are path elements
   RETURN: 
	number of bytes in the header and contents together
*/

#include <resolv.h>

int 
webfetch(const char *path,char *buffer,int maxlength, const char *verb,char *etag)
{
	int sockfd=-1, numbytes;  
	struct addrinfo hints, *servinfo, *p;
	int rv;
	char hostname[HOSTLEN];
	char pathstub[PATHLEN];
	char *reqstr=0;
	int bytessofar=0;
	char etagheader[1024]="";
//	char *origbuffer=buffer;
//	struct stat flagstat;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	
	etagheader[0]='\0';
	if(strcmp(etag,"")!=0) {
		strcpy(etagheader,"If-None-Match: ");
		strlcat(etagheader,etag,1024);
		strlcat(etagheader,"\r\n",1024);
	}
	
/*	if(stat(flagfilename,&flagstat)!=0) {
		brintf("Networking not enabled via %s yet\n",flagfilename);
		return 0;
	} else {
		brintf("Found %s file! Fetching web...\n",flagfilename);
		if(networkmode==0) {
			brintf("First time network mode enabled. Re-initializing resolver.\n");
			res_init();
			networkmode=1;
		}
	}
*/	pathparse(path,hostname,pathstub,HOSTLEN,PATHLEN);
	
	brintf("Hostname is: %s\n",hostname);

	if ((rv = getaddrinfo(hostname, "80", &hints, &servinfo)) != 0) {
		brintf("I failed to getaddrinfo: %s\n", gai_strerror(rv));
		buffer[0]='\0';
		return 0;
	}
	brintf("Got getaddrinfo()...\n");

	// loop through all the results and connect to the first we can
	for(p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype,
				p->ai_protocol)) == -1) {
			perror("client: socket");
			continue;
		}

		if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
			close(sockfd);
			perror("client: connect");
			continue;
		}

		break;
	}
	
	if (p == NULL) {
		fprintf(stderr, "client: failed to connect\n");
		buffer[0]='\0';
		close(sockfd);
		return 0;
	}
	
	brintf("Okay, connectication has occurenced\n");

/*     getsockname(sockfd, s, sizeof s);
    brintf("client: connecting to %s\n", s);
*/
	freeaddrinfo(servinfo); // all done with this structure
	asprintf(&reqstr,"%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: CREST-fs/0.2\r\n%s\r\n",verb,pathstub,hostname,etagheader);
	brintf("REQUEST: %s\n",reqstr);
	send(sockfd,reqstr,strlen(reqstr),0);
	free(reqstr);
	

	while ((numbytes = recv(sockfd, buffer, FETCHBLOCK-1, 0)) >0 && bytessofar <= maxlength) {
		//keep going
		bytessofar+=numbytes;
		buffer=(buffer+numbytes);
	}
	if(numbytes<0) {
		brintf("Error - %d\n",numbytes);
		buffer[0]='\0';
		close(sockfd);
		return 0;
	}
	if(bytessofar > maxlength) {
		brintf("OVERFLOW: %d, max: %d\n",bytessofar,maxlength);
		buffer[0]='\0';
		close(sockfd);
		return 0;
	}

	buffer[numbytes] = '\0';

//    brintf("client: received '%s'\n",origbuffer);

	close(sockfd);

	return bytessofar;
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

//path will look like "/domainname.com/directoryname/file" - with leading slash

FILE *
get_cacheelem(const char *path,char *headers,int maxheaderlen)
{
	//there is a possibility that the 'file' that we're opening is not a file, after all, if so - return 0 so we don't try to write to it
	//specifically, this seems to happen with SYMLINKS
	struct stat precheck;
	brintf("I am giong to try to stat: %s\n",path+1);
	
	if(lstat(path+1,&precheck)==0 && !(S_ISREG(precheck.st_mode))) {
		//if you're a FIFO, a directory, or a symlink - anything other than 'plain-jane file', return 0;
		brintf("Cachefile exists and is not a regular file - returning '0', mode: %d\n",precheck.st_mode);
		return 0;
	}
	FILE *cachefile=fopen(path+1,"r+");
	int cachelen=0;
	if(!cachefile) {
		//couldn't make the cachefile easily, check we've got folders
		brintf("Couldn't make cachefile '%s' easily, checking folders...\n",path+1);
		redirmake(path+1);
		cachefile=fopen(path+1,"w+"); //should we force exclusive!?
	}
	if(!cachefile) {
		brintf("WARNING: Cache file opened read-only (could not open read-write)!!!!\n");
		cachefile=fopen(path+1,"r");
		if(!cachefile) {
			brintf("ERROR: couldn't even open cachefile '%s' read-only. Bailing.\n",path+1);
			perror("Cuzza: ");
			exit(76);
		}
	}
	if(cachefile && !feof(cachefile) && headers) {
		cachelen=fread(headers,1,maxheaderlen,cachefile);
		brintf("Reading into cache... %d bytes\n",cachelen);
	}
	if(headers) {
		brintf("headers are not zero, cachelen is: %d\n",cachelen);
		headers[cachelen]='\0'; //should also handle the case where the file didn't exist, and nothing was read.
	}
	brintf("going to rewind cachefile\n");
	int fs=fseek(cachefile,0,SEEK_SET);
	clearerr(cachefile);
	//rewind(cachefile);
	brintf("returning rew status: %d\n",fs);
	return cachefile;
}

#define METAPREPEND		"/.crestfs_metadata_rootnode"
#define METALEN 1024

void get_cachefiles(const char *path,FILE **header,FILE **data,void *headbuf,int bufferlen)
{
	char metafilename[METALEN];
	
	//for the metafilename, we need to transform from:
	//   /domainname.com/dirname/filename
	//TO:
	//	 /.crestfs_metadata_rootnode/domainname.com/dirname/filename
	
	strcpy(metafilename,METAPREPEND);
	strlcat(metafilename,path,METALEN);
	
//	*metafile=fopen(metafilename,"r+");
	
	*header=get_cacheelem(metafilename,headbuf,bufferlen);
	*data=get_cacheelem(path,0,0);
	if(*header==0 || *data==0) {
		brintf("*header: %p, *data: %p\n",*header,*data);
	}
}

/*** Directory Cache functionality ***/

int dircacheentries=0;
struct dircacheentry {
	char *path;
	time_t	when;
	struct stat st;
};

#define DIRCACHESIZE 128

struct dircacheentry dircache[128];

struct stat *finddircache(const char *path)
{
	int i;
	for(i=0;i<dircacheentries;i++) {
		if(strcmp(dircache[i].path,path)==0) {
			return &(dircache[i].st);
		}
	}
	return 0;
}

void	insertdircache(const char *path,struct stat *st)
{
	if(dircacheentries>=DIRCACHESIZE) {
		//no more directory cache entries for you right now
		//later we will do something clever with finding the
		//oldest entry and getting rid. but for now, just do nothing.
		return;
	}
	dircache[dircacheentries].path=strdup(path);
	dircache[dircacheentries].when=time(0);
	memcpy(&(dircache[dircacheentries].st),st,sizeof(struct stat));
	dircacheentries++;
}

/******************* END UTILITY FUNCTIONS< BEGIN ACTUALLY DOING OF STUFF!!!!! *********************/

#define HEADERLEN 65535
#define DIRCACHEFILE "/.crestfs_directory_cachenode"

static int
crest_getattr(const char *path, struct stat *stbuf)
{
	char header[HEADERLEN];
	memset(stbuf, 0, sizeof(struct stat));
	header[0]='\0';

	if (strcmp(path, "/") == 0) { /* The root directory of our file system. */
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 1; //set this to '1' to force find(1) to be able to iterate
	} else {
		char hostname[80];
		char pathpart[1024];
		int st=0;
		pathparse(path,hostname,pathpart,80,1024);
		
		if(strcmp(pathpart,"/")==0) {
			//root of a host, MUST be a directory no matter what.
			stbuf->st_mode = S_IFDIR | 0755; //I am a a directory!
			stbuf->st_nlink = 1; //use '1' to allow find(1) to decend within...
		} else {
			char results[1024];
			struct stat cachestat;
			char dircachepath[1024];
			struct stat *dircachestat;
			FILE *cachefile=0;
			FILE *headerfile=0;
			int statstatus=0;
			//zeroth: check dir STAT cache
			dircachestat=finddircache(path);
			if(dircachestat) {
				if((dircachestat->st_mode & S_IFMT) == 0) {
					//NAK stat
					return -ENOENT;
				} else {
					memcpy(stbuf,dircachestat,sizeof(struct stat));
					return 0;
				}
			}
			//first! Check cache
			brintf("going to be statting: %s\n",path+1);
			if(lstat(path+1,&cachestat)==0) {
				char date[80];
				
				statstatus=1;
				brintf("Stat successful for getattr, let's see if it's new enough...\n");
				if (S_ISLNK(cachestat.st_mode)) {
					brintf("seems to be a link: %s...\n",path);
					stbuf->st_mode = S_IFLNK | 0755;
					stbuf->st_nlink = 1;
					return 0;
				} else if(S_ISDIR(cachestat.st_mode)) {
					struct stat metastat;
					
					memset(&metastat,0,sizeof(metastat));
					//now check the dircache file
					brintf("Cache file is a directory file, statting further...\n");
					strlcpy(dircachepath,path+1,1024);
					strlcat(dircachepath,DIRCACHEFILE,1024);
					if(stat(dircachepath,&metastat)==0 && metastat.st_mtime>time(0)-MAXCACHEAGE) {
						//recent enough cache, don't bother with the HEAD
						stbuf->st_mode = S_IFDIR | 0755;
						stbuf->st_nlink = 1;
						///you got this from the cached files
						//don't need to cache this in the fast 
						//dircache
						return 0;
					} else {
						brintf("old dircache file %s -refetching.\n",dircachepath);
					}
				} else if (S_ISREG(cachestat.st_mode)) {
					//see if cached file is recent enough to use, if so, use it?
					//_should_ be via 'date' field, but maybe we'll use the stat results
					//we already have?
					
					brintf("Cache file on behalf of path: %s is a FILE, fetching info on it...\n",path);
					get_cachefiles(path,&headerfile,&cachefile,header,HEADERLEN); //MAKE SURE TO CLOSE THESE!!!!!!!
					brintf("Header info is: %s\n",header);
					brintf("headerfile: %p, cachefile: %p\n",headerfile,cachefile);
					fetchheader(header,"date",date,80);
					brintf("Cachefile age: %s, in seconds: %ld\n",date,time(0)-parsedate(date));
					if(time(0) - parsedate(date) > MAXCACHEAGE) {
						//it's too old, invalidate the header array so it will get picked up normally
						brintf("Cachefile too old!\n");
						header[0]='\0';
					}
					//otherwise, we've pre-populated 'header' with the CACHE header
				} else {
					brintf("Dunno WHUT kind a file this is... %x\n",cachestat.st_mode);
					//exit(99);
					return -ENOENT; //no special files allowed
				}
			}
			if(strlen(header)==0) {
				st=webfetch(path,header,HEADERLEN,"HEAD","");
			}
			brintf("webfetch returns: %d\n",st);
			if(st!=0) {
				// this means, nobody's fetched, or if they
				// did fetch, the network's down or not working
				if(fetchstatus(header)>=400 && fetchstatus(header)<500) { //400, e.g., 404...
					struct stat blankstat;
					memset(&blankstat,0,sizeof(struct stat));
					brintf("nonexistent file, so sayeth the Server. Obey.\n");
					if(cachefile) fclose(cachefile);
					if(headerfile) fclose(headerfile);
					insertdircache(path,&blankstat);
					return -ENOENT;
				}
				//brintf("Headers fetched: %s",header);
				fetchheader(header,"Location",results,1024);
				//brintf("Location found? %s\n",results);
				//so - question - how do we determine the 'type' of this file - 
				//e.g. is it a directory? or a file?
				/*
				It's a more complicated question than I thought.
				#1) Amazon, for example, won't have indexes - but will have appropriate things I can do instead. that's fine.
				#2) A homepage that doesn't list its subsidiary pages _won't_ be considered a directory. Hell, no root page
					will ever be considered a directory - because there's nothing you can request _without_ the slash.
				#3) I think that answers my question. Root dir is always a dir. Otherwise do your test.
				*/
				
				//brintf("Headers we are working with: %s\n",header);
				int httpstatus=fetchstatus(header);
				brintf("Status: %d\n",httpstatus);
				if(httpstatus==301 || httpstatus==302 || httpstatus==303 || httpstatus==307) {
					//http redirect, means either DIRECTORY or SYMLINK
					//start out by keeping it simple - if it ends in a
					//slash, it's a directory.
					if(strlen(results)>0 && results[strlen(results)-1]=='/') {
						brintf("Uhm, %s is a directory?\n",results);
						//e.g. - user has been redirected to a directory, then:
						stbuf->st_mode = S_IFDIR | 0755; //I am a a directory?
						stbuf->st_nlink = 1; //a lie, to allow find(1) to work.		
						insertdircache(path,stbuf); //cache this please
					} else {
						//ZERO caching attempts, to start with...
						stbuf->st_mode = S_IFLNK;
						stbuf->st_nlink = 1;
					}
				} else {
					brintf("%s is a file, strlen %d, status: %d\n",results,(int)strlen(results),fetchstatus(header));
					char length[32];
					char date[32];
					stbuf->st_mode = S_IFREG | 0755;
					//brintf("Pre-mangulation headers: %s\n",header);
					fetchheader(header,"last-modified",date,32);
					fetchheader(header,"content-length",length,32);
					//brintf("Post-mangulation headers is: %s\n",header+1);
					//brintf("Post-mangulation headers is: %s\n",header);
					//brintf("BTW, date I'm trying to format: %s\n",date);
					brintf("WEIRD...date: %s, length: %s\n",date,length);
					stbuf->st_mtime = parsedate(date);
					stbuf->st_size = atoi(length);
					if(strlen(length)==0) {
						//zero byte file, 'read' will *never* get called on it, so it will *never* cache properly.
						int writestatus=fwrite(header,1,strlen(header),headerfile);
						
						brintf("STatus of your write is: %d\n",writestatus);
					}
				}
			} else if(statstatus==1) {
				//SOMETHING was statted before, 
				//and by virtue of the 'else', 
				//NO Bytes were fetched...
				//let's use that STAT
				brintf("statstatus is one, try to returns stale cache info\n");
				if(cachestat.st_mode & S_IFDIR) {
					stbuf->st_mode = S_IFDIR | 0755;
					stbuf->st_nlink = 1;
					if(headerfile) fclose(headerfile);
					if(cachefile) fclose(cachefile);
					return 0;
				}
				if(cachestat.st_mode & S_IFREG) {
					stbuf->st_mode = S_IFREG | 0755;
					stbuf->st_mtime = cachestat.st_mtime;
					stbuf->st_size = cachestat.st_size;
					if(headerfile) fclose(headerfile);
					if(cachefile) fclose(cachefile);
					return 0;
				}
			} else {
				brintf("no files stat'ed, no internet to grab data from, i give up. no such file.\n");
				return -ENOENT;
			}
			if(headerfile)
				fclose(headerfile);
			else
				brintf("TRY TO CLOSE NO HEADERFILE!\n");
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
	char hostname[80];
	char pathpart[1024];
	char location[4096]="";
	char header[HEADERLEN];
	int st=0;
	pathparse(path,hostname,pathpart,80,1024);
	
	//we SHOULD get the metadata cachefile and check the Date header on it,
	//and if that's recent enough, use the symlink that's already there as the data cachefile
	
	
	st=webfetch(path,header,HEADERLEN,"HEAD","");
	if(st>0) {
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
		return 0;
	} else {
		//no data on web, internet is unhappy
		int linklen=readlink(path+1,buf,bufsize);
		if(linklen) {
			buf[linklen]='\0';
			return 0;
		} else {
			brintf("Can't read link %s: %s\n",path+1,strerror(errno));
			return -ENOENT;
		}
	}	
}

static int
crest_open(const char *path, struct fuse_file_info *fi)
{
//	if (strcmp(path, file_path) != 0) { /* We only recognize one file. */
//		return -ENOENT;
//	}
	struct stat st;
	return 0;
	if(crest_getattr(path,&st) == -ENOENT) {
		return -ENOENT;
	}
    if ((fi->flags & O_ACCMODE) != O_RDONLY) { /* Only reading allowed. */
        return -EACCES;
    }

    return 0;
}

void touch(char *path)
{
	FILE *f;
	char headers[65535];
	f=get_cacheelem(path,headers,65535);
	int fw=fwrite("",0,1,f);
	int fu=utime(path,0);
	brintf("I'mo touch something: %s, int: %d, fu: %d\n",path,fw,fu);
	fclose(f);
}

#define DIRBUFFER	1*1024*1024
// 1 MB

#define TOOMANYFILES 200

static int
crest_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
              off_t offset, struct fuse_file_info *fi)
{
	char dircachefile[1024];
	FILE *dirfile=0;
	char headers[8196];
	char date[80];

	//should do a directory listing in the domains directory (the first one)
	// but let's do that later.
	
	if(strcmp(path,"/")==0) {
		brintf("PATH IS /!!!!!\n");
		DIR *mydir;
		struct dirent *dp;
		int id=1;
		
		mydir=opendir(".");
		while((dp=readdir(mydir))!=0) {
			brintf("Potential dir is: %s, id: %d (vs offset: %d)\n",dp->d_name,id,offset);
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
	
	strlcpy(dircachefile,path,1024); 
	strlcat(dircachefile,DIRCACHEFILE,1024);

	brintf("Dircachefile: %s\n",dircachefile);	
	dirfile=get_cacheelem(dircachefile,headers,8196);
	fetchheader(headers,"Date",date,80);
	//brintf("So as to mention, the headers IS: %s\n",headers);
	
///*	if(offset<++failcounter) */filler(buf, ".", NULL, 0/*failcounter*/); /* Current directory (.)  */
///*	if(offset<++failcounter) */filler(buf, "..", NULL, 0/*failcounter*/);          /* Parent directory (..)  */
//	return 0;
	brintf("okay, sanity check time. how's our cache file pointer looking right now?\n");
	char nutjob[9]="";
	fread(nutjob,8,1,dirfile);
	nutjob[8]='\0';
	brintf("initial results are: %s\n",nutjob);
	rewind(dirfile);
	if(time(0) - parsedate(date) > MAXCACHEAGE) {
		char slashpath[1024];
		int bytes=0;
		brintf("IN dirlist, old cache...\n");

		strncpy(slashpath,path,1024);
		strlcat(slashpath,"/",1024);
		brintf("slashpath: %s\n",slashpath);
		bytes=webfetch(slashpath,dirbuffer,DIRBUFFER,"GET","");

		if(bytes>0) {
			if(fetchstatus(dirbuffer)<200 || fetchstatus(dirbuffer)>299) {
				// prepare for 304 We don't do etags for directories.
				free(dirbuffer);
				return -ENOENT;
			}
		
			fwrite(dirbuffer,1,bytes,dirfile);
			rewind(dirfile);
		} else {
			brintf("No bytes retrieved from Webernet...\n");
		}
	}
	if(strlen(dirbuffer)==0) {
		char *filecursor=dirbuffer;
		brintf("Reading from cache because strlen of dirbuffer is 0. Dirfile: %p\n",dirfile);
		brintf("More about our dirfile friend: feof: %d, and ferror? %d (strerror says: %s)\n",feof(dirfile),ferror(dirfile),strerror(errno));
		brintf("Ftell syas: %s\n",ftell(dirfile));
		while(!feof(dirfile) && !ferror(dirfile)) {
			int bytes=fread(filecursor,1,8196,dirfile);
			brintf("I just read %d bytes\n",bytes);
			if(bytes<=0) {
				brintf("Bad bytes? %d\n feof: %d, ferror: %d, explanation: %s\n",bytes,feof(dirfile),ferror(dirfile),strerror(errno));
			}
			filecursor=(filecursor+bytes);
		}
	}
	fclose(dirfile);
	//if we haven't managed to fill the dirbuffer, fill it now
	int status=regcomp(&re,"<a[^>]href=['\"]([^'\"]+)['\"][^>]*>([^<]+)</a>",REG_EXTENDED|REG_ICASE); //this can be globalized for performance I think.
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
	char *filebuffer=0;
	//char *headerend=0;
	char length[32];
	FILE *cachefile;
	FILE *headerfile;
	char cacheheaders[32767];
	char etag[32];
	char *headerend=0;
	
	char date[32];
	int file_size=0;
	
	int returnedbytes=0;

	get_cachefiles(path,&headerfile,&cachefile,cacheheaders,32767);
	if(headerfile==0 || cachefile==0) {
		brintf("get_cachefiles didn't work?!\n");
		exit(57);
	}
	fetchheader(cacheheaders,"date",date,32); //this was the last time the content was fetched (or 0, which is 1/1/1970)
	fetchheader(cacheheaders,"etag",etag,32);
	//e-tag? last-modified-since?
	
	brintf("CACHE HEADERS ARE: %s\n",cacheheaders);
	
	brintf("I'm getting a file %s for which the cache date is: %s\n",path,date);

	if(time(0) - parsedate(date) > MAXCACHEAGE) {
		filebuffer=malloc(FILEMAX);
		brintf("OLD cachefile!\n");
		int totalbytes=webfetch(path,filebuffer,FILEMAX,"GET",etag);
		if(fetchstatus(filebuffer)==200) {
			int writestat=0;
			int hdrstat=0;
			int hdrbytes=0;
			headerend=strstr(filebuffer,"\r\n\r\n");
			int contentbytes=0;
			char lengthbuffer[32];
			fetchheader(filebuffer,"Content-length",lengthbuffer,32);
			contentbytes=atoi(lengthbuffer);
			if(headerend==0) {
				//invalid HTML?
				brintf("Bad HTML (no headers)\n");
				exit(-1);
			}
			hdrbytes=headerend-filebuffer+4;
			brintf("HEADER BYTES CALCULATED TO BE: %d\n",hdrbytes);
			brintf("cachefiles:headerfile: %p, cachefile: %p\n",headerfile,cachefile);
			while(hdrstat<hdrbytes) {
				hdrstat+=write(fileno(headerfile),filebuffer+hdrstat,hdrbytes-hdrstat);
			}
			brintf("HEADER I WROTE: %d bytes!\n",hdrstat);
			//re-fill 'cacheheaders'
			memcpy(cacheheaders,filebuffer,hdrbytes);
			cacheheaders[hdrbytes]='\0';
			
			headerend+=4;
			while(writestat<contentbytes) {
				writestat+=write(fileno(cachefile),headerend+writestat,contentbytes-writestat);
			}
			if(writestat < contentbytes) {
				brintf("funky mismatch business writestat: %d, contentbytes: %d, totalbyes: %d, filemax: %d\n",writestat,contentbytes,totalbytes,FILEMAX);
			}
			rewind(headerfile);
			rewind(cachefile);
		}
		if(fetchstatus(filebuffer)==304) {
			//write the new header
			//because we don't want to check the server AGAIN
			//next time until at least MAXCACHEAGE
			fwrite(filebuffer,1,strlen(filebuffer)+1,headerfile);
			ftruncate(fileno(headerfile),strlen(filebuffer)+1);
			rewind(headerfile);
		}
		if(fetchstatus(filebuffer)>399 && fetchstatus(filebuffer)<600) {
			free(filebuffer);
			if(headerfile) fclose(headerfile);
			if(cachefile) fclose(cachefile);
			return -ENOENT;
		}
		free(filebuffer);
	}
	
	//okay, reiterate where we are at:
	
	//we either have a good cache, or we refreshed it and made it good
	//otherwise, we got no file
	
	//if the internet was down and the server didn't tell us definitively
	//that there was no file, how can we know?
	//well, the cachefile will be empty, and so will be the header?
	
	fetchheader(cacheheaders,"content-length",length,32);
	if(strlen(length)==0) {
		//no content-length header;
		//either this is 304'ed content or we ahve
		//an unprimed cache. Try to go by cachefile filesize
		struct stat cachestat;
		
		if(fstat(fileno(cachefile),&cachestat)) {
			brintf("Couldn't even stat content file...assuming nonexistent\n");
			if(headerfile) fclose(headerfile);
			if(cachefile) fclose(cachefile);
			return -ENOENT;
		}
		file_size=cachestat.st_size;
	} else {
		file_size=atoi(length);
	}
	
	if (offset >= file_size) { /* Trying to read past the end of file. */
		if(headerfile) fclose(headerfile);
		if(cachefile) fclose(cachefile);
		return 0;
	}

	if (offset + size > file_size) { /* Trim the read to the file size. */
		size = file_size - offset;
	}
	brintf("I'm about to seek to %d\n",offset);
	if(offset!=0) {
		if(fseek(cachefile,offset,SEEK_SET)==-1) {
			//cannot seek to point in file!
			brintf("Seek off end of file, dying: %s.\n",strerror(errno));
			exit(57);
		}
	}
	brintf("Going to read %d bytes...\n",size);
	returnedbytes = fread(buf,1,size,cachefile);
	fclose(headerfile);
	fclose(cachefile);
	return returnedbytes;
	
	
	
	
	
	
	
	
	
	
#if 0	
	if(headerend==0 || fetchstatus(filebuffer)==304) {
		//this would only happen if nothing has been so far loaded
		//or if the Etags match OK, meaning our content is 'ok'
		filecursor=filebuffer;
		brintf("VALID CACHEFILE! ...or internet is down and we have no choice...or etags say its ok (status: %d) \n",fetchstatus(filebuffer));
		while(!feof(headerfile)) {
			char buffer[8192];
			int bytesread=0;
			bytesread=fread(buffer,1,8192,headerfile);
			//brintf("Iterazzione! bytes: %d, buffer: %s\n",bytesread,buffer);
			//brintf("\nENDA DE BUFFERO!\n");
			if(bytesread>0) {
				memcpy(filecursor,buffer,bytesread);
				filecursor=(filecursor+bytesread);
			} else {
				brintf("Error reading from header: %s\n",strerror(errno));
				break;
			}
		}
		if(filecursor!=filebuffer) {
			//we read SOMETHING for the cache headers
			//remove the trailing NUL at the end of the headers, pls
			//back up one char.
			filecursor-=1;
		}
		while(!feof(cachefile)) {
			char buffer[8192];
			int bytesread=0;
			bytesread=fread(buffer,1,8192,cachefile);
			if(bytesread==-1) {
				brintf("Error reading from cache file: %s\n",strerror(errno));
				//this shouldn't happen; if the file doesn't exist, we
				//shouldn't even get here.
				exit(14);
			}
			//brintf("Iterazzione! bytes: %d, buffer: %s\n",bytesread,buffer);
			//brintf("\nENDA DE BUFFERO!\n");
			brintf("Reading %d bytes and copying them to filebuffer\n",bytesread);
			memcpy(filecursor,buffer,bytesread);
			filecursor=(filecursor+bytesread);
		}
	}
	fclose(headerfile);
	fclose(cachefile);
	//brintf("GODDAMMIT! HERe's filebuffer you dummy! \n%s\nBLEAH",filebuffer);
	fetchheader(filebuffer,"content-length",length,32);
	if(strcmp(length,"")!=0) {
		file_size=atoi(length);
		headerend=strstr(filebuffer,"\r\n\r\n");
		//headerend=filebuffer;
		headerend+=4;
	} else {
		char *rnrn=strstr(filebuffer,"\r\n\r\n");
		//this could be un-primed cache (cache without header info)
		//or not-modified header info (HTTP status 304)
		brintf("Empty length detected - guessing via cache data\n");
		file_size=cachesize.st_size;
		if(rnrn) {
			brintf("Found end-of-headers, using that to determine start of data\n");
			headerend=rnrn+4;
		} else {
			//no end of headers, must be unprimed cache?
			brintf("No end-of-headers. Assuming all I have is data.\n");
			headerend=filebuffer;
		}
	}
	brintf("File size: %d\n",file_size);
	//brintf("First char: %c, last char??: %c\n",headerend[0],headerend[file_size]);
	//brintf("Header end is: %s\n",headerend);

	if (offset >= file_size) { /* Trying to read past the end of file. */
		free(filebuffer);
		return 0;
	}

	if (offset + size > file_size) { /* Trim the read to the file size. */
		size = file_size - offset;
	}

	memcpy(buf, headerend + offset, size); /* Provide the content. */
	free(filebuffer);
	return size;
#endif
}

int	cachedir = 0;

static void *
crest_init(struct fuse_conn_info *conn)
{
	fchdir(cachedir);
	close(cachedir);
	return 0;
}

static struct fuse_operations crest_filesystem_operations = {
    .init    = crest_init,    /* mostly to keep access to cache     */
    .getattr = crest_getattr, /* To provide size, permissions, etc. */
    .readlink= crest_readlink,/* to handle funny server redirects */
    .open    = crest_open,    /* To enforce read-only access.       */
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
	
	webfetch("/desk.nu/braydix",buf,DIRBUFFER,"HEAD","");
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
