#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "common.h"

int maxcacheage=0;

char authfile[256]="/dev/null";

#include "resource.h"
#include <string.h>

int main( int argc, char **argv )
{
//	int i;
	if(argc<4||argc>5) {
		printf("Wrong number of args: %d.\n",argc);
		printf("Usage: %s CACHEAGE CACHEROOT /PATH-URL/without/http/leading/it/but/starting/with/slash [VERB=GET]\n",argv[0]);
		exit(1);
	}
	char verb[16]="GET";
	if(argc==5) {
		if(strlen(argv[4])>15) {
			printf("Trying to use HTTP verb that is too long: %s\n",argv[4]);
			exit(1);
		}
		strcpy(verb,argv[4]);
	}
	maxcacheage=atoi(argv[1]);
	printf("Max Cache Age Temporarily Configured To: %d, using HTTP verb: %s\n",maxcacheage,verb);
	char headers[65535];
	int isdir;
	char pwd[1024];
	getcwd(pwd,1024);
	if(chdir(argv[2])) {
		printf("Could not change to cache directory!\n");
		exit(1);
	}
	FILE *p=get_resource(argv[3],headers,65535,&isdir,verb,"plausibilitytest","r");
	if(p==BADFILE) {
		printf("Resource returned I/O Error.\n");
		exit(1);
	}
	printf("\n------------------------------------------------------------\n" 
		"File pointer: %p,Headers: %s, Is directory?: %d\n",p,headers,isdir);
	printf("============================================================\n\n");
	if(p==0) {
		printf("(No data file associated with resource)\n");
	} else {
		while(!feof(p)) {
			char readbuf[65535];
			int bytes=fread(readbuf,1,65535,p);
			fwrite(readbuf,1,bytes,stdout);
		}
	}
	chdir(pwd);
	return 0;

}
