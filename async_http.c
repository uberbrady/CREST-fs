
#include <sys/select.h>
#include "async_http.h"

#include <stdio.h>
#include "common.h"

#include <sys/socket.h>
#include <string.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <stdlib.h>


#define BUFLEN 65535

#define VERBLEN 16

typedef enum {
	Empty=0,
	Resolving,
	Connecting,
	SendingHeaders,
	SendingBody,
	ReceivingHeaders,
	ReceivingBody,
	Done
}	http_state_t;

struct opaque_http {
	int fd;
	int reqbodyfd;
	http_state_t httpstate;
	char verb[VERBLEN];
	char headers[HEADERLEN]; //null-terminated request header string *WITH* final \r\n\r\n sequence. re-used for response headers.
	char buffer[BUFLEN]; //containes either a chunk OR actual body pieces. Also used as buffer when SENDing a body
	char *progress; //How far have we gotten into the headers for send, the buffer for send, or the buffer for receive?
	char *endbuffer; //for body buffers, where does the buffer end?
	int chunked:1; //is it chunked or plain?
	int length; //content-length for 'straight' content, chunk-size for 'chunked'?
	int bodyprogress; //how far into the 'chunk' are we now, or how far into the 'body' are we now?
}; // http_t (typedef'ed in .h)

#define MAX_KEEP 128
#define HOSTLEN 128

typedef struct {
	char hostname[HOSTLEN];
	int fd;
} keepalive_t; //only insert into keeps when they're idle.

//WE are in charge of:
//HTTP header stuff
//transfer-encoding: chunked
//we have buffer-management routines so that we can 'hide' the transfer-encodinig from our consumers

//we *do* handle sending the headers ourselves. We have a headers_sent_http() routine to see if they've been sent yet.
//we *DO NOT* handle sending the body. Or calculating body size; that's your problem.
//we *do* handle receiving the headers, and *do* do some rudimentary parsing of them.
//we have a body_finished_http() method you can call when you're done sending the body
//You must always call that, even if you have no body, so we can know to send more \r\n's and start RECEIVING HEADERS
//We have a handle_http method that handles continuing to transmit the headers, continuing to receive them
//you can grab at the *decoded* body contents with a helper function get_response_http(), which may return 0 if the chunk
//isn't ready yet(?) (Why not just give you the partial chunk? You don't care?)

//We CANT PARSE MANIFESTS OR DIRECTORY LISTINGS. We do enough already.
//We *CAN* parse Connection:close
//we will PROBABLY expose some kind of keepalive-checker to make sure our kept-alive connections haven't died.

/*
Division of labor TAKE 2:
File-to-send is passed along with new_http
http handles sending header, sending body, parsing response (as appropriate), filling up chunkbufs and whatnot.

CONSUMER is in charge of: retrieving chunkbuf's (or straightbufs) from the http object, and doing something with them.
His job for overflows, too - if net is responding faster than disk, that's his problem, not ours.
chunkbuf thing needs to be binary-safe - don't use string functions if you can avoid them.
How does he apply 'backpressure' to say 'stop sending me network packets'? If, say, the chunks are coming in fast-and-furious,
and he's dutifully writing them down. But the chunk buffer is loading up faster than the disk?

pause_http(); -- will make it so the next check() doesn't look for activity on that FD.
resume_http(); -- will make it so that it *does* start looking for activity on that FD.

*/

keepalive_t keepalives[MAX_KEEP];

void check_keepalives(fd_set *readset,int *maxfd)
{
	int i;
	for(i=0;i<MAX_KEEP;i++) {
		if(keepalives[i].hostname[0]!='\0') {
			brintf("We think we have a keepalive that we want to check on. %d for hostname: %s\n",keepalives[i].fd,keepalives[i].hostname);
			BFD_SET(keepalives[i].fd,readset);
			if(keepalives[i].fd>*maxfd) {
				*maxfd=keepalives[i].fd;
			}
		}
	}
}

void handle_keepalives(fd_set *readers)
{
	int i;
	for(i=0;i<MAX_KEEP;i++) {
		if(keepalives[i].fd!=-1 && keepalives[i].hostname[0]!='\0' && FD_ISSET(keepalives[i].fd,readers)) {
			//Data showed up on a connection that should just be sitting around in 'keepalive' mode.
			//What ought we to do?
			//just blow it away.
			
			close(keepalives[i].fd);
			keepalives[i].hostname[0]='\0';
		}
	}
}

#define MAX_HTTP 16
http_t httparray[MAX_HTTP];

void init_http(void)
{
	memset(&httparray,0,sizeof(httparray));
	brintf("HTTP sublevel initiated\n");
}



int receiving_body(http_t *h)
{
	brintf("We're asking about http_t %p\n",h);
	brintf("State for that is: %d\n",h->httpstate);
	return h->httpstate==ReceivingBody || h->httpstate==Done; //Done counts as receiving body I think. Maybe. Sure. Yeah, why not.
}

int get_block_http(http_t *h,void **ptr)
{
	
	*ptr=h->progress;
	brintf("Some troubleshooting fun for get_block_http: Header starts at: %p, Progress puts us at: %p, end of buffer is at: %p\n",h->buffer,h->progress,h->endbuffer);
	brintf("In absolute terms if the Header is ZERO then Progress is: %d bytes, and end is at %d bytes\n",h->progress-h->buffer,h->endbuffer-h->buffer);
	brintf("The data actually *is*: %s\n",h->buffer);
	return h->endbuffer - h->progress; //number of bytes available in the buffer
}

int more_data_http(http_t *h,int writtenbytes)
{
	//okay! The user of our stuff here has managed to use (writtenbytes) out of our connection. This is good!
	//we need to move the 'progress' pointer along appropriately.
	//as a bonus - if they've read the ENTIRE buffer, we can point back to the beginning of the buffer for subsequent reads.
	
	h->progress+=writtenbytes;
	if(h->progress==h->endbuffer) {
		h->progress=h->buffer;
		h->endbuffer=h->buffer;
	}
	if(!h->chunked) {
		h->bodyprogress+=writtenbytes;
		if(h->bodyprogress>=h->length) {
			return 0; //no more_data
		} else {
			return 1;
		}
	} else {
		brintf("Can't handle more_data_http for chunked yet!\n");
		exit(45);
	}
}

void check_http(fd_set *readset,fd_set *writeset,int *maxfd)
{
	int i;
	for(i=0;i<MAX_HTTP;i++) {
		switch(httparray[i].httpstate) {
			case Empty:
			case Done:
			break;
			
			case Connecting:
			//we need to check for WRITABILITY of socket.
			brintf("We have a connecting socket we want to check for writability on: %d",httparray[i].fd);
			BFD_SET(httparray[i].fd,writeset);
			if(httparray[i].fd>*maxfd) {
				*maxfd=httparray[i].fd;
			}
			break;

			case Resolving:
			//do something with a-res to make sure we're listening?
			break;

			case SendingBody:
			//sendingbody ONLY gets set if there is an actual body to send. This is deliberate.
			BFD_SET(httparray[i].reqbodyfd,readset);
			if(httparray[i].reqbodyfd>*maxfd) {
				*maxfd=httparray[i].reqbodyfd;
			}
			//FALLTHROUGH - It's OK!
			case SendingHeaders:
			BFD_SET(httparray[i].fd,writeset);
			if(httparray[i].fd>*maxfd) {
				*maxfd=httparray[i].fd;
			}
			break;
			
			case ReceivingHeaders:
			case ReceivingBody:
			BFD_SET(httparray[i].fd,readset);
			if(httparray[i].fd>*maxfd) {
				*maxfd=httparray[i].fd;
			}
			break;
			
			default:
			brintf("Weird http state detected!: %d\n",httparray[i].httpstate);
		}
	}
}

void ReceiveBody(http_t *h,int newbytes)
{
	/*
	
	*/
	//for 'straight' (non-chunked) data, we just use the body-cache *AS IS* - we don't need to do anything to it.
	h->endbuffer+=newbytes; //first off, move the end-of-buffer indicator over.
	if(h->chunked) {
		brintf("No chunked support. Sorry.");
		exit(99);
	} else {
		brintf("We got some body bytes: %d\n",newbytes);
		h->bodyprogress+=newbytes;
		if(h->bodyprogress>=h->length) {
			brintf("HTTP state should be done now?\n");
			h->httpstate=Done;
/*			
			//if you want to insert into keepalive, you need to know the hostname!!!! Maybe track it in opaque_http? Dunno.
			int i;
			for(i=0;i<MAX_KEEP;i++) {
				
			} */
			//finish up, hand back the connection?
			//to be properly 'nice', we ought to do this now, but I think I'll do it 'later' - let finish_http() handle it
		}
	}
}

#include <errno.h>

void handle_http(fd_set *readset,fd_set *writeset)
{
	//handle close()'ed keepalives first, so we don't try to use any of these now-bad sockets
	handle_keepalives(readset);

	int j;
	for(j=0;j<MAX_HTTP;j++) {
		int bytecounter=0;
		char *indicator=0;
		int zeroread=0;
		
		switch(httparray[j].httpstate) {
			case Empty:
			case Done:
			//nothing interesting to do here. Move along.
			break;
			
			case Resolving:
			//check for FD_ISSET() something
			//do receiving things. Resolve resolvey things
			//if you're done resolving, move along to SendingHeaders
			break;
			
			case Connecting:
			brintf("There's a connecting socket out there...is it ready to write?\n");
			if (FD_ISSET(httparray[j].fd, writeset)) {
				brintf("FD is set for write for connecting HTTP socket..\n");
				int error=0;
				unsigned int len = sizeof(error);
				if (getsockopt(httparray[j].fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
					brintf("Connection error: %d with errno: %d, msg: %s\n",error,errno,strerror(errno));
					exit(11);
				} else {
					httparray[j].httpstate=SendingHeaders;
				}
			}
			break;
			
			case SendingHeaders:
			if(FD_ISSET(httparray[j].fd,writeset)) {
				brintf("I am trying to send headers: %s from offset: %d\n",httparray[j].headers,httparray[j].progress-httparray[j].headers);
				bytecounter=send(httparray[j].fd,httparray[j].progress,strlen(httparray[j].progress),0);
				httparray[j].progress+=bytecounter;
				if(strlen(httparray[j].progress)==0) {
					if(httparray[j].reqbodyfd!=-1) {
						brintf("Moving to SendingBody because we have a body FD of: %d\n",httparray[j].reqbodyfd);
						httparray[j].httpstate=SendingBody;
						httparray[j].progress=httparray[j].buffer; //reset progress to start of buffer
						httparray[j].endbuffer=httparray[j].endbuffer;
					} else {
						brintf("Going straight to ReceivingHeaders, no body\n");
						httparray[j].httpstate=ReceivingHeaders;
						httparray[j].headers[0]='\0'; //reset headers to length zero
					}
				}
			}
			break;
			
			case SendingBody:
			if(FD_ISSET(httparray[j].reqbodyfd,readset)) {
				//read into buffer, taking care not to overwrite what we already have
				int bytesread=read(httparray[j].reqbodyfd,httparray[j].progress,BUFLEN-(httparray[j].progress-httparray[j].buffer));
				httparray[j].endbuffer+=bytesread;
				if(bytesread==0) {
					zeroread=1;
				}
				//append to END of buffer.
				//what if that read was zero?
			}
			if(FD_ISSET(httparray[j].fd,writeset)) {
				//send bodybuffer to Intarwebs
				int sentbytes=send(httparray[j].fd,httparray[j].progress,httparray[j].endbuffer-httparray[j].progress,0);
				if(sentbytes==(httparray[j].endbuffer-httparray[j].progress)) {
					//we sent the entire buffer! Reset the end of buffer and progress counter back to the start of the buffer!
					httparray[j].endbuffer=httparray[j].buffer;
					httparray[j].progress=httparray[j].buffer;
					if(zeroread) {
						//DONE SENDING BODY! WHOOP!
						//do I need to send a \r\n as well? I don't remember how it works in the spec.
						//if so - what if the send buffers are full?
						//maybe only run the state change 'if' 
						if(send(httparray[j].fd,"\r\n",2,0)==2) {
							close(httparray[j].reqbodyfd); //should I be doing this? Yes. How's the other guy gonna know when I'm done?
							httparray[j].httpstate=ReceivingHeaders;
							httparray[j].headers[0]='\0';
						}
					}
				} else {
					//we did *not* send the entire buffer. BOO.
					httparray[j].progress+=sentbytes;
				}
			}
			break;
			
			case ReceivingHeaders:
			if(FD_ISSET(httparray[j].fd,readset)) {
				int headerssofar=strlen(httparray[j].headers);
				bytecounter=recv(httparray[j].fd,httparray[j].headers+headerssofar, HEADERLEN-headerssofar-1,0);
				if(headerssofar>=HEADERLEN) {
					brintf("TOO BOCOUP! Header is too long\n");
					exit(53);
				}
				httparray[j].headers[bytecounter+headerssofar]='\0';
				if(0!=(indicator=strstr(httparray[j].headers,"\r\n\r\n"))) {
					//copy the remainder into the bodybuf, and make sure to parse out the transfer-encoding: chunked if it's there
					int bodybytes=bytecounter-(indicator-(httparray[j].headers+headerssofar))-4; //need to compensate for the \r\n\r\n we just found
					memcpy(httparray[j].buffer,indicator+4,bodybytes);
					*(indicator+4)='\0';//clip off the indicator
					brintf("RECEIVED HEADERS!!! They are: %s\n",httparray[j].headers);
					brintf("Remnant is: %d bytes:  %s\n",bodybytes,httparray[j].buffer);
					//parse_headers_somehow(); //important if nothing else to determine if this is chunked or not. Also will want to fill a metacache elem
					char transfer_encoding[128];
					fetchheader(httparray[j].headers,"transfer-encoding",transfer_encoding,128);
					if(strncasecmp(transfer_encoding,"chunked",128)==0) {
						httparray[j].chunked=1;
						httparray[j].length=-1; //sentinel that means 'find next chunk size?'
					} else {
						//in this case, we need to know the TOTAL length so we can be sure we read only that number of bytes (and maybe a \r\n)
						char content_length[128];
						fetchheader(httparray[j].headers,"content-length",content_length,128);
						httparray[j].chunked=0;
						httparray[j].length=atoi(content_length);
					}
					httparray[j].bodyprogress=0; //yes, zero. the bodybits will get handled in ReceiveBody
					httparray[j].endbuffer=httparray[j].buffer; //this will be adjusted later by the call to ReceiveBody
					httparray[j].progress=httparray[j].buffer;
					if(should_have_body(httparray[j].verb,fetchstatus(httparray[j].headers))) {
						httparray[j].httpstate=ReceivingBody;
						ReceiveBody(&httparray[j],bodybytes);
					} else {
						httparray[j].httpstate=Done;
					}
				}
			}
			break;
			
			case ReceivingBody:
			bytecounter=recv(httparray[j].fd,httparray[j].endbuffer,BUFLEN-(httparray[j].endbuffer-httparray[j].buffer),0);
			brintf("RECV called for regular REceivingBody Thing - %d bytes retrieved\n",bytecounter);
			ReceiveBody(&httparray[j],bytecounter);
			break;
		}
	}
}

char headerbuf[HEADERLEN];

char *assemble_header(char *verb,char *fspath,char *extraheaders,char *purpose,char *etag)
{
	//extraheadersbuf should either be blank, or be terminated with \r\n
	char extraheadersbuf[HEADERLEN];
	if(etag && etag[0]!='\0') {
		strcpy(extraheadersbuf,"If-None-Match: ");
		strlcat(extraheadersbuf,etag,HEADERLEN);
		strlcat(extraheadersbuf,"\r\n",HEADERLEN);
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
	char hostpart[80];
	char pathpart[1024];
	pathparse(fspath,hostpart,pathpart,80,1024);
	
	snprintf(headerbuf,HEADERLEN,"%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: CREST-fs/2.0\r\nReferer: %s\r\n%s\r\n",verb,pathpart,hostpart,purpose,extraheadersbuf);
	return headerbuf;
}

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>

int synchronous_lookup(char *hostpart)
{
	int rv;
	int sockfd=-1;
	struct addrinfo hints, *servinfo=0, *p=0;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	brintf("WORKER: synchronous resolution of %s\n",hostpart);
	if ((rv = getaddrinfo(hostpart, "80", &hints, &servinfo)) != 0) {
		//brintf("Getaddrinfo timing test: FAIL-AFTER: %ld - couldn't lookup %s because: %s\n",
		//	time(0)-start,hostpart,gai_strerror(rv));
		return -1;
	}
	//brintf("Got getaddrinfo()...GOOD-AFTER: %ld\n",time(0)-start);

	// loop through all the results and connect to the first we can
	for(p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype,
				p->ai_protocol)) == -1) {
			continue;
		}
		struct timeval to;
		memset(&to,0,sizeof(to));
		to.tv_sec = 3;
		to.tv_usec= 0;
		if(setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to))) {
			brintf("Could not set RECEIVE timeout: %s(%d)\n",strerror(errno),errno);
			abort();
		}
		if(setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to))) {
			brintf("Could not set SEND timeout?: %s(%d)\n",strerror(errno),errno);
			abort();
		}
		if(fcntl(sockfd, F_SETFL, O_NONBLOCK)==-1) {
			brintf("Couldn't set socket to be nonblocking: %s(%d)\n",strerror(errno),errno);
		}
	
		struct timeval tget;
		memset(&tget,0,sizeof(tget));
		unsigned int sizething=sizeof(tget);
		if(getsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tget,&sizething)) {
			brintf("Could NOT *GET* receive timeouts %s(%d)\n",strerror(errno),errno);
			abort();
		}
		//brintf("Gotten socket options - seconds: %d, usec: %d. Options size: %d (vs sizeof at %ld) \n",
		//	(int)tget.tv_sec, (int)tget.tv_usec, sizething,sizeof(tget));
		if(getsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tget,&sizething)) {
			brintf("Could not *GET* send timeouts %s(%d)\n",strerror(errno),errno);
			abort();
		}
		//brintf("Gotten socket options - seconds: %d, usec: %d\n. Options size: %d (vs sizeof %ld) \n",
		//	(int)tget.tv_sec, (int)tget.tv_usec, sizething, sizeof(tget));

		if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1 && errno!= EINPROGRESS) {
			close(sockfd);
			brintf("Hostname which we couldn't connect to: %s %s(%d)",hostpart,strerror(errno),errno);
			continue;
		} else {
			brintf("CONNECTED! SOCKET IS: %d\n",sockfd);
			return sockfd;
		}
	}
	//looped through, found nothing. BAIL.
	close(sockfd);
	return -1;
}


http_t *new_http(char *resource,char *verb,char *headers,int bodyfd,int bodylen,char *prevetag,char *purpose)
{
	//if you can find a keepalive, use it.
	int i;
	for(i=0;i<MAX_HTTP;i++) {
		//find a blank http.
		if(httparray[i].httpstate==Empty) {
			http_t *ht=&httparray[i];
			//Either use a keepalive or start a new connection (async, of course). If you are using a keepalive, change state to SendingHeaders
			//assemble headers into header piece
			char lengthheader[1024]="";			
			ht->reqbodyfd=bodyfd;
			if(bodyfd!=-1) {
				//do *I* have to handle adding a Content-Length header to the request? Who the fuck else will?! I *AM* HTTP! DAMMIT!
				snprintf(lengthheader,1024,"Content-Length: %d\r\n",bodylen);
				if(headers==0) {
					headers=lengthheader;
				} else {
					strncat(lengthheader,headers,1024);
					headers=lengthheader;
				}
			}
			strncpy(ht->headers,assemble_header(verb,resource,headers,purpose,prevetag),HEADERLEN);
			ht->progress=ht->headers;
			strncpy(ht->verb,verb,VERBLEN);
			int j;
			char hostname[80];
			char urlpath[1024];
			pathparse(resource,hostname,urlpath,80,1024);
			for(j=0;j<MAX_KEEP;j++) {
				if(strcmp(keepalives[j].hostname,hostname)==0) {
					ht->httpstate=SendingHeaders;
					ht->fd=keepalives[j].fd;
					//yank the entry out of keepalives.
					keepalives[j].hostname[0]='\0';
					keepalives[j].fd=-1;
					return ht;
				}
			}
			//couldn't find a keepalive to use, better create a new HTTP connection
			//INSERT A-RES INIT OR SOMETHING HERE *INSTEAD* !!!!!!!!!
			ht->fd=synchronous_lookup(hostname);
			ht->httpstate=Connecting;
			return ht;
		}
	}
	return 0;
}

void finish_http(http_t *h,char *hostname,char *headers)
{
	int j;
	//insert into keepalives
	for(j=0;j<MAX_KEEP;j++) {
		if(keepalives[j].hostname[0]=='\0') {
			strncpy(keepalives[j].hostname,hostname,HOSTLEN);
			keepalives[j].fd=h->fd;
			h->fd=-1;
			break;
		}
	}
	if(h->fd!=-1) {
		//couldn't insert it into keepalives, just close it
		brintf("Could not insert active connection into keepalives! Closing\n");
		close(h->fd);
		h->fd=-1;
	}
	strncpy(headers,h->headers,HEADERLEN);
	h->httpstate=Empty;
}
