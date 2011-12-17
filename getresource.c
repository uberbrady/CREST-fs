#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "common.h"

int maxcacheage=0;

char authfile[256]="/dev/null";

#include "resource.h"

extern int
	is_plausible_file(const char *origpath);


char *plauses[]={"Nonexistent","Unknown","Fresh"};


int main( int argc, char **argv )
{
//	int i;
	if(argc!=4) {
		printf("Wrong number of args: %d.\n",argc);
		printf("Usage: %s CACHEAGE CACHEROOT /PATH-URL/without/http/leading/it/but/starting/with/slash\n",argv[0]);
		exit(1);
	}
	maxcacheage=atoi(argv[1]);
	brintf("Max Cache Age Temporarily Configured To: %d\n",maxcacheage);
	char headers[65535];
	int isdir;
	char pwd[1024];
	getcwd(pwd,1024);
	chdir(argv[2]);
	FILE *p=get_resource(argv[3],headers,65535,&isdir,"GET","plausibilitytest","r");
	printf("\n------------------------------------------------------------\n" 
		"File pointer: %p,Headers: %s, Is directory?: %d\n",p,headers,isdir);
	printf("============================================================\n\n");
	while(!feof(p)) {
		char readbuf[65535];
		int bytes=fread(readbuf,1,65535,p);
		fwrite(readbuf,1,bytes,stdout);
	}
	chdir(pwd);
	return 0;

}
