
#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <unistd.h>
#include "http.h" 
#include "common.h"

/* keepalive support */

struct keepalive {
	char *host;
	int fd;
	int inuse;
	//some sort of time thing?
};


#include <pthread.h>

pthread_mutex_t keep_mut = PTHREAD_MUTEX_INITIALIZER;

#define MAXKEEP 32
//we want this normaly to be higher - say, 32 or so? But I lowered it to look at a bug

struct keepalive keepalives[MAXKEEP];
int curkeep=0;

int
find_keep(char *hostname)
{
	int i;
	pthread_mutex_lock(&keep_mut);
	for(i=0;i<curkeep;i++) {
		if(keepalives[i].host && strcasecmp(hostname,keepalives[i].host)==0) {
			//int newfd=-1;
			int *inuse=&keepalives[i].inuse;
			//brintf("Found a valid keepalive at index: %d\n",i);
			//fcntl(poo)
			if(*inuse==0) {		//This should be an atomic test-and-set
				(*inuse)++;	//But I don't know how to do that ...
				pthread_mutex_unlock(&keep_mut);
				return keepalives[i].fd;
				/*
				newfd=dup(keepalives[i].fd);
				if(newfd==-1) {
					brintf("keepalive dup failed, blanking and closing that entry(%i)\n",i);
					free(keepalives[i].host);
					keepalives[i].host=0;
					close(keepalives[i].fd);
					keepalives[i].fd=0;
					return -1; 
				}
				return newfd; */
			} else {
				brintf("Well, I found a good keepalive candidate, but it's already in use: %d\n",*inuse);
			}
		}
	}
	brintf("NO valid keepalive available for %s, current entry count: %d\n",hostname,curkeep);
	pthread_mutex_unlock(&keep_mut);
	return -1;
}

int
insert_keep(char *hostname,int fd)
{
	//re-use empty slots
	int i;
	brintf("Inserting keepalive for hostname: %s\n",hostname);
	for(i=0;i<curkeep;i++) {
		if(keepalives[i].host==0) {
			brintf("Reusing keepalive slot %d for insert for host %s\n",i,hostname);
			keepalives[i].host=strdup(hostname);
			keepalives[i].fd=fd;
			keepalives[i].inuse=1;
			return 1;
		}
	}
	if(curkeep<MAXKEEP) {
		keepalives[curkeep].host=strdup(hostname);
		keepalives[curkeep].fd=fd;
		keepalives[curkeep].inuse=1;
		curkeep++;
		return 1;
	}
	return 0;
}

int
delete_keep(int fd)
{
	int i;
	for(i=0;i<curkeep;i++) {
		if(keepalives[i].fd == fd) {
			brintf("Found the keepalive to delete for host %s (%d).\n",keepalives[i].host,i);
			free(keepalives[i].host);
			keepalives[i].host=0;
			//close(keepalives[i].fd); //whoever else is using this FD can keep using it, but we don't wanna keep it open anymore
			//whoever's deleting can close it, but I ain't gon do it
			keepalives[i].fd=0;
			keepalives[i].inuse=0;
			return 1;
		}
	}
	return 0;
}

void
return_keep(int fd)
{
	int i;
	pthread_mutex_lock(&keep_mut);
	for(i=0;i<curkeep;i++) {
		if(keepalives[i].fd==fd) {
			keepalives[i].inuse--;
			brintf("Returning a keepalive for host %s, inuse is now: %d\n",keepalives[i].host,keepalives[i].inuse);
			pthread_mutex_unlock(&keep_mut);
			return;
		}
	}
	brintf("Someone tried to return keepalive with FD: %d and I couldn't find it.\n",fd);
	pthread_mutex_unlock(&keep_mut);
}
/* end keepalive support */

#include <resolv.h>

//http_request - extra headers are optional (0 is ok), and so is the body param (0)
//http_request does *NOT* know which headers to add - e.g. content-length (!) so 
//that's your problem, not its. It's got enough to do already.
//extra-headers is full HTTP headers separated by \r\n. It will add the last \r\n for you, so just keep it like:
//	"foo: bar\r\nbaz: bif"
//The 'body', if set, will be transmitted after the headers. Closing the body file pointer is your problem.

httpsocket badsock={-1,0,0,0,unknown_encoding,0,0};

httpsocket
http_request(const char *fspath,char *verb,char *etag, char *referer,char *extraheaders,FILE *body)
{
	char hostpart[1024]="";
	char pathpart[1024]="";
	pathparse(fspath,hostpart,pathpart,1024,1024);
	int sockfd=-1;
	char *reqstr=0;
	char extraheadersbuf[16384];
	long start=0;
	int keptalive=0;
	
	//brintf("Hostname is: %s, path is: %s\n",hostpart,pathpart);
	
	//brintf("http_request: VERB: %s, URL: %s, referer: %s, extraheaders: %s, body pointer: %p\n",verb,fspath,referer,extraheaders,body);
	start=time(0);
	//brintf("Getaddrinfo timing test: BEFORE: %ld\n",start);
	
	sockfd=find_keep(hostpart);
	//brintf("finished find keep: %d\n",sockfd);
	if(sockfd==-1) {
		//brintf("Sockfd was -1\n");
		int rv;
		struct addrinfo hints, *servinfo=0, *p=0;
		memset(&hints, 0, sizeof hints);
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;

		if ((rv = getaddrinfo(hostpart, "80", &hints, &servinfo)) != 0) {
			//brintf("Getaddrinfo timing test: FAIL-AFTER: %ld - couldn't lookup %s because: %s\n",
			//	time(0)-start,hostpart,gai_strerror(rv));
			return badsock;
		}
		//brintf("Got getaddrinfo()...GOOD-AFTER: %ld\n",time(0)-start);

		// loop through all the results and connect to the first we can
		for(p = servinfo; p != NULL; p = p->ai_next) {
			if ((sockfd = socket(p->ai_family, p->ai_socktype,
					p->ai_protocol)) == -1) {
				//perror("client: socket");
				continue;
			}
			struct timeval to;
			memset(&to,0,sizeof(to));
			to.tv_sec = 3;
			to.tv_usec= 0;
			if(setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to))) {
				perror("Could not set RECEIVE timeout");
				abort();
			}
			if(setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to))) {
				perror("Could not set SEND timeout?");
				abort();
			}
			
			struct timeval tget;
			memset(&tget,0,sizeof(tget));
			unsigned int sizething=sizeof(tget);
			if(getsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tget,&sizething)) {
				perror("Could NOT *GET* receive timeouts");
				abort();
			}
			//brintf("Gotten socket options - seconds: %d, usec: %d. Options size: %d (vs sizeof at %ld) \n",
			//	(int)tget.tv_sec, (int)tget.tv_usec, sizething,sizeof(tget));
			if(getsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tget,&sizething)) {
				perror("Could not *GET* send timeouts");
				abort();
			}
			//brintf("Gotten socket options - seconds: %d, usec: %d\n. Options size: %d (vs sizeof %ld) \n",
			//	(int)tget.tv_sec, (int)tget.tv_usec, sizething, sizeof(tget));

			if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
				close(sockfd);
				brintf("Path which we couldn't connect to: %s",fspath);
				perror("client: connect");
				continue;
			}

			break;
		}

		if (p == NULL) {
			brintf("client: failed to connect\n");
			close(sockfd);
			return badsock;
		}

		//brintf("Okay, connectication has occurenced connect-AFTER: %ld\n",time(0)-start);
		insert_keep(hostpart,sockfd);
		freeaddrinfo(servinfo); // all done with this structure
	} else {
		//brintf("Using kept-alive connection... keep-AFTER: %ld\n",time(0)-start);
		keptalive=1;
	}

/*     getsockname(sockfd, s, sizeof s);
    brintf("client: connecting to %s\n", s);
*/
	//extraheadersbuf should either be blank, or be terminated with \r\n
	if(etag && strcmp(etag,"")!=0) {
		strcpy(extraheadersbuf,"If-None-Match: ");
		strlcat(extraheadersbuf,etag,16384);
		strlcat(extraheadersbuf,"\r\n",16384);
	} else {
		extraheadersbuf[0]='\0';
	}
	if(extraheaders && strlen(extraheaders)>0) {
		strlcat(extraheadersbuf,extraheaders,16384);
		strlcat(extraheadersbuf,"\r\n",16384);
	}
	//brintf("Checking to see if authorization is desired for %s: %s\n",fspath,wants_auth(fspath));
	if(wants_auth(fspath)) {
		char authstring[80]="\0";
		fill_authorization(fspath,authstring,80);
		strlcat(extraheadersbuf,authstring,16384);
		strlcat(extraheadersbuf,"\r\n",16384);
	}
	asprintf(&reqstr,"%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: CREST-fs/1.75\r\nReferer: %s\r\n%s\r\n",verb,pathpart,hostpart,referer,extraheadersbuf);
	//brintf("REQUEST: %s\n",reqstr);
	int sendresults=send(sockfd,reqstr,strlen(reqstr),0);
	free(reqstr);
	//brintf("from start to SEND-AFTER delay was: %ld\n",time(0)-start);
	int bodysendresults=1;
	int totalbytes=0;
	if(body) {
		unsigned int sendsize=0xFFFFFFFF;
		socklen_t optlength=sizeof(sendsize);
		if(setsockopt(sockfd,SOL_SOCKET,SO_SNDBUF,&sendsize,optlength)) {
			brintf("Couldn't SET socket options, should get mangled back in place below\n");
		}
		if(getsockopt(sockfd,SOL_SOCKET,SO_SNDBUF,&sendsize,&optlength)) {
			brintf("Couldn't get socket options :( Guessing 1024\n");
			sendsize=65535;
		}
		//brintf("We are given an entity-body in http_request! Sending it along...(Buffersize: %d)\n",sendsize);
		char *bodybuf=malloc(sendsize);
		int bytesread=-1;
		while(!feof(body) && !ferror(body)) {
			bytesread=fread(bodybuf,1,sendsize,body);
			//brintf("Not at end yet, read this many bytes: %d\n",bytesread);
			int sendloop=0;
			int bytessent=0;
			do {
				sendloop=send(sockfd,bodybuf+bytessent,bytesread-bytessent,0);
				if(sendloop==-1) {
					//brintf("Error in sending (got -1)...Maybe RETRYING. ERr: %s\n",strerror(errno));
				} else {
					bytessent+=sendloop;
				}
				if(bytessent!=bytesread) {
					//brintf("A HA!!!!! Read more bytes than we sent. Whoops. Read: %d, Wrote: %d\n",bytesread,bytessent);
				}
			} while((sendloop==-1 && errno==EAGAIN) || (sendloop!=-1 && bytessent<bytesread));
			if(sendloop==-1) {
				//brintf("Nope, Big fail - sendloop is still negative one, errno must've been not EAGAIN\n");
				bodysendresults=-1;
				break; //no need to keep trying to send busted data
			}
			totalbytes+=bytessent;
		}
		free(bodybuf);
		send(sockfd,"\r\n",2,0); // I can't see why in the spec this is necessary, but it seems to be how it works...
	}
	
	//brintf("Sendresults on socket are: %d, keptalive is: %d Total bytes sent: %d\n",sendresults,keptalive,totalbytes);
	char peekbuf[8];
	int peekresults=0;
	do {
		peekresults=recv(sockfd, peekbuf, sizeof(peekbuf), MSG_PEEK);
	} while (peekresults==-1 && errno== EAGAIN );
	if(peekresults>0) {
		//brintf("Buffer peek says: %c%c%c%c\n",peekbuf[0],peekbuf[1],peekbuf[2],peekbuf[3]);
	} else {
		//brintf("peek failed :(\n");
	}
	//brintf("Recv delay from start - RECV-AFTER was: %ld\n",time(0)-start);
	if(sendresults<=0 || peekresults<=0 || bodysendresults<=0) {
		//brintf("Bad socket?! BELETING! sendresults: %d, peekresults: %d bodysendresults: %d(Reason: %s)\n",sendresults,peekresults,bodysendresults,strerror(errno));
		//either we GOT this from the keepalive pool, or we INSERTED it into the pool - either way, pull it out now!!!
		delete_keep(sockfd);
		close(sockfd);
		if(keptalive) {
			//now that we've deleted the keepalive we came from, retry the request
			//hopefully it will take the 'no keepalive found' path...and not infinite loop.
			//which would be bad.
			char modrefer[1024];
			strlcpy(modrefer,referer,1024);
			strlcat(modrefer,"+refetch",1024);
			if(body) {
				rewind(body);
			}
			return http_request(fspath,verb,etag,modrefer,extraheaders,body);
		} else {
			return badsock;
		}
	}
	httpsocket r;
	r.fd=sockfd;
	r.http11=-1;
	r.closed=-1;
	r.status=-1;
	r.encoding=unknown_encoding;
	r.contentlength=-1;
	r.headed=(strcmp(verb,"HEAD")==0);
	return r;
}

int
http_valid(httpsocket sock)
{
	return (sock.fd>0); //should this be more comprehensive? We have embryonic sockets where we don't know everything...
}

int
http_close(httpsocket *sock)
{
	if(http_valid(*sock)) {
		if(sock->http11 && ! sock->closed) {
			brintf("Returning keepalive, http11: %d, connectin-closed: %d\n",sock->http11,sock->closed);
			return_keep(sock->fd);
		} else {
			brintf("Closing and DELETING Socket HTTP11: %d, connection-closed: %d\n",sock->http11,sock->closed);
			delete_keep(sock->fd);
			close(sock->fd);
		}
		*sock=badsock;
		return 0;
	} else {
		brintf("ATTEMPT TO CLOSE INVALID HTTPSOCKET! Doing nothing...\n");
		*sock=badsock; //it was already bad, but let's make it badder.
		return 1;
	}
}

int 
recv_headers(httpsocket *mysocket,char **headerpointer/* ,void **bodypiece */)
{
	int mydesc=dup(mysocket->fd);
	//brintf("My socket is: %d, we duped that to %d. I *PROMISE* to close it, here in-function.\n",mysocket->fd,mydesc);
	FILE *web=fdopen(mydesc,"r");
	if(!web) {
		brintf("COULD NOT OPEN FILE!\n");
		exit(99); //I *have* to exit - I literally do NOT know what I can do here. Gotta exit. Sorry.
	}
	setvbuf(web,0,_IONBF,0);
	int bytesofheader=0;
	*headerpointer=calloc(1,1); //init to \0
	int i=0;
	while(i++<100) {
		char linebuf[1024]="";
		fgets(linebuf,1024,web);
		bytesofheader+=strlen(linebuf);
		//brintf("header line[%d]: strlen is %zd, header is: %d, line is :%s",i,strlen(linebuf),bytesofheader,linebuf);
		*headerpointer=realloc(*headerpointer,bytesofheader+1);
		strcat(*headerpointer,linebuf);
		if(strcmp(linebuf,"\r\n")==0) {
			char headerval[1024];
			fetchheader(*headerpointer,"connection",headerval,1024);
			mysocket->closed=(strcasecmp(headerval,"close")==0) ;
			mysocket->status=fetchstatus(*headerpointer);
			fetchheader(*headerpointer,"transfer-encoding",headerval,1024);
			if(strcasecmp(headerval,"chunked")==0) {
				mysocket->encoding=chunked;
			} else {
				mysocket->encoding=regular;
			}
			//brintf("Detected Xfer Encoding: %s, translating to: %d\n",headerval,mysocket->encoding);
			fetchheader(*headerpointer,"content-length",headerval,1024);
			mysocket->contentlength=atol(headerval);
			
			mysocket->http11=((*headerpointer)[7]=='1');
			fclose(web);
			return 1;
		}
	}
	fclose(web);
	return 0;
}

int
straight_handler(httpsocket fdesc,FILE *datafile)
{
	int blocklen=65535;
	int bytes=-1;
	int bodybytes=0;
	if(fdesc.contentlength < 65535) {
		blocklen=fdesc.contentlength;
	}
	brintf("Blockeln is: %d. File descriptor is: %d\n",blocklen,fdesc.fd);
	if(blocklen>0) {
		char *mybuffer=malloc(blocklen);
		while((bytes = recv(fdesc.fd,mybuffer,blocklen,0))) { //re-use existing buffer
			if(bytes==-1 && (errno==EAGAIN || errno==EINTR)) {
				brintf("Recv reported EAGAIN or EINTR\n");
				continue;
			}
			if(bytes<=0) {
				brintf("Recv returned 0 or -1. FAIL?: cause: %s\n",strerror(errno));
				return 0; //maybe?!?!!?
				break;
			}
			brintf("okay, we finished with teh stupid eagains. What's going on?\n");
			fwrite(mybuffer,bytes,1,datafile);
			bodybytes+=bytes;
			brintf("Read %d bytes, expecting total of %d, curerently at: %d\n",bytes,fdesc.contentlength,bodybytes);
			if(bodybytes>=fdesc.contentlength) {
				brintf("Okay, read enough data, bailing out of recv loop. Read: %ld, expecting: %d\n",ftell(datafile),fdesc.contentlength);
				free(mybuffer);
				mybuffer=0;
				break;
			}
			if(fdesc.contentlength-bodybytes>65535) {
				blocklen=65535;
			} else {
				blocklen=fdesc.contentlength-bodybytes;
			}
		}
		if (mybuffer) {
			free(mybuffer);
		}
	}
	return 1;
}

int
chunked_handler(httpsocket fdesc,FILE *datafile)
{
	int fd2=dup(fdesc.fd);
	FILE *webs=fdopen(fd2,"r");
	int chunkbytes=-1;
	if(!webs) {
		brintf("I could not open webs from file descriptor :%d\n",fdesc.fd);
	}
	brintf("Beginning Chunk HAndlings!\n");
	while(chunkbytes!=0) {
		char lengthline[16]="";
		char *badchar=0;
		if(fgets(lengthline,16,webs)==0) {
			if(feof(webs))
				brintf("EOF\n");
			if(ferror(webs))
				brintf("FERROR\n");
			fclose(webs);
			return 0; //FAIL
		}
		chunkbytes=strtoul(lengthline,&badchar,16);
		brintf("Starting a chunk, lengthline is: '%s', numerically that's: %d And 'badchar' portion of string is: '%s', badchar strlen is: %zd, [%hhd,%hhd]\n",
			lengthline,chunkbytes,badchar,strlen(badchar),badchar[0],badchar[1]);
			
		if(badchar==lengthline) {
			brintf("We handled NOTHING. This shit is TOTALLY MESSED UP\n");
			brintf("chunked - we expected a size, and got something that wouldn't parse at all\n");
			brintf("we should exit, but why don't we continue instead and read another line for fun?\n");
			chunkbytes=-1;
			continue;
		}
		
		int readbytes=0;
		char *buffer=malloc(chunkbytes);
		while(readbytes<chunkbytes) {
			int bytes=fread(buffer,1,chunkbytes,webs);
			int writtenbytes=fwrite(buffer,1,chunkbytes,datafile);
			brintf("Reading some bytes for this chunk, wanted %d, got %d\n",chunkbytes,bytes);
			//decrement some bytes...
			if(bytes!=writtenbytes) {
				brintf("ERROR - wrote fewr bytes than we read!!!!! read: %d, wrote: %d\n",bytes,writtenbytes);
			}
			readbytes+=bytes;
			if(readbytes==chunkbytes) {
				brintf("YAY! end of chunk!");
				char crlf[2];
				int r=fread(crlf,1,2,webs); (void)r;
				if(crlf[0]!='\r' || crlf[1]!='\n') {
					brintf("CRLF fail! We expected CRLF (\\r \\n, 13 10), and got (%hhd %hhd). read said: %d\n",crlf[0],crlf[1],r);
				}
				chunkbytes=-1;
			}
		}
		free(buffer);
	}
	fclose(webs);
	return 1;
}

int
contents_handler(httpsocket mysocket,FILE *fp)
{
	brintf("About to 'handle' contents. Here's what we know. Headed?: %d Status: %d, encoding: %d\n",mysocket.headed,mysocket.status,mysocket.encoding);
	if(mysocket.headed) {
		//All responses to the HEAD Method MUST NOT include a message body...
		return -1;
	}
	int httpstatus=mysocket.status;
	if((httpstatus>=100 && httpstatus <200) || httpstatus==204 || httpstatus==304) {
		//All 1xx, 204, and 304 responses MUST NOT include a message-body..
		return -1;
	} else {
		switch(mysocket.encoding) {
			case chunked:
			brintf("Handlin' chunked, baby!\n");
			chunked_handler(mysocket,fp);
			return 0;
			break;
			
			case regular:
			straight_handler(mysocket,fp);
			return 0;
			break;
			
			default:
			brintf("UNKNOWN CONTENT ENCODING! Can't handle it!");
		}
	}
	return -1;
}

void
wastebody(httpsocket mysocket)
{
	FILE *waster=fopen("/dev/null","w");
	contents_handler(mysocket,waster);
	fclose(waster);
}

