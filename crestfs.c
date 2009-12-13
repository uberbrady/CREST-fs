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
#include <sys/types.h>
#include <netinet/in.h>

#include <dirent.h>

#include <regex.h>

#define FUSE_USE_VERSION  26
#include <fuse.h>
#include <sys/time.h>
//#include <xlocale.h>

#include <strings.h>

#include "resource.h"
#include "common.h"

//global variables (another one is down there but it's use is jsut there.
char rootdir[1024]="";
int maxcacheage=-1;


//we may want this to be configurable in future - a command line option perhaps
/*************************/

//retrieve the value of an HTTP header
//be warned the headers may not be a nul-terminated string
//so don't do stupid stuff like strlen(headers) because it 
//may not work
/******************* END UTILITY FUNCTIONS< BEGIN ACTUALLY DOING OF STUFF!!!!! *********************/

#define METALEN 1024

#define HEADERLEN 65535

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

	//brintf("Weboffset: %d\n",iter->weboffset);
	while(regexec(&iter->re,directoryfile+iter->weboffset,3,iter->rm,0)==0) {
		reanswer(directoryfile+iter->weboffset,&iter->rm[1],hrefname,255);
		reanswer(directoryfile+iter->weboffset,&iter->rm[2],linkname,255);
		iter->weboffset+=iter->rm[0].rm_eo;

		//brintf("href: %s link: %s\n",hrefname,linkname);
		if(strcmp(hrefname,linkname)==0) {
			iter->filecounter++;
			//brintf("ELEMENT: %s\n",hrefname);
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
			/**** ALL WORK IS DONE WITH get_resource! ****/
			
			cachefile=get_resource(path,header,HEADERLEN,&isdirectory,"HEAD","getattr");
			
			/*** THAT DID A LOT! ****/
			
			if(isdirectory) {
				brintf("Getattr for directory mode detected.\n");
				stbuf->st_mode = S_IFDIR | 0755;
				stbuf->st_nlink = 1;
			} else {
				int httpstatus=fetchstatus(header);
				char date[80];
				char length[80]="";

				//brintf("Status: %d\n",httpstatus);
				//brintf("Header you got was: %s\n",header);
				switch(httpstatus) {
					case 200:
					case 304:
					//brintf("It is a file\n");
					stbuf->st_mode = S_IFREG | 0755;
					//brintf("Pre-mangulation headers: %s\n",header);
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
					//brintf("WEIRD...date: %s, length: %d\n",date,(int)stbuf->st_size);
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
	
	cachefile=get_resource(path,header,HEADERLEN,&is_directory,"HEAD","readlink");
	
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
	
	if(cachefile) {
		brintf("Freaky, got a valid cachefile for a symlink?!\n");
		fclose(cachefile);
	}
	struct stat st;
	int stres=lstat(path+1,&st);
	if(stres==0 && S_ISLNK(st.st_mode)) {
		char linkbuf[1024]="";
		int linklen=readlink(path+1,linkbuf,1024);
		if(linklen>1023) {
			brintf("Too long of a link :(\n");
			exit(54);
		}
		linkbuf[linklen+1]='\0';
		if(stres==0 && strncmp(linkbuf,buf,1024)==0) {
			//brintf("Perfect match on symlink! Not doing anything\n");
			return 0;
		}
	}
	brintf("I will unlink: %s, and point it to: %s\n",path+1,buf);
	int unlinkstat=unlink(path+1);
	int linkstat=symlink(buf,path+1);
	brintf("Going to return link path as: %s (unlink status: %d, link status: %d)\n",buf,unlinkstat,linkstat);
	return 0;
}

static int
crest_open(const char *path, struct fuse_file_info *fi)
{
	FILE *rsrc=0;
	char headers[4096];
	
	int is_directory=-1;
	if ((fi->flags & O_ACCMODE) != O_RDONLY) { /* Only reading allowed. */
		return -EACCES;
	}
	rsrc=get_resource(path,headers,4096,&is_directory,"GET","open");
	if(is_directory) {
		if(rsrc) {
			fclose(rsrc);
		}
		return -EISDIR;
	}
	if(fetchstatus(headers)==404) {
		if(rsrc) {
			brintf("Wow - weird, got a 404 but a valid resource? I guess\n");
			fclose(rsrc);
		}
		return -ENOENT;
	}
	if(rsrc) {
		//brintf("Going to set filehandle to POINTER: %p\n",rsrc);
		fi->fh=(long int)rsrc;
		return 0;
	} else {
		brintf("Uh, no resource retrieved? Setting fh to zero?\n");
		fi->fh=0;
		return 0;
	}
}

static int
crest_release(const char *path, struct fuse_file_info *fi)
{
	brintf("Closing filehandle %p: for file: %s\n",(FILE *)(unsigned int)fi->fh,path);
	if(fi->fh) {
		return fclose((FILE *)(unsigned int)fi->fh);
	} else {
		return 0;
	}
}

static int
crest_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
              off_t offset, struct fuse_file_info *fi)
{
	FILE *dirfile=0;
	int is_directory=-1;
	if(fi) {
		//brintf("Well, dunno why this is, but we have file info: %p, filehandle: %d\n",fi,(int)fi->fh);
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

	char *dirbuffer=calloc(DIRBUFFER,1);
	if(dirbuffer==0) {
		brintf("Great, can't malloc our dirbuffer. baililng.\n");
		exit(19);
	}
	dirbuffer[0]='\0';
	
	regex_t re;
	regmatch_t rm[3];
	
	memset(rm,0,sizeof(rm));
	
	dirfile=get_resource(path,0,0,&is_directory,"GET","readdir");
	if(!is_directory) {
		if(dirfile) {
			fclose(dirfile);
		}
		free(dirbuffer);
		return -ENOTDIR;
	}
	if(dirfile==0) {
		//got no directory listing file, this directory doesn't even exist.
		free(dirbuffer);
		return -ENOENT;
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
		//brintf("Weboffset: %d\n",weboffset);
		status=regexec(&re,dirbuffer+weboffset,3,rm,0); // ???
		if(status==0) {
			reanswer(dirbuffer+weboffset,&rm[1],hrefname,255);
			reanswer(dirbuffer+weboffset,&rm[2],linkname,255);

			//brintf("Href? %s\n",hrefname);
			//brintf("Link %s\n",linkname);
			//brintf("href: %s link: %s\n",hrefname,linkname);
			if(strcmp(hrefname,linkname)==0) {
				//brintf("ELEMENT: %s\n",hrefname);
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
	//brintf("while loop - complete\n");
	if(failcounter>=TOOMANYFILES) {
		brintf("Fail due to toomany\n");
	}
	free(dirbuffer);
	regfree(&re);
	return 0; //success? or does that mean fail?
}

#define FILEMAX 64*1024*1024

static int
crest_read(const char *path, char *buf, size_t size, off_t offset,
           struct fuse_file_info *fi)
{
	FILE *cachefile;
	
	if(fi) {
		//brintf("Good filehandle for path: %s, pointer:%p\n",path,(FILE *)(unsigned int)fi->fh);
	} else {
		brintf("BAD FILE*INFO* IS ZERO for path: %s\n",path);
		brintf("File was not properly opened?\n");
		return -EIO;
	}
	cachefile=(FILE *)(unsigned int)fi->fh;
	
	if(cachefile) {
		if(fseek(cachefile,offset,SEEK_SET)==0) {
			return fread(buf,1,size,cachefile);
		} else {
			//fail to seek - what'ssat mean?
			return 0;
		}
	} else {
		return -EIO;
	}
}

int	cachedir = 0;

static void *
crest_init(struct fuse_conn_info *conn __attribute__((unused)) )
{
	//brintf("I'm not intending to do anything with this connection: %p\n",conn);
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

#define HOSTLEN 64
#define PATHLEN 1024


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

#if defined(MEMTEST)
#include <mcheck.h>
#endif

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
	
	if(argc<4) {
		brintf("Not right number of args, you gave me %d\n",argc);
		brintf("Usage: %s mountdir cachedir maxcacheage [options]\n",argv[0]);
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
	cachedir= open(argv[2], O_RDONLY); //second parameter is cachedir
	if(cachedir==-1) {
		brintf("%d no open cachedir\n",cachedir);
		exit(2);
	}
	maxcacheage=atoi(argv[3]);
	if(maxcacheage<=0) {
		brintf("%d is not a valid max cache age\n",maxcacheage);
		exit(3);
	}
	for(i=4;i<argc;i++) {
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
	#if defined(MEMTEST)
	mtrace();
	//void *crapweasel=0;
	//crapweasel=malloc(123);
	printf("I JUST SET MTRACE!!!!!\n");
	#endif
	//#endif
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
	resource=get_resource(argv[1],headers,65535,&isdirectory,argv[2],"testing");
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