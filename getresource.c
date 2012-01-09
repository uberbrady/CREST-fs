#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "common.h"
#include "worker.h"

int maxcacheage=0;

char authfile[256]="/dev/null";
#include <stdio.h>
#include "resource.h"
#include <string.h>

void prompt()
{
	printf("Type a URL and a verb (a good one is 'GET'), <return> to exit> ");
}

#include <sys/stat.h>

int main( int argc, char **argv )
{
//	int i;
	if(argc!=3) {
		printf("Wrong number of args: %d.\n",argc);
		printf("Usage: %s CACHEAGE CACHEROOT\n",argv[0]);
		exit(1);
	}
	init_worker("/tmp/crestfs.sock",argv[2]);
	init_thread_resources();
	maxcacheage=atoi(argv[1]);
	printf("Max Cache Age Temporarily Configured To: %d\n",maxcacheage);
	char pwd[1024];
	getcwd(pwd,1024);
	if(chdir(argv[2])) {
		printf("Could not change to cache directory!\n");
		exit(1);
	}
	prompt();
	char url[1024];
	char verb[16];
	while(scanf("%s %s",url,verb)>1) {
		printf("Scanf returned url: %s, and verb: %s\n",url,verb);
		if(strlen(verb)==0) {
			strcpy(verb,"GET");
		}
		printf("%s'ing resource: %s\n",verb,url);
		response_t resp;
		memset(&resp,0,sizeof(resp)); //zero it out for the best effect...mebbe...
		FILE *p=new_get_resource(url,verb,"test_getresource","r",&resp);
		if(p==BADFILE) {
			printf("Resource returned I/O Error.\n");
			exit(1);
		}
		char * msg=debug_response(&resp,"MAIN: test_getresource");
		printf("%s",msg);
		free(msg);
		printf("\n------------------------------------------------------------\n" 
			"File pointer: %p,Filetype: %d\n",p,resp.filetype);
		printf("============================================================\n\n");
		if(p==0) {
			printf("(No data file associated with resource)\n");
		} else {
			struct stat st;
			fstat(fileno(p),&st);
			printf("fstat says fileno is: %d, file length is: %ld\n",fileno(p),(long)st.st_size);
			while(!feof(p)) {
				printf("EOF? %d\n",feof(p));
				char readbuf[65535];
				int bytes=fread(readbuf,1,65535,p);
				printf("Bytes read via fread: %d\n",bytes);
				bytes=fwrite(readbuf,1,bytes,stdout);
				printf("Bytes written to stdout: %d\n",bytes);
			}
			fclose(p);
		}
		printf("\n\n");
		prompt();
	}
	chdir(pwd);
	return 0;

}
