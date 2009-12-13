
#define _GNU_SOURCE

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <regex.h>
#include <time.h>
#include <sys/time.h>


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


