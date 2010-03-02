
#define _GNU_SOURCE

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <regex.h>
#include <time.h>
#include <sys/time.h>

#include "common.h"

#include <crypt.h>

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

//char * strptime(char *,char *,struct tm *);
#else
#include <stdarg.h>
#endif

/* utility functions for debugging, path parsing, fetching, etc. */
#include <sys/stat.h>

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
		//brintf("Folderbuffer is: %s, status is: %d\n",foldbuf,mkfold);
		if(mkfold!=0) {
			//brintf("Here's why: %s\n",strerror(errno));
		}
		slashloc+=1;//iterate past the slash we're on
	}
}

FILE *
fopenr(char *filename,char*mode)
{	
	redirmake(filename);
	return fopen(filename,mode);
}



void brintf(char *format,...)
{
	va_list whatever;
	va_start(whatever,format);
	vprintf(format,whatever);
	fflush(NULL);
}


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

void
reanswer(char *string,regmatch_t *re,char *buffer,int length)
{
	memset(buffer,0,length);
	//brintf("You should be copying...? %s\n",string+re->rm_so);
	strlcpy(buffer,string+re->rm_so,length);
	buffer[re->rm_eo - re->rm_so]='\0';
	//brintf("Your answer is: %s\n",buffer);
}

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
fetchstatus(const char *headers) //what?! Why do I have to qualify this?
{
	char status[4]="";
	// HTTP/1.0 200 BLah
	
	if(strlen(headers)<strlen("HTTP/1.0 xyz")) { //what the hell is this about? valgrind is complaining about strlen
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
	memset(&mytime,0,sizeof(mytime));
	char *formatto=strptime(datestring,"%a, %e %b %Y %H:%M:%S %Z",&mytime);
	if(formatto==0) {
		return 0;
	} else {
		return mktime(&mytime);
	}
}

char _staticauthfile[1024]="";
char _userpass[1024]="";
//static buffer; which is fine, we don't want 15 different concepts of what authfile is happening

char *
rootauthurl()
{
	if(strcmp(_staticauthfile,"")!=0) {
		//cache this, but we have no invalidation method yet. This is going to change though.
		return _staticauthfile;
	}
	FILE *authfp=fopen(authfile,"r");
	if(!authfp) {
		return 0; //no authfile at all
	}
	fgets(_staticauthfile,1024,authfp);
	if(strlen(_staticauthfile)>1) {
		//just start by assuming there's a newline at the end of that.
		_staticauthfile[strlen(_staticauthfile)-1]='\0';
		fgets(_userpass,1024,authfp);
		_userpass[strlen(_userpass)-1]='\0';
		strlcat(_userpass,":",1024);
		fgets(_userpass+strlen(_userpass),1024-strlen(_userpass),authfp);
		if(_userpass[strlen(_userpass)-1]=='\n') {
			_userpass[strlen(_userpass)-1]='\0';
		}
	}
	fclose(authfp);
	return _staticauthfile;
}

#include <pthread.h>

pthread_mutex_t mut = PTHREAD_MUTEX_INITIALIZER;


void 
hashname(const char *filename,char hash[23])
{
	pthread_mutex_lock(&mut);
	
	char *ans=crypt(filename,"$1$");
	strcpy(hash,ans+4);
	pthread_mutex_unlock(&mut);
	//strreplace(hash,"/","-");
	char *iter=hash;
	while((iter=strchr(iter,'/'))) { //gotta convert / to - for filenames
		iter[0]='-';
	}
}

char *
wants_auth(const char *path)
{
	char * rau=rootauthurl();
	if(path[0]=='/') {
		path=path+1; //skip leading slashes methinks, FS-style path arrays tend to have them
	}
	if (rau && strlen(rau)>0 && strncmp(rau,path,strlen(rau))==0) {
		return _userpass;
	} else {
		return 0;
	}
}

char *b64table="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void
fill_authorization(const char *path,char *authstring,int authlen)
{
	char *rawauthstring=wants_auth(path);
	if(rawauthstring==0) {
		authstring[0]='\0';
		return;
	}
	strlcpy(authstring,"Authorization: Basic ",authlen);
	//get the authstring from somewhere, then...
	path=path;
	int authlensofar=strlen(authstring);
	unsigned int bits6;
	for(bits6=0;bits6<strlen(rawauthstring)*8;bits6+=6) {
		unsigned int byteindex=bits6/8;
		int bitremainder=bits6 % 8;
		char a=rawauthstring[byteindex];
		char b=0;
		if(strlen(rawauthstring)>byteindex) {
			b=rawauthstring[byteindex+1];
		}
		unsigned short ab=((unsigned short)a)<<8 | b;
		//brintf("a is: %d and b is: %d My short is: %d, bitremainder: %d\n",a,b,ab,bitremainder);
		switch (bitremainder) {
			case 0:
			ab>>=10;
			break;
			
			case 6:
			ab>>=4;
			break;
			
			case 4:
			ab>>=6;
			break;
			
			case 2:
			ab>>=8;
			break;
		}
		//brintf("Transformed, that's: %d, then masked it's %d\n",ab,(ab & 0x3F));
		authstring[authlensofar]=b64table[ab & 0x3F];
		authlensofar++;
		if(authlensofar>authlen) {
			//return? What
			//need to \0 a string first?
			exit(57); 
		}
		
		//remainder 0 => rightshift 10
		//remainder 6 => rightshift 4
		//number will be 12, which is byteindex 1 now, and remainder
		//remainder 4 => rightshift 6
	}
	authstring[authlensofar]='\0';
	switch(strlen(rawauthstring) % 3) {
		case 1:
		strcat(authstring,"==");
		break;
		
		case 2:
		strcat(authstring,"=");
		break;
	}
	if(strlen(authstring)>(unsigned)authlen) {
		exit(57);
	}
}


//PUT support routines?


#define PENDINGDIR ".crestfs_pending_writes"

#include <unistd.h>
#include <sys/stat.h>

void 
markdirty(const char *filename)
{
	char hash[23];
	char pendplace[1024];
	char mangledfilename[1024];
	hashname(filename,hash);
	strlcpy(pendplace,PENDINGDIR,1024);
	strlcat(pendplace,"/",1024);
	strlcat(pendplace,hash,1024);
	mkdir(PENDINGDIR,0700);
	
	strlcpy(mangledfilename,"..",1024);
	strlcat(mangledfilename,filename,1024);
	symlink(mangledfilename,pendplace);
}

int check_put(const char *path)
{
	char hash[23];
	hashname(path,hash);
	char putpath[1024];
	strlcpy(putpath,PENDINGDIR,1024);
	strlcat(putpath,"/",1024);
	strlcat(putpath,hash,1024);
	struct stat dontcare;
	brintf("I am checking path: %s for path: %s\n",putpath,path);
	return (lstat(putpath,&dontcare)==0);
}

char *okstring="HTTP/1.1 200 OK\r\nEtag: \"\"\r\n\r\n";
//The blank etag is actually necessary to trigger proper stat's of the written file. UGH.

#include "resource.h" //needed for putting_routine and invalidate_parents

void
faux_freshen_metadata(const char *path) //cleans and silences metadata for reasonable results to stat()'s, possibly before a file's been actually uploaded
{
	char metafilename[1024];
	strlcpy(metafilename,METAPREPEND,1024);
	strlcat(metafilename,path,1024);
	FILE *metafile=fopen(metafilename+1,"w");
	if(metafile) {
		int writeres=fwrite(okstring,1,strlen(okstring)+1,metafile);
		if(writeres !=0) {
			brintf("rewriting metafile had some problems: write: %d\n",writeres);
		}
		fclose(metafile);
	} else {
		brintf("Could not open/create metafile for faux-freshen! Silently continuing, I guess?\n");
	}
}

#include <libgen.h> //for dirname()

///this function will prolly end up deprecated...
void
invalidate_parents(const char *origpath)
{
	char path[1024];
	strncpy(path,origpath,1024);
	char * basepath=dirname((char *)path+1);
	char parentdir[1024];
	strncpy(parentdir,basepath,1024);
	strncat(parentdir,DIRCACHEFILE,1024);

	char parentmetadir[1024];
	strncpy(parentmetadir,METAPREPEND,1024);
	strncat(parentmetadir,"/",1024);
	strncat(parentmetadir,parentdir,1024);

	int pd=unlink(parentdir); //the actual listing...
	int pdm=unlink(parentmetadir+1); //the metadata about the listing
	brintf("I tried to unlink some parentals for cache-invalidation - parentdir itself (%s) got me: %d; parentmetadir (%s) got: %d\n",
		parentdir,pd,parentmetadir+1,pdm);
	//cached directory now invalidated	
}

#include <utime.h>

void 
append_parents(const char *origpath)
{
	char path[1024];
	char namepath[1024];
	strncpy(path,origpath,1024);
	strncpy(namepath,origpath,1024);
	char * basepath=dirname((char *)path+1);
	char * appendme=basename(namepath+1);
	char parentdir[1024];
	strncpy(parentdir,basepath,1024);
	strncat(parentdir,DIRCACHEFILE,1024);

	char parentmetadir[1024];
	strncpy(parentmetadir,METAPREPEND,1024);
	strncat(parentmetadir,"/",1024);
	strncat(parentmetadir,parentdir,1024);

	FILE *p=fopen(parentdir,"a");
	if(p) {
		fputs("\n<a href='",p);
		fputs(appendme,p);
		fputs("'>",p);
		fputs(appendme,p);
		fputs("</a>\n",p);
		fclose(p);
		int pdm=utime(parentmetadir+1,0); //the metadata about the listing.
							//I'm not sure I like this - it 'freshens' the listing, possibly incorrectly
		brintf("I tried to augment the parentals parentdir itself (%s) got me: %p; parentmetadir (%s) got: %d\n",
			parentdir,p,parentmetadir+1,pdm);
	} else {
		brintf("Couldn't open parentdir: %s, I don't know if that's a bad thing or not...leaving everything alone...\n",parentdir);
	}
	
	//cached directory now invalidated	
}


#include <dirent.h>
#include <sys/file.h>


#include <errno.h>


#define IDLETIME 10

void *
putting_routine(void *unused __attribute__((unused)))
{
	while(1) {
		brintf("I like to put things\n");
		DIR *pendingdir=opendir(PENDINGDIR);
		struct dirent mydirent;
		struct dirent *dp=0;
		memset(&mydirent,0,sizeof(mydirent));
		if(pendingdir==0) {
			brintf("Error opening pendingdir is: %s\n",strerror(errno));
			sleep(10);
			continue;
		}
		int sleeptime=10;
		while(readdir_r(pendingdir,&mydirent,&dp)==0) {
			if(dp==0) {
				brintf("End of directory?\n");
				break;
			}
			char *headerpointer=0;
			if(strcmp(dp->d_name,".")==0 || strcmp(dp->d_name,"..")==0) {
				continue;
			}
			char linkname[1024];
			strlcpy(linkname,PENDINGDIR,1024);
			strlcat(linkname,"/",1024);
			strlcat(linkname,dp->d_name,1024);
			char linktarget[1024];
			int s=-1;
			s=readlink(linkname,linktarget,1024);
			linktarget[s]='\0';
			brintf("Look! I found this one: %s -> %s\n",dp->d_name,linktarget);
			//lock the metadata file, PUT the results, and close the lock (opened FILE *)
			char metafilename[1024];
			strlcpy(metafilename,METAPREPEND,1024);
			strlcat(metafilename,linktarget+2,1024);

			struct stat st;
			if(stat(linkname,&st)!=0) {
				brintf("Couldn't stat our file! Going to next iteration of loop\n");
				continue;
			}
			int fileage=time(0) - st.st_mtime;
			brintf("Checking if File has been around long enough - IDLETIME is: %d, file age is %d, old sleep time was: %d, new sleep time might be: %d\n",
				IDLETIME,fileage,sleeptime,IDLETIME-fileage);
			if( fileage < IDLETIME) {
				brintf("No it has NOT\n");
				if((IDLETIME - fileage) >0 && (IDLETIME - fileage) <sleeptime) {
					sleeptime=IDLETIME-fileage;
				}
				continue;
			} else {
				brintf("File is old enough, continue processing it\n");
			}

			FILE *metafile=fopen(metafilename+1,"r+");
			int lck=-1;
			if(metafile) {
				lck=flock(fileno(metafile),LOCK_EX);
			}
			brintf("We are going to try to open metafile: %s. results: %p. lockstatus: %d\n",metafilename+1,metafile,lck);
			FILE *contentfile=fopen(linktarget+3,"r");
			brintf("Content file is: %s (%p)\n",linktarget+3,contentfile);
			char *headerplus;
			asprintf(&headerplus,"Content-length: %d",(int)st.st_size);
			
			//http_request(char *fspath,char *verb,char *etag, char *referer,char *extraheaders,FILE *body)
			brintf("Our link target for today is: %s\n",linktarget+2);
			int fd=http_request(linktarget+2,"PUT",0,"putting_routine",headerplus,contentfile);
			fclose(contentfile);
			free(headerplus);
			if(fd<=0) {
				brintf("Seriously weird problem with http_request, going for next iteration in loop...(%s)\n",strerror(errno));
				free(headerpointer);
				fclose(metafile);
				continue;
			}
			recv_headers(fd,&headerpointer);
			
			int status=fetchstatus(headerpointer);
			
			if(status==200 || status==201 || status==204) {
				char etag[1024]="";
				char sanitized_headers[8192]="";
				ftruncate(fileno(metafile),0);
				brintf("RECEIVED HEADERS SAY: %s\n",headerpointer);
				fetchheader(headerpointer,"etag",etag,1024);
				snprintf(sanitized_headers,8192,"HTTP/1.0 %d Synthetic Header\r\n",fetchstatus(headerpointer));
				if(strlen(etag)>0) { 
					strlcat(sanitized_headers,"Etag: ",8192);
					strlcat(sanitized_headers,etag,8192);
					strlcat(sanitized_headers,"\r\n",8192);
				}
				
				strlcat(sanitized_headers,"Last-Modified: ",8192);
				char httpdate[80];
				struct tm now;
				time_t nowtime;
				nowtime=time(0);
				localtime_r(&nowtime,&now);
				strftime(httpdate,80,"%a %e %b %Y %H:%M:%S %Z",&now);
				strlcat(sanitized_headers, httpdate, 8192);
				strlcat(sanitized_headers,"\r\n",8192);
				
				strlcat(sanitized_headers,"\r\n",8192);
				fputs(sanitized_headers,metafile);
				fputc('\0',metafile);
				//fputs(headerpointer,metafile);
				

				//then readback headers, write them to headerfile, 
				//close(fd); //stop that!!!!
				wastebody("PUT",fd,headerpointer);
				return_keep(fd);
				unlink(linkname);//now that we're done, toss the symlink
				free(headerpointer);
	
				//need to invalidate parent directory's cached info (as it shall no longer be valid)
				//append_parents(linktarget+2); //that's the fuse-related path (with leading forward-slash) for the file we just put.
				//Wait, wait, wait - I did the 'append-parents' deal already when I created teh file (so that parental directory listsings would work)
				//so I don't need to do that again, nor do I need to freshen that directory listing - It's fine if it gets refreshed x seconds
				//after the initial append_parents call, that's fine. Well, not *completely* - what if the file hasn't been put up yet?
				//that could be an issue. It *should* get put up, but what if it's big and takes longer than x seconds? Could happen!
				//the short answer is "I dunno" and the longer answer is "When I do all-async-always, it'll be fixed."
				//FIXME
				
				fclose(metafile);//unlock, end transaction
			} else {
				brintf("Status isn't in the 200 range I'm expecting: %d\n",status);
				brintf("Headers for fail are: %s\n",headerpointer);
				free(headerpointer);
				//close(fd); //goddammit! Stop that rude garbage!
				wastebody("PUT",fd,headerpointer);
				return_keep(fd);
				fclose(metafile);
			}
			
		}
		sleep(10);
		closedir(pendingdir);
	}
	return 0;
}

#include <sys/socket.h>
#include <errno.h>

int 
recv_headers(int mysocket,char **headerpointer/* ,void **bodypiece */)
{
	int mydesc=dup(mysocket);
	brintf("My socket is: %d, we duped that to %d. I *PROMISE* to close it, here in-function.\n",mysocket,mydesc);
	FILE *web=fdopen(mydesc,"r");
	setvbuf(web,0,_IONBF,0);
	int bytesofheader=0;
	*headerpointer=calloc(1,1); //init to \0
	int i=0;
	while(i++<100) {
		char linebuf[1024]="";
		fgets(linebuf,1024,web);
		bytesofheader+=strlen(linebuf);
		brintf("First header line: strlen is %d, header is: %d, line is :%s",strlen(linebuf),bytesofheader,linebuf);
		*headerpointer=realloc(*headerpointer,bytesofheader+1);
		strcat(*headerpointer,linebuf);
		if(strcmp(linebuf,"\r\n")==0) {
			fclose(web);
			return 1;
		}
	}
	fclose(web);
	return 0;
}

// DIRECTORY HELPER FUNCTIONS

/**** DIRECTORY ITERATOR HELPERS ****/

#define DIRBUFFER	1*1024*1024
// 1 MB
#define DIRREGEX	"<a[^>]href=['\"]([^'\"]+)['\"][^>]*>([^<]+)</a>"

static regex_t re;
static int initted=0;

void
init_directory_iterator(directory_iterator *iter,const char *headers, FILE *fp) //char *?
{
	brintf("Initializing directory iterator\n");
	if(!initted) {
		int status=regcomp(&re,DIRREGEX,REG_EXTENDED|REG_ICASE);
		if(status!=0) {
			char error[80];
			regerror(status,&re,error,80);
			brintf("ERROR COMPILING REGEX: %s\n",error);
			exit(98);
		}
		initted=1;
	}
	memset(iter,0,sizeof(iter));
	iter->mode=unknown;
	
	if(fp==0) {
		brintf("FAIL URE - I cannot iterate on 'empty!'\n");
		exit(62);
	}
	char contenttype[1024]="";
	brintf("Mine Headers arE: %s",headers);
	fetchheader((char *)headers,"content-type",contenttype,1024);
	if(strcasecmp(contenttype,"x-vnd.bespin.corp/directory-manifest")==0) {
		iter->mode=manifest;
		iter->iterator.manifestmode.fptr=fp; //be kind, rewind
	} else {
		iter->mode=html;
		iter->iterator.htmlmode.directory_buffer=calloc(DIRBUFFER,1);
		if(iter->iterator.htmlmode.directory_buffer==0) {
			brintf("Great, can't malloc our dirbuffer. baililng.\n");
			exit(19);
		}
		iter->iterator.htmlmode.directory_buffer[0]='\0';
		fread(iter->iterator.htmlmode.directory_buffer,1,DIRBUFFER,fp);
		rewind(fp);
		
		memset(&(iter->iterator.htmlmode.rm),0,sizeof(iter->iterator.htmlmode.rm));
		iter->iterator.htmlmode.weboffset=0;
	}
}

int
directory_iterate(directory_iterator *metaiter,char *buf,int buflen,char *etagbuf,int etaglen)
{
	brintf("ITERATE!\n");
	char hrefname[255];
	char linkname[255];
	char *directory=0;
	char etag[128];
	char filename[1024];
	switch(metaiter->mode) {
		case unknown:
		brintf("Unknown file type (or uninitted directory_iterator?)\n");
		return 0;
		break;
		
		case html:
		if(etagbuf!=0 && etaglen!=0) {
			etagbuf[0]='\0'; //no etags in htmlmode, sorry
		}

		//brintf("Weboffset: %d\n",iter->weboffset);
		directory=metaiter->iterator.htmlmode.directory_buffer;
		while(regexec(&re,directory+metaiter->iterator.htmlmode.weboffset,3,metaiter->iterator.htmlmode.rm,0)==0) {
			reanswer(directory+metaiter->iterator.htmlmode.weboffset,&metaiter->iterator.htmlmode.rm[1],hrefname,255);
			reanswer(directory+metaiter->iterator.htmlmode.weboffset,&metaiter->iterator.htmlmode.rm[2],linkname,255);
			metaiter->iterator.htmlmode.weboffset+=metaiter->iterator.htmlmode.rm[0].rm_eo;

			//brintf("href: %s link: %s\n",hrefname,linkname);
			if(strcmp(hrefname,linkname)==0) {
				metaiter->iterator.htmlmode.filecounter++;
				//brintf("ELEMENT: %s\n",hrefname);
				strlcpy(buf,hrefname,buflen);
				return 1;
			}
		}
		return 0;
		break;
		
		case manifest:
		//just read another line, split it off, and you're good.
		//hell, the algorithm may be in use somewhere already
		while(fscanf(metaiter->iterator.manifestmode.fptr,"%128s %1024[^\r\n]",etag,filename) != EOF) {
			brintf("ETAG: %s for filename: %s\n",etag,filename);
			strlcpy(buf,filename,buflen);
			
			if(etagbuf!=0 && etaglen!=0) {
				strlcpy(etagbuf,etag,etaglen);
			}
			return 1;
		}
		return 0;
		break;
		
		default:
		return 0;
		break;
	}
}

void free_directory_iterator(directory_iterator *iter)
{
	brintf("FREEING directory iterator!\n");
	switch(iter->mode) {
		case unknown:
		break;
		
		case html:
		free(iter->iterator.htmlmode.directory_buffer);
		break;
		
		case manifest:
		if(iter->iterator.manifestmode.fptr) {
			rewind(iter->iterator.manifestmode.fptr);
		} else {
			brintf("Manifest file pointer is empty - that's WEIRD!\n");
		}
		break;
		
		default:
		break;
	}
}
