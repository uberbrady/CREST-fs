/* For more information, see http://www.macdevcenter.com/pub/a/mac/2007/03/06/macfuse-new-frontiers-in-file-systems.html. */ 
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

#include <regex.h>

#define _FILE_OFFSET_BITS 64
#define FUSE_USE_VERSION  26
#include <fuse.h>

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


#define MAXDATASIZE 65535
#define HOSTLEN 64
#define PATHLEN 1024

/* GIVEN: a 'path' where the first element is a hostname, the rest are path elements
   RETURN: 
*/
void 
webfetch(const char *path,char *buffer,int maxlength, char *verb)
{
	int sockfd, numbytes;  
	struct addrinfo hints, *servinfo, *p;
	int rv;
	char hostname[HOSTLEN];
	char pathstub[PATHLEN];
	char *reqstr=0;
//	char *origbuffer=buffer;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	
	pathparse(path,hostname,pathstub,HOSTLEN,PATHLEN);
	
	//printf("Hostname is: %s\n",hostname);

	if ((rv = getaddrinfo(hostname, "80", &hints, &servinfo)) != 0) {
		fprintf(stderr, "I failed to getaddrinfo: %s\n", gai_strerror(rv));
		buffer[0]='\0';
		return;
	}

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
		exit(-1);
	}
	
	printf("Okay, connectication has occurenced\n");

/*     getsockname(sockfd, s, sizeof s);
    printf("client: connecting to %s\n", s);
*/
    freeaddrinfo(servinfo); // all done with this structure
	asprintf(&reqstr,"%s %s HTTP/1.0\r\nHost: %s\r\n\r\n",verb,pathstub,hostname);
	//printf("REQUEST: %s\n",reqstr);
	send(sockfd,reqstr,strlen(reqstr),0);
	free(reqstr);
	

    while ((numbytes = recv(sockfd, buffer, MAXDATASIZE-1, 0)) >0) {
		//keep going
		buffer+=numbytes;
    }
	if(numbytes<0) {
		printf("Error - %d\n",numbytes);
		return;
	}

    buffer[numbytes] = '\0';

//    printf("client: received '%s'\n",origbuffer);

    close(sockfd);

    return;
}


void
reanswer(char *string,regmatch_t *re,char *buffer,int length)
{
	memset(buffer,0,length);
	//printf("You should be copying...? %s\n",string+re->rm_so);
	strlcpy(buffer,string+re->rm_so,length);
	buffer[re->rm_eo - re->rm_so]='\0';
	//printf("Your answer is: %s\n",buffer);
}

void
fetchheader(char *headers,char *name,char *results,int length)
{
	char *cursor=headers;
	int blanklinecount=0;
	while(cursor[0]!='\0' && cursor < headers+strlen(headers)) {
		char *lineending=strstr(cursor,"\r\n");
		//printf("SEARCHING FROM: CHARACTERS: %c %c %c %c %c\n",cursor[0],cursor[1],cursor[2],cursor[3],cursor[4]);
		//printf("Line ENDING IS: %p, %s\n",lineending,lineending);
		if(lineending==cursor) {
			blanklinecount++;
		} else {
			char line[1024];
			char *colon=0;
			int linelen=0;

			blanklinecount=0; //consecutive blank line counter must be reset
			strlcpy(line,cursor,1024);
			line[lineending-cursor]='\0';
			//printf("my line is: %s\n",line);
			linelen=strlen(line);
			colon=strchr(line,':');
			if(colon) {
				int keylen=colon-line;
				//printf("Colon onwards is: %s keylen: %d\n",colon,keylen);
				if(strncasecmp(name,line,keylen)==0) {
					while(colon[1]==' ') {
						//printf("advanced pointer once\n");
						colon++;
					}
					//printf("colon+1: %s, length %d, strlen(line): %d\n",colon+1,linelen-keylen,linelen);
					strncpy(results,colon+1,linelen-keylen);
					results[linelen-keylen+1]='\0';
					//printf("Well, results look like the will be: %s\n",results);
					return;
				}
				//printf("line is: %s\n",line);
			} else {
				//printf("No colon found!!!!!!!!!\n");
			}
		}
		if(blanklinecount==2) {
			break;
		}
		
		cursor=lineending+2;//advance past the carriage-return,newline
	}
	//must not have found it or would've left already!
	results[0]='\0';
}

int
fetchstatus(char *headers)
{
	char status[4];
	
	strncpy(status,headers+9,3);
	status[3]='\0';
	return atoi(status);
}

/******************* END UTILITY FUNCTIONS< BEGIN ACTUALLY DOING OF STUFF!!!!! *********************/

#define HEADERLEN 65535

static int
crest_getattr(const char *path, struct stat *stbuf)
{
	char header[HEADERLEN];
    memset(stbuf, 0, sizeof(struct stat));

    if (strcmp(path, "/") == 0) { /* The root directory of our file system. */
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
    } else {
		char hostname[80];
		char pathpart[1024];
		pathparse(path,hostname,pathpart,80,1024);
		
		if(strcmp(pathpart,"/")==0) {
			//root of some host, MUST be a directory no matter what.
			stbuf->st_mode = S_IFDIR | 0755; //I am a a directory?
			stbuf->st_nlink = 2; //with a . and a .. which will be wrong for certain.						
		} else {
			char results[80];
			webfetch(path,header,HEADERLEN,"HEAD");
			if(fetchstatus(header)>=400 && fetchstatus(header)<500) { //400, e.g., 404...
				return -ENOENT;
			}
			//printf("Headers fetched: %s",header);
			fetchheader(header,"Location",results,80);
			//printf("Location found? %s\n",results);
			//so - question - how do we determine the 'type' of this file - 
			//e.g. is it a directory? or a file?
			/*
			It's a more complicated question than I thought.
			#1) Amazon, for example, won't have indexes - but will have appropriate things I can do instead. that's fine.
			#2) A homepage that doesn't list its subsidiary pages _won't_ be considered a directory. Hell, no root page
				will ever be considered a directory - because there's nothing you can request _without_ the slash.
			#3) I think that answeres my question. Root dir is always a dir. Otherwise do your test.
			*/
			if(fetchstatus(header)>=300 && fetchstatus(header)<400 && strlen(results)>0 && results[strlen(results)-1]=='/') {
				//e.g. - user has been redirected to a directory, then:
				stbuf->st_mode = S_IFDIR | 0755; //I am a a directory?
				stbuf->st_nlink = 2; //with a . and a .. which will be wrong for certain.			
			} else {
				char length[32];
				char date[32];
				char *formatto=0;
				struct tm mytime;
				stbuf->st_mode = S_IFREG | 0755;
				fetchheader(header,"last-modified",date,32);
				//printf("Post-mangulation headers is: %s\n",header);
				fetchheader(header,"content-length",length,32);
				//printf("Post-mangulation headers is: %s\n",header);
				//printf("BTW, date I'm trying to format: %s\n",date);
				if((formatto=strptime(date,"%a, %e %b %Y %H:%M:%S %Z",&mytime))!=0) { //Tue, 18 Nov 2008 15:34:20 GMT
					stbuf->st_mtime = mktime(&mytime);
					//printf("I have a time! and it is: %s\n",asctime(&mytime));
				}
				//if(formatto)
				//	printf("Formatto is: %s\n",formatto);
				stbuf->st_size = atoi(length);
			}
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

#define DIRBUFFER	1*1024*1024
// 1 MB

static int
crest_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
              off_t offset, struct fuse_file_info *fi)
{
	char *dirbuffer=0;

	regex_t re;
	regmatch_t rm[3];
	int status=0;
	int weboffset=0;
	char slashpath[1024];
	
	strncpy(slashpath,path,1024);
	strlcat(slashpath,"/",1024);
	
	path=slashpath;
	
	if (strcmp(path, "/") == 0) {
		//we are exactly JUST the ROOT  dir
		//in this case, we *MAY* want to walk through what we have cached and list that.
	    filler(buf, ".", NULL, 0);           /* Current directory (.)  */
	    filler(buf, "..", NULL, 0);          /* Parent directory (..)  */
	    //filler(buf, "poopie+doops", NULL, 0); /* The only file we have...? */
		//so, yeah, EVENTUALLY we want to walk through the cache directory
		//and pick hostnames for which we have cached data. maybe.
		return 0;
	}
	//pathparse(path,hostname,pathpart,HOSTLEN,PATHLEN);
	
	dirbuffer=malloc(DIRBUFFER);
	
	webfetch(path,dirbuffer,DIRBUFFER,"GET");
	if(strlen(dirbuffer)==0) {
		free(dirbuffer);
		return -ENOENT;
	}
	printf("Fetchd: %s\n",dirbuffer);
	status=regcomp(&re,"<a[^>]href=['\"]([^'\"]+)['\"][^>]*>([^<]+)</a>",REG_EXTENDED|REG_ICASE); //this can be globalized for performance I think.
	if(status!=0) {
		char error[80];
		regerror(status,&re,error,80);
		printf("ERROR COMPILING REGEX: %s\n",error);
		exit(-1);
	}
    filler(buf, ".", NULL, 0);           /* Current directory (.)  */
    filler(buf, "..", NULL, 0);          /* Parent directory (..)  */
	int failcounter=0;
	while(status==0 && failcounter < 100) {
		failcounter++;
		
		status=regexec(&re,dirbuffer+weboffset,3,rm,0); // ???
		char hrefname[255];
		char linkname[255];
		if(status==0) {
			reanswer(dirbuffer+weboffset,&rm[1],hrefname,255);
			reanswer(dirbuffer+weboffset,&rm[2],linkname,255);

			//printf("Href? %s\n",hrefname);
			//printf("Link %s\n",linkname);
			printf("href: %s link: %s\n",hrefname,linkname);
			if(strcmp(hrefname,linkname)==0) {
				filler(buf, hrefname, NULL, 0);
			}
			weboffset+=rm[0].rm_eo;
			printf("Weboffset: %d\n",weboffset);
		}
		//printf("staus; %d, 0: %d, 1:%d, 2: %d, href=%s, link=%s\n",status,rm[0].rm_so,rm[1].rm_so,rm[2].rm_so,hrefname,linkname);
		//filler?
		//filler(buf,rm[])
	}
	if(failcounter>=100) {
		printf("Fail due to toomany\n");
	}
	
	free(dirbuffer);
	
    return 0;
}

#define FILEMAX 64*1024*1024

static int
crest_read(const char *path, char *buf, size_t size, off_t offset,
           struct fuse_file_info *fi)
{
	char *filebuffer=0;
	char *headerend=0;
	char length[32];
	
	filebuffer=malloc(FILEMAX);
	
	webfetch(path,filebuffer,FILEMAX,"GET");
	fetchheader(filebuffer,"content-length",length,32);
	int file_size=atoi(length);
	//printf("File size: %d\n",file_size);
	headerend=strstr(filebuffer,"\r\n\r\n");
	headerend+=4;
	//printf("Header end is: %s\n",headerend);

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

static struct fuse_operations crest_filesystem_operations = {
    .getattr = crest_getattr, /* To provide size, permissions, etc. */
    .open    = crest_open,    /* To enforce read-only access.       */
    .read    = crest_read,    /* To provide file content.           */
    .readdir = crest_readdir, /* To provide directory listing.      */
};

/* Test routines */

void pathtest(char *fullpath)
{
	char host[HOSTLEN];
	char path[PATHLEN];
	pathparse(fullpath,host,path,HOSTLEN,PATHLEN);
	printf("Okay, from path '%s' host is: '%s' and path is: '%s'\n",fullpath,host,path);
}

void hdrtest(char *header,char *name)
{
	char pooh[90];
	fetchheader(header,name,pooh,80);
	printf("Header name: %s: value: %s\n",name,pooh);
}

/* END TESTING */

int
main(int argc, char **argv)
{
	char buf[DIRBUFFER];
/*	pathtest("/desk.nu");
	pathtest("/desk.nu/pooh.html");
	pathtest("/desk.nu/braydix");
	pathtest("/desk.nu/braydix/");
	exit(0); */
				
/* 	if(status) {
		char error[80];
		regerror(status,&re,error,80);
		printf("error: %s\n",error);
	} else {
		printf("NO FAIL!!!!!!!!!!!\n");
	}
	exit(0);
	regfree(&re); */
	
	webfetch("/desk.nu/braydix",buf,DIRBUFFER,"HEAD");
	hdrtest(buf,"Scheissen");
	hdrtest(buf,"etag");
	hdrtest(buf,"date");
	hdrtest(buf,"content-type");
	printf("Header status: %d\n",fetchstatus(buf));
	printf("Lookit: %s\n",buf);
	//exit(0);
    return fuse_main(argc, argv, &crest_filesystem_operations, NULL);
}
