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
	brintf("Max Cache Age Temporarily Configured To: %d",maxcacheage);
/*	printf("Args are:\n");
	for(i=0;i<argc;i++) {
		printf("Arg[%d] is: %s\n",i,argv[i]);
	} */
	char pwd[1024];
	getcwd(pwd,1024);
	chdir(argv[2]);
	int p=is_plausible_file(argv[3]);
	chdir(pwd);
	printf("File: %s plausibility: %d [%s]\n",argv[2],p,plauses[p]);
/*	char headers[65535];
	int isdir;
	FILE *p=get_resource(argv[2],headers,65535,&isdir,"GET","plausibilitytest","r");
	printf("\n------------------------------------------------------------\n" 
		"File pointer: %p,Headers: %s, Is directory?: %d\n",p,headers,isdir);
*/
	return 0;

}
