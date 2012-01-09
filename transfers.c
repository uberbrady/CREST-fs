
#include <sys/select.h>
#include "transfers.h"
#include "async_http.h"

#include <stdio.h>
#include "common.h"

#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

typedef enum {
	Idle=0,
//	Resolving, //unused? DNS resolution happens at the HTTP socket layer/keepalive layer, not here.
	Sending,
	Receiving,
	Finished
} transfer_status_t;

#define MAXREQUEST 16384

struct opaque_transfer {
	//what resource are we looking for? I don't think we have to know?
	//opaque-up these things?
	http_t *websock;
	// int requestbuf[MAXREQUEST];
	// int headersent;
	int filesock;
	//some sort of buffer - in case the web outpaces our disk, or vice-versa?
	int recursioncount; //to keep track of redirect loops, etc? start at 0?
	char headers[HEADERLEN];
	transfer_status_t status;
	char hostname[128];
};

//we do *NOT* speak HTTP.
//we *do* handle reading from the appropriate HTTP buffers, and writing to disk, and that kind of thing
//we *do* handle sending data when the HTTP layer tells us we should.
//we *do* have to know the size of any contents we have to send; and whether or not we'll be sending data.

#define MAXTRANSFER 32

transfer_t transfers[MAXTRANSFER];

void init_transfers()
{
	brintf("BTW, sizeof transfers is: %ld\n",sizeof(transfers));
	memset(transfers,0,sizeof(transfers));
	brintf("Initializing HTTP");
	init_http();
	//if we need to 'init' HTTP sublayer, it would happen *here*
}

transfer_t *new_transfer(char *resource,char *verb,char *headers ,int bodyfd,int bodylen,char *prevetag,char *purpose)
{
	//If we already had a connection going, this should be instantaneous. If we did not... it could take quite a few select()-loops
	//to even get there. We gotta wait to send a UDP packet, we gotta read a UDP packet response...it's a big ole pain in the neck.
	
	//maybe HTTP needs to be a similar 'layer' - we request a new 'http connection', and we 'service' them the same way or whatever...
	//and a 'transfer' needs to see if its associated HTTP connection is ready to send, or not, or whatever.
	int i;
	for(i=0;i<MAXTRANSFER;i++) {
		if(transfers[i].status==Idle) {
			
			//assemble http_t socket
			
			pathparse(resource,transfers[i].hostname,0,128,0); //FIXME - duplicated work, new_http also does this. Minor quibble. very minor.
			
			transfers[i].websock=new_http(resource,verb,headers,bodyfd,bodylen,prevetag,purpose); // or something? will either put in a new async lookup, or a full-functioning socket
			
			//open FD for ...file? tmpfile? Something?
			char fetchfilename[1024];
			strncpy(fetchfilename,datafile(resource,-1),1024); //use datafile()?
			strcat(fetchfilename,".downloadXXXXXX");
			transfers[i].filesock=open(mktemp(fetchfilename),O_RDWR|O_CREAT|O_EXCL|O_NONBLOCK);
			//do we have to check if we somehow -1'ed?
			transfers[i].status=Sending;
			return &transfers[i];
		}
	}
	return 0; //no empty transfer slots available! Try again?
}


void
check_transfers(fd_set *readfds,fd_set *writefds,int *maxfds)
{
	//brintf("Checking HTTP...\n");
	check_http(readfds,writefds,maxfds); //handle sending headers and receiving them and the appropriate state-changes. And handle dying keepalives.
	//brintf("Checking keepalives...\n");
	check_keepalives(readfds,maxfds); //handle pending keepalives that may close() on us
	//brintf("checking actual transfer records...\n");
	int j;
	for(j=0;j<MAXTRANSFER;j++) {
		//brintf("Checking transfer: #%d\n",j);
		if(transfers[j].websock && receiving_body(transfers[j].websock)) { //only listen to writeability of file IF we are receiving the body now
			if(transfers[j].filesock > *maxfds) { //does this belong in that leg? I DONT KNWO?!?!?! WHAT ABOUT IF THERE WAS A BODY?! AAAAAH?!
				*maxfds=transfers[j].filesock;
			}
			BFD_SET(transfers[j].filesock,writefds);
		}   //																							Not including the bit with the filesocket thing - that belongs here for sure
	}
}

void
handle_transfers(fd_set *readset,fd_set *writeset)
{
	int i;
	//how do we handle if the Network outstrips the disk? Keep malloc'ing new buffers?!
	handle_http(readset,writeset); //handle http-handled stuff. reading and writing headers, whatnot. Loading up chunk-buffers.

	for(i=0;i<MAXTRANSFER;i++) {
		int blocklen=0;
		void *httpdata=0;
		if(transfers[i].websock && receiving_body(transfers[i].websock) && 0!=(blocklen=get_block_http(transfers[i].websock,&httpdata))) { //got data!
			brintf("Data is available on something where we're receiving the body...\n");
			if(FD_ISSET(transfers[i].filesock,writeset)) {
				brintf("And furthermore, we're ready to write to our file! Blocklen: %d, httpdata ptr: %p\n",blocklen,httpdata);
				//so we got data, and we're ready to write it! ROCK ON.
				int writtenbytes=write(transfers[i].filesock,httpdata,blocklen);
				if(!more_data_http(transfers[i].websock,writtenbytes)) {
					//We're DONE!!!! UHm. Now what?
					brintf("WE ARE DONE TRANSFERRING!!!!!! YAY!\n");
					finish_http(transfers[i].websock,transfers[i].hostname,transfers[i].headers); //should deallocate the http
					transfers[i].websock=0;
					//FIXME - if this works now, zero byte files will likely FAIL.
					int seekout=lseek(transfers[i].filesock,0,SEEK_SET); //rewind file descriptor
					if(seekout==-1) {
						brintf("Handle_transfers seek fail?: %s(%d)\n",strerror(errno),errno);
					}
					int oldflags = fcntl(transfers[i].filesock, F_GETFL, 0); //get previous set of flags so we can disable nonblocking mode on this file
					if(oldflags==-1) {
						brintf("WORKER - Transfers - handle_transfers - ERROR - oldflags on file descriptor is -1?!\n");
					}
					oldflags &= ~O_NONBLOCK; //just turn off NONBLOCK
					if(fcntl(transfers[i].filesock,F_SETFL,oldflags)==-1) {
						brintf("WORKER - Transfers - handle_transfers - ERROR - cannot unset O_NONBLOCK on FD\n");
					}
					
					/* BEGIN FREAKY DEAKY TEST */
					char freakybuf[65535];
					memset(freakybuf,0,sizeof(freakybuf));
					int freakybytes=read(transfers[i].filesock,freakybuf,65535);
					brintf("Read %d freakybytes and got: '%s'\n",freakybytes,freakybuf);
					seekout=lseek(transfers[i].filesock,0,SEEK_SET);
					brintf("Final freaky seek says: %d\n",seekout);
					/* END FREAKY DEAKY TEST */
					transfers[i].status=Finished;
				}
				if(writtenbytes!=blocklen) {
					brintf("Stall write: %d bytes ready, only %d bytes written\n",blocklen,writtenbytes);
				}
			} else {
				brintf("Alas, we aren't ready for it\n");
			}
		}
	}
}

int transfer_completed(transfer_t *tr,char *header,int *datafd)
{
	if(tr==0) {
		brintf("WORKER: Transfers - transfer_completed - ERROR - tried to check transfer_completed on a NULL transfer!!!\n");
		return 0;
	}
	if(tr->status!=Finished) {
		brintf("WORKER: Transfers - transfer_completed - NOTICE - tried to call 'transfer_completed' on a non-completed transfer!!!\n");
		return 0;
	}
	strcpy(header,tr->headers);
	*datafd=tr->filesock;
	brintf("WORKER: Transfers - transfer_completed - Returning Data FD as: %d\n",*datafd);
	if(tr->websock) {
		brintf("WORKER: Transfers - transfer_completed - WARNING - completing transfer with a VALID websock?!!? Continuing silently...\n");
	}
	return 1;
}
