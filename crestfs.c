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
#define MAXCACHEAGE		3600*2
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
webfetch(const char *path,char *buffer,int maxlength, const char *verb)
{
	int sockfd, numbytes;  
	struct addrinfo hints, *servinfo, *p;
	int rv;
	char hostname[HOSTLEN];
	char pathstub[PATHLEN];
	char *reqstr=0;
	int bytessofar=0;
//	char *origbuffer=buffer;
//	struct stat flagstat;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	
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
		close(sockfd);
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
	asprintf(&reqstr,"%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: CREST-fs/0.2\r\n\r\n",verb,pathstub,hostname);
	brintf("REQUEST: %s\n",reqstr);
	send(sockfd,reqstr,strlen(reqstr),0);
	free(reqstr);
	

	while ((numbytes = recv(sockfd, buffer, FETCHBLOCK-1, 0)) >0 && bytessofar <= maxlength) {
		//keep going
		bytessofar+=numbytes;
		buffer+=numbytes;
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
		cachelen=fread(headers,1,65535,cachefile);
		brintf("Reading into cache... %d bytes\n",cachelen);
	}
	if(headers) {
		headers[cachelen]='\0'; //should also handle the case where the file didn't exist, and nothing was read.
	}
	rewind(cachefile);
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
			if(stat(path+1,&cachestat)==0) {
				char date[80];
				
				statstatus=1;
				brintf("Stat successful for getattr, let's see if it's new enough...\n");
				if(cachestat.st_mode & S_IFDIR ) {
					//now check the dircache file
					brintf("Cache file is a directory file, statting further...\n");
					strlcpy(dircachepath,path+1,1024);
					strlcat(dircachepath,DIRCACHEFILE,1024);
					if(stat(dircachepath,&cachestat)==0 && cachestat.st_mtime>time(0)-MAXCACHEAGE) {
						//recent enough cache, don't bother with the HEAD
						stbuf->st_mode = S_IFDIR | 0755;
						stbuf->st_nlink = 1;
						///you got this from the cached files
						//don't need to cache this in the fast 
						//dircache
						return 0;
					} else {
						brintf("old dircache file %s -refetching:/\n",dircachepath);
					}
				} else if (cachestat.st_mode & S_IFREG) {
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
					//brintf("Dunno WHUT kind a file this is... %x\n",cachestat.st_mode);
					//exit(99);
					return -ENOENT; //no special files allowed
				}
			}
			if(strlen(header)==0) {
				st=webfetch(path,header,HEADERLEN,"HEAD");
			}
			
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
				brintf("Status: %d\n",fetchstatus(header));
				if(fetchstatus(header)>=300 && fetchstatus(header)<400 
				   && strlen(results)>0 && results[strlen(results)-1]=='/') {
					brintf("Uhm, %s is a directory?\n",results);
					//e.g. - user has been redirected to a directory, then:
					stbuf->st_mode = S_IFDIR | 0755; //I am a a directory?
					stbuf->st_nlink = 1; //a lie, to allow find(1) to work.		
					insertdircache(path,stbuf); //cache this please
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
	int fw=fwrite("0",1,1,f);
	int fu=utime(path,0);
	brintf("I'mo touch something: %s, int: %d, fu: %d\n",path,fw,fu);
	fclose(f);
}

#define DIRBUFFER	1*1024*1024
// 1 MB

#define TOOMANYFILES 200

#define NOWEBS	-2147483647

int
crest_readdir_from_web(const char *path, void *buf, fuse_fill_dir_t filler,
              off_t offset, struct fuse_file_info *fi,char *dircachefile)
{
	char *dirbuffer=0;

	regex_t re;
	regmatch_t rm[3];
	int status=0;
	int weboffset=0;
	
	char slashpath[1024];

	strncpy(slashpath,path,1024);
	strlcat(slashpath,"/",1024);

	memset(rm,0,sizeof(rm));

	brintf("SKIP SKIP SKIP CACHE - DO GET STUFF INSTAED!!!\n");
	dirbuffer=malloc(DIRBUFFER);
	
	webfetch(slashpath,dirbuffer,DIRBUFFER,"GET");
	if(strlen(dirbuffer)==0) {
		free(dirbuffer);
		return NOWEBS;
	}
	//we don't currently handle redirects (30x)
	//nor (100 Continue)
	//otherwise this would just say > 399 (would mean 400 or 500 errors)
	if(fetchstatus(dirbuffer)<200 || fetchstatus(dirbuffer)>299) {
		return -ENOENT;
	}
	//brintf("Fetchd: %s\n",dirbuffer);
	status=regcomp(&re,"<a[^>]href=['\"]([^'\"]+)['\"][^>]*>([^<]+)</a>",REG_EXTENDED|REG_ICASE); //this can be globalized for performance I think.
	if(status!=0) {
		char error[80];
		regerror(status,&re,error,80);
		brintf("ERROR COMPILING REGEX: %s\n",error);
		//this is systemic failure, unmount and die.
		exit(-1);
	}
    filler(buf, ".", NULL, 0);           /* Current directory (.)  */
    filler(buf, "..", NULL, 0);          /* Parent directory (..)  */
	int failcounter=0;
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
				char chopslash[255];
				char elempath[1024];
				struct stat nodestat;

				brintf("ELEMENT: %s\n",hrefname);
				
				strlcpy(elempath,path,1024); // TOTAL GUESS! This looks like it's WRONG FIXME
				strlcat(elempath,"/",1024); //then slash, then...
				
				strlcpy(chopslash,hrefname,255);
				if(chopslash[strlen(chopslash)-1]=='/') {
					int dirmak=0;
					chopslash[strlen(chopslash)-1]='\0';
					strlcat(elempath,chopslash,1024);
					
					brintf("DIR ELEM PATH: %s\n",elempath);
					
					if(stat(elempath,&nodestat)==0) {
						if (nodestat.st_mode & S_IFREG) {
							brintf("Cache sub-node already exists, but it exists as a file when it should be a directory, so I'm going to delete it\n");
							unlink(elempath);
						}
						if (nodestat.st_mode & S_IFDIR) {
							brintf("Cache sub-node already exists, as a directory. going to next node!\n");
							continue;
						}
					}				
					brintf("I want to try to make directory: %s\n",elempath);
					dirmak=mkdir(elempath+1,0777);
					if(dirmak==-1 && errno!= EEXIST ) {
						brintf("Fail to make directory for caching purposes! For shame! For shame!!!! (Reason: %d), means: %s\n",errno,strerror(errno));
						brintf("Try to recursively make directory now");
						redirmake(elempath+1);
						brintf("Recursive directory mades!\n");
					}
				} else {
					strlcat(elempath,chopslash,1024);
					brintf("Making a file for directory-caching purposes: %s\n",elempath);
					if(stat(elempath,&nodestat)==0) {
						if(nodestat.st_mode & S_IFREG) {
							brintf("File cache-node already exists, skipping...\n");
							continue;
						}
						if(nodestat.st_mode & S_IFDIR) {
							brintf("CRAP. sub-cache node exists, but is a directory, not a file. I coudl try to unlink it...\n");
							if(rmdir(elempath)!=0) {
								brintf("Unable to remove directory - probably because it's not empty: %s\n",elempath);
								exit(189);
							}
						}
					}
					touch(elempath);
				}
				filler(buf, chopslash, NULL, 0);
			}
		}
		//brintf("staus; %d, 0: %d, 1:%d, 2: %d, href=%s, link=%s\n",status,rm[0].rm_so,rm[1].rm_so,rm[2].rm_so,hrefname,linkname);
		//filler?
		//filler(buf,rm[])
	}
	if(failcounter>=TOOMANYFILES) {
		brintf("Fail due to toomany\n");
	}
	free(dirbuffer);
	brintf("Now I make the dircache: %s?\n",dircachefile); //LEADING SLAHSES OK!!!!
	touch(dircachefile);
	return 0; //success? or does that mean fail?
}

static int
crest_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
              off_t offset, struct fuse_file_info *fi)
{
	char dircachefile[1024];
	struct stat statbuf;
	int statresult=-1;
	
	DIR *mydir;
	struct dirent *dp=0;
	char dircacheskip[1024];
	char metacacheskip[1024];

	
	strlcpy(dircachefile,path,1024); 
	strlcat(dircachefile,DIRCACHEFILE,1024);
	
	statresult=stat(dircachefile+1,&statbuf);

	brintf("readdir: %s ",path);
	brintf("Statresult: %d, ",statresult);
	brintf("st_mode | S_IFREG: %d, ",statbuf.st_mode & S_IFREG);
	brintf("now: %ld, ",time(0));
	brintf("mtime: %ld, ",statbuf.st_mtime);
	brintf("FULLCOMP: %d\n",(time(0)-statbuf.st_mtime <= MAXCACHEAGE));
	
	brintf("OFFSET? %ld\n",offset);

	//if (strcmp(path, "/") == 0 || (statresult==0 && (statbuf.st_mode & S_IFREG) && (time(0)-statbuf.st_mtime <= MAXCACHEAGE))) {
	if (!(strcmp(path, "/") == 0 || (statresult==0 && (statbuf.st_mode & S_IFREG) && (time(0)-statbuf.st_mtime <= MAXCACHEAGE)))) {
		int resultstatus=crest_readdir_from_web(path,buf,filler,offset,fi,dircachefile);
		//getting a '401 - gone' is NOT the same as the web being down
		//the server says the page DOES NOT EXIST. the server MUST
		//be respected.
		if(resultstatus==0 || resultstatus==-ENOENT) {
			return resultstatus;
		}
	}

	strlcpy(dircacheskip,DIRCACHEFILE,1024);//CONTAINS the leading slash! Gotta make sure to skip it when you use it.
	strlcpy(metacacheskip,METAPREPEND,1024);
	brintf("GENERATING DIRECTORY LISTING FROM CACHE!!!\n");

	if(strcmp(path,"/")==0) {
		mydir=opendir(".");
	} else {
		mydir=opendir(path+1);
	}
	//seekdir(mydir,offset);

	//either we are exactly JUST the ROOT  dir OR we have a cache to read from
	//in either case, we want to walk through what we have cached and list that.
	while((dp=readdir(mydir))!=0) {
		struct stat entry;
		memset(&entry,0,sizeof(entry));
		brintf("entry: '%s', vs. '%s' or '%s'\n",dp->d_name,dircacheskip+1,metacacheskip+1);
		if(strcmp(dp->d_name,dircacheskip+1)==0 || strcmp(dp->d_name,metacacheskip+1)==0) { //need to skip the leading '/' in DIRCACHEFILE
			//we don't want to display the DIRCACHEFILE's as if they are valid contents - they aren't!
			//they're just an internal marker. This also becomes a good way to tell the difference between
			//a cache directory and a live-mount directory - you can only see the DIRCACHEFILE's in the
			//cache directory, they won't show up in the live directory
			brintf("skipping my own crap in the directory\n");
			continue;
		}
		if(dp->d_type==DT_REG || dp->d_type==DT_DIR) {
			brintf("Directory Entry being added: '%s'\n",dp->d_name);
			entry.st_ino=dp->d_ino;
			entry.st_mode = dp->d_type << 12;
			if(filler(buf, dp->d_name, NULL, 0)) 
				break; 
		}
	}
	/*filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	filler(buf, "Doodie", NULL, 0);
	filler(buf, "DooDoo", NULL, 0);
	return 0; */

	closedir(mydir);
	return 0;
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
	char cacheheaders[65535];
	char *headerend=0;
	
	char date[32];
	int file_size=0;
	void *filecursor;
	get_cachefiles(path,&headerfile,&cachefile,cacheheaders,65535);
	fetchheader(cacheheaders,"date",date,32); //this was the last time the content was fetched (or 0, which is 1/1/1970)
	//e-tag? last-modified-since?
	
	brintf("CACHE HEADERS ARE: %s\n",cacheheaders);
	
	brintf("I'm getting a file %s for which the cache date is: %s\n",path,date);

	filebuffer=malloc(FILEMAX);
	if(time(0) - parsedate(date) > MAXCACHEAGE) {
		brintf("EEENVALEED cachefile!\n");
		int totalbytes=webfetch(path,filebuffer,FILEMAX,"GET");
		if(fetchstatus(filebuffer)==200) {
			int writestat=0;
			int hdrstat=0;
			int hdrbytes=0;
			char *headerend=strstr(filebuffer,"\r\n\r\n");
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
			while(hdrstat<hdrbytes) {
				hdrstat+=write(fileno(headerfile),filebuffer+hdrstat,hdrbytes-hdrstat);
			}
			brintf("HEADER I WROTE: %d bytes!\n",hdrstat);
			headerend+=4;
			while(writestat<contentbytes) {
				writestat+=write(fileno(cachefile),headerend+writestat,contentbytes-writestat);
			}
			if(writestat < totalbytes) {
				brintf("funky mismatch business writestat: %d, filemax: %d\n",writestat,FILEMAX);
			}
		}
		if(fetchstatus(filebuffer)>399 &&fetchstatus(filebuffer)<600) {
			return -ENOENT;
		}
	}
	if(headerend==0) { //this would only happen if nothing has been so far loaded
		filecursor=filebuffer;
		brintf("VALID CACHEFILE! ...or internet is down and we have no choice \n");
		while(!feof(headerfile)) {
			char buffer[8192];
			int bytesread=0;
			bytesread=fread(buffer,1,8192,headerfile);
			//brintf("Iterazzione! bytes: %d, buffer: %s\n",bytesread,buffer);
			//brintf("\nENDA DE BUFFERO!\n");
			memcpy(filecursor,buffer,bytesread);
			filecursor+=bytesread;
		}
		while(!feof(cachefile)) {
			char buffer[8192];
			int bytesread=0;
			bytesread=fread(buffer,1,8192,cachefile);
			//brintf("Iterazzione! bytes: %d, buffer: %s\n",bytesread,buffer);
			//brintf("\nENDA DE BUFFERO!\n");
			memcpy(filecursor,buffer,bytesread);
			filecursor+=bytesread;
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
		brintf("Empty length detected - guessing via data\n");
		file_size=filecursor-(void *)filebuffer;
		headerend=filebuffer;
	}
	brintf("File size: %d\n",file_size);
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
	.init	 = crest_init,	  /* mostly to keep access to cache		*/
    .getattr = crest_getattr, /* To provide size, permissions, etc. */
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
	
	webfetch("/desk.nu/braydix",buf,DIRBUFFER,"HEAD");
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
	
	//INVOKE AS: 	./crestfs /tmp/doodle -r -s -d -o nolocalcaches
	
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
