
#include <sys/file.h>
#include "worker.h"
#include <stdio.h>
#include "http.h"

#include "common.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include <errno.h>


typedef struct {
	httpsocket websocket;
	int filesocket;
} download_t;

//a current, running connection-  has a request, and a response (starting at null), and a download pointer for if it has to download
//NB - multiple requests can all be waiting on the same download

typedef enum {
	Disconnected=0, //not connected at all
	ReceivingRequest,
	Processing,
	SendingResponse
} phase_t;

typedef struct {
	int reqdesc; //request descriptor
	request_t req;
	response_t resp;
	download_t *download;
	phase_t phase;
} connection_t;

#define MAXCONNECTIONS 32

connection_t connections[MAXCONNECTIONS]; //connections from client machines. We'll be reading from and writing to these, right?

// http.c should have all of the web connections we're worrying about

#define MAXDOWNLOADS 32

download_t downloads[32]; //

int maxfd=0;

void
check_connections(fd_set *readfds,fd_set *writefds)
{
	int i;
	for(i=0;i<MAXCONNECTIONS;i++) {
		//check reads from unix socket connections, and writes to the same
		if(connections[i].phase==ReceivingRequest) {
			brintf("WORKER: check_connections - going to listen for reads on connection #: %d\n",i);
			if(connections[i].reqdesc > maxfd) {
				maxfd=connections[i].reqdesc;
			}
			FD_SET(connections[i].reqdesc,readfds);
		}
		if(connections[i].phase==SendingResponse) {
			brintf("WORKER: check_connections - going to listen for writes on connection #: %d\n",i);
			if(connections[i].reqdesc > maxfd) {
				maxfd=connections[i].reqdesc;
			}
			FD_SET(connections[i].reqdesc,writefds);
		} 
	}
}

void
check_downloads(fd_set *readfds,fd_set *writefds)
{
	int j;
	for(j=0;j<MAXDOWNLOADS;j++) {
		//check reads from network, and writes to disk
		if(http_valid(downloads[j].websocket) && downloads[j].filesocket > -1) {
			if(downloads[j].websocket.fd > maxfd) {
				maxfd=downloads[j].websocket.fd;
			}
			if(downloads[j].filesocket > maxfd) {
				maxfd=downloads[j].filesocket;
			}
			FD_SET(downloads[j].websocket.fd,readfds);
			FD_SET(downloads[j].filesocket,writefds);
		}
	}
}

void
handle_new_connections(fd_set *readers,int *s) {
	if(FD_ISSET(*s,readers)) {
		 struct sockaddr_un remote;
		 brintf("WORKER: New connections pending! Welcome!\n");
		 unsigned int l;
	   int r = accept(*s, (struct sockaddr *) &remote, &l);
	   if (r == -1) {
				//our File Descriptor was marked as 'interesting for listening'
				//and yet, our accept() call has returned zero.
				//our socket MUST be disconnected. Let's stop listening for it.
	      //perror("accept()");
				brintf("WORKER: Accept Error %s, closing and shutting down\n",strerror(errno));
				close(*s);
				*s=-1;
	   } else {
	      brintf("WORKER: connect from unix socket, accepted!\n");
				//uhm, do I have to fcntl that 'r'? Or is it already fcntl'ed?
				int j;
				int foundconn=0;
				for(j=0;j<MAXCONNECTIONS;j++) {
					if(connections[j].phase==Disconnected) {
						brintf("WORKER: Found an unused connection to use. #%d for my new FD#: %d\n",j,r);
						connections[j].phase=ReceivingRequest;
						connections[j].reqdesc=r;
						foundconn=1;
						break;
					}
				}
				if(foundconn==0) {
					brintf("WORKER: ERROR - no more connection slots available!!!!!!");
					//exit? I dunno.
				}
	   }
	}
}

void handle_connections(fd_set *readset,fd_set *writeset)
{
	int i;
	//handle reading in requests first
	for(i=0;i<MAXCONNECTIONS;i++) {
		if((connections[i].phase!=Disconnected) && FD_ISSET(connections[i].reqdesc,readset)) {
			brintf("WORKER: Found a file descriptor to read stuff from! Connection: %d, FD#: %d\n",i,connections[i].reqdesc);
			request_t myreq;
			int bytes=recv(connections[i].reqdesc,&myreq,sizeof(myreq),0);
			if(bytes==sizeof(myreq)) {
				brintf("WORKER: Request received. Resource: %s, Purpose: %s, Headers only? %d, read only? %d\n",myreq.resource,myreq.purpose,myreq.headers_only,myreq.read_only);
				connections[i].phase=SendingResponse;
			} else {
				if(bytes==0 || bytes==-1) {
					brintf("WORKER: Socket returned zero or -1 - is it now closed?\n");
					close(connections[i].reqdesc);
					connections[i].phase=Disconnected;
				} else {
					brintf("WORKER: Warning! Did not receive enough bytes for a full request. Got: %d, wanted: %ld :/\n",bytes,sizeof(myreq));
				}
			}
		}
		
		//handle writing out responses next
		if(FD_ISSET(connections[i].reqdesc,writeset)) {
			//brintf("WORKER: Weird: A file descriptor is ready for writing and I don't know what to write to it. Connection: %d, FD#: %d\n",i,connections[i].reqdesc);
			brintf("WORKER: Ready to write to file descriptor: Connection: %d, fd#: %d\n",i,connections[i].reqdesc);
			response_t resp;
			resp.filetype=File;
			resp.moddate=1234567890;
			resp.size=111111111;
			struct msghdr mymsg;
			struct iovec io_vector;
			memset(&mymsg,0,sizeof(mymsg));
			mymsg.msg_iov=&io_vector;
			mymsg.msg_iovlen=1;
			io_vector.iov_base=&resp;
			io_vector.iov_len=sizeof(resp);
			
			char ancillary_element_buffer[CMSG_SPACE(sizeof(int))];
		  int available_ancillary_element_buffer_space = CMSG_SPACE(sizeof(int));
		  mymsg.msg_control = ancillary_element_buffer;
		  mymsg.msg_controllen = available_ancillary_element_buffer_space;
		  
		  struct cmsghdr *control_message = NULL;
		  control_message = CMSG_FIRSTHDR(&mymsg);
		  control_message->cmsg_level = SOL_SOCKET;
		  control_message->cmsg_type = SCM_RIGHTS;
		  control_message->cmsg_len = CMSG_LEN(sizeof(int));
		  *((int *) CMSG_DATA(control_message)) = open("/dev/null",O_WRONLY);
		
		  
			int bytessent=sendmsg(connections[i].reqdesc,&mymsg,0);
			if(bytessent!=sizeof(resp)) {
				brintf("WORKER: unable to send full response back - wanted to send: %ld, but only sent: %d\n",sizeof(resp),bytessent);
				if(bytessent==0 || bytessent==1) {
					brintf("Sent zero or negative-one bytes; Assuming this connection is busted?\n");
					close(connections[i].reqdesc);
					connections[i].phase=Disconnected;
				}
			} else {
				brintf("WORKER: response sent!\n");
				
				connections[i].phase=ReceivingRequest; //transaction complete
			}
		}
	}
}

void
handle_downloads(fd_set *readset,fd_set *writeset)
{
	int i;
	//how do we handle if the Network outstrips the disk? Keep malloc'ing new buffers?!
	for(i=0;i<MAXDOWNLOADS;i++) {
		if(FD_ISSET(downloads[i].websocket.fd,readset)) {
			//allocate a new buffer? Read into it? Hook it into the write list?
		//	recv(); //this needs to be wrapped somehow - simply calling recv is not gonna do it.
			//what if we're dealing with content-transfer-encoding: chunked?
		}
		if(FD_ISSET(downloads[i].filesocket,writeset)) {
			//pull from the write list, send it? free() it or something? (or return it to the pool?)
		//	send();
		}
/*		if(num_of_bytes_written >=num_of_bytes_to_write) {
			//see if there's an associated request
			//modify some stuff in there - to mark it as complete, or complete-ish
			//cache stat() thing?
			//new file descriptor - or rewind this one, or something?
			//send the stat-block, *and* the file descriptor
			//and then the connection is complete.
		} */
	}
}

void 
run_worker(char *socketname)
{
	int s;
	struct sockaddr_un local;
	int len;
	int maxfds;

	memset(connections,0,sizeof(connections));//_should_ reset everything to 'Disconnected'...right?

	s = socket(AF_UNIX, SOCK_STREAM, 0);
	
	fcntl(s,F_SETFL,O_NONBLOCK); //non-blocking IO baby!
	
	local.sun_family = AF_UNIX;  /* local is declared before socket() ^ */
	strcpy(local.sun_path, socketname); //need full path here
//	unlink(local.sun_path); //this is done on SERVER side
	len = strlen(local.sun_path) + sizeof(local.sun_family)+1;
	bind(s, (struct sockaddr *)&local, len);
	
	if(listen(s, 5)) {
		perror("Worker Listen");
		brintf("WORKER: Listen Error: %s\n",strerror(errno));
		exit(49);
	}
	
	//listen for things on 's' as well as everything else
	fd_set readers,writers;
	while(1) {
		FD_ZERO(&readers);
		FD_ZERO(&writers);
		if(s!=-1) {
			maxfd=s; //our new-connection-listener is always there, if nothing else, *it* is the last FD
			FD_SET(s,&readers); //and we certainly want to know about new connections coming in
		} else {
			maxfds=-1; //nothing valid to listen to! We're probably going to quit out at some point
		}
		
		check_connections(&readers,&writers);
		check_downloads(&readers,&writers);
		
		//check_uploads(&readers,&writers);
		
		if(s==-1) {
			brintf("WORKER: We have nothing to listen to anymore. Quitting.\n");
			exit(0);
		}
		
		int results=select(maxfd+1,&readers,&writers,0,0); //never time out
		brintf("WORKER: SELECT HAS RETURNED! Results are: %d\n",results);
		//handle new connections
		handle_new_connections(&readers,&s);
		//handle existing connections
		handle_connections(&readers,&writers);
		
		//handle existing downloads
		handle_downloads(&readers,&writers);
		
		//handle_uploads();
	}
}