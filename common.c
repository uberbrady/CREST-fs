
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
		fclose(authfp);
	}
	return _staticauthfile;
}

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
	if (strlen(rau)>0 && strncmp(rau,path,strlen(rau))==0) {
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
