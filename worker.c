
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

#include "metacache.h"

#include "transfers.h"

#include <time.h>

typedef struct {
	char *path; //I think this is superfluous - we *know* which search you are, based on which request you are! (well, unless this changes
							// which it could, as you walk around the path to do an impossible-files detection, or whatever.
							//NO IT IS NOT
							// the searches need to 'stand alone' - when we're handling them, for example. Otherwise you'd have to walk allllll of the 
							// 'transfers' to find out what path we're looking for.
	response_t entry;
	int directoryhint:1;
	//at some point some kind of directory thing? I don't know really
	int metafd; //is this the METADATA FD? or the DATA-Data-FD?
	int datafd;
	transfer_t *transfer;
} search_t;

#define MAXSEARCHES 128

search_t searches[MAXSEARCHES];

typedef enum {
	Disconnected=0, //not connected at all
	ReceivingRequest,
	Searching,
	Transferring,
	SendingResponse
} phase_t;

typedef struct {
	int reqdesc; //request descriptor
	request_t req;
	response_t resp;
	search_t *search; //'search' also may contain 'transfer' - handled separately
	phase_t phase;
	int datafilefd;
} connection_t;

#define MAXCONNECTIONS 32

connection_t connections[MAXCONNECTIONS]; //connections from client machines. We'll be reading from and writing to these, right?

// http.c should have all of the web connections we're worrying about

char *filetypes[] ={
		"NoFile",
		"IOError",
		"File",
		"Directory", //HTML-style directory listing
		"Manifest", //Manifest directory listing
		"Symlink"
};


char *
debug_response(response_t *resp,char *context)
{
	char *tmp=0;
	asprintf(&tmp,"%s Response: filetype: %s, moddate: %d, size: %ld\n",context,filetypes[resp->filetype],resp->moddate,resp->size);
	return tmp;
}

void
init_searches(void)
{
	memset(searches,0,sizeof(searches));
}

void
finish_search(search_t *which)
{
	int i;
	int matches=0;
	if(which->directoryhint) {
		which->entry.filetype=Directory;
	}
	//is it our job to insert the found entry into the cache? I Dno't know.
	for(i=0;i<MAXCONNECTIONS;i++) {
		if(connections[i].search==which) {
			brintf("WORKER: Searches: Found someone who was waiting on this search! Yay!\n");
			char *myresp=debug_response(&which->entry,"WORKER: Searches. finish_search");
			brintf("I bet I'm not properly setting which->entry, ARGH: %s\n",myresp);
			free(myresp);
			connections[i].resp=which->entry;
			connections[i].phase=SendingResponse;
			connections[i].search=0; //MUST set this to zero so a new re-use of this will start a new search
			connections[i].datafilefd=which->datafd; //WARNING FIXME - does this mean multiple requests to read file will ALL refer to same FD?!
			//IF SO THAT COULD BE VERY VERY VERY VERY VERY BAD!!! FIXME BROKEN FIX FIXME DUP DUP2
			matches++;
		}
	}
	if(matches==0) {
		brintf("WORKER: Searches: Really weird - search finished and no one cares.\n");
	}
	free(which->path);
	which->path=0;
	close(which->metafd);
}

void
new_search(connection_t *conn)
{
	int i;
	brintf("WORKER: Searches: Looking for %s, or making a new one\n",conn->req.resource);
	int timestamp=0;
	response_t resp;
	char etag[128]="";
	if(find_entry(conn->req.resource,&resp,&timestamp,etag,128)) {
		if(time(0)-timestamp<=maxcacheage) {
			brintf("WORKER: Searches - FOUND IT IN CACHE!!!! GOING straight to Sending Response\n");
			//We *found* a cache entry, and it is FRESH (trivially, not by clever etags optimizations or other such)
			conn->resp=resp;
			conn->phase=SendingResponse;
			return; //without allocating a search or anything
		}
		//if the entry is STALE, we may use the etag to try and get a 304 response, barring that, it's a regular search
	} else {
		brintf("WORKER: Searches - Could not find in cache, trying to grab from disk... for resource %s\n",connections[i].req.resource);
		int empty=-1;
		for(i=0;i<MAXSEARCHES;i++) {
		//	brintf("WORKER: Searches: Checking Search #%d which alleges to have path: %p\n",i,searches[i].path);
			if(searches[i].path && strcmp(searches[i].path,conn->req.resource)==0) {
				brintf("WORKER: Searches: Found an existing search for your resource, using that.\n");
				conn->phase=Searching;
				conn->search=&searches[i];
				return;
			}
			//brintf("Current guess at 'empty' slot is: %d\n",empty);
			if(empty==-1 && searches[i].path==0) {
				//use the first one available...I dunno why. It makes me feel good. Hence once we set empty we don't keep re-setting it.
				brintf("WORKER: Searches: Empty slot found!: %d\n",i);
				empty=i;
			}
		}
		if(empty!=-1) {
			brintf("WORKER: Searches: It's a brand new search entry: %d\n",empty);
			searches[empty].path=strdup(conn->req.resource);
			memset(&searches[empty].entry,0,sizeof(response_t));
			//IF timestamp!=0, go straight to transferring? (trivial refresh method)
			//brintf("WORKER: Searches - We've memset to zero\n");
			//should we open() here, and mark for nonblocking? Yes, I guess it's inherent in the Search process.
			char cwd[1024];
			getcwd(cwd,1024);
			brintf("WORKER: Searches. I am a little curious as to our 'current working directory': %s\n",cwd);
			int file_or_directory=-1;
			searches[empty].metafd=open(metafile(conn->req.resource,&file_or_directory),O_NONBLOCK|O_RDONLY);
			if(searches[empty].metafd==-1) {
				brintf("WORKER: Searches. Couldn't open file as plain file: %s\n",strerror(errno));
				file_or_directory=1;
				searches[empty].metafd=open(metafile(conn->req.resource,&file_or_directory),O_NONBLOCK|O_RDONLY);
				conn->search=&searches[empty];
				if(searches[empty].metafd==-1) {
					brintf("WORKER: Searches - could not open as file OR Directory!!! %s\n",strerror(errno));
					//searches[empty].entry.filetype=NoFile;
					//finish_search(&searches[empty]);
					conn->phase=Transferring;
					conn->search->transfer=new_transfer(conn->req.resource,"GET",0,-1,0,etag,"instant_new_transfer");
					return;
				} else {
					brintf("WORKER: Searches - we've managed to open metafile as a directory instead.\n");
					searches[empty].directoryhint=1;
				}
			} else {
				brintf("WORKER: Searches - we've opened a file descriptor: %d for resource %s\n",searches[empty].metafd,conn->req.resource);
				if(file_or_directory==1) {
					searches[empty].directoryhint=1;
				} else {
					searches[empty].directoryhint=0;
				}
				conn->search=&searches[empty];
				conn->phase=Searching;
				return;
			}
			/*
			we didn't find it hot-in-cache. What can we do?
			we can walk the directory tree back towards the root - the same way that is_plausible_file does, but with directory caches
			we can do that *without* hitting disk!
			if we don't find it *then*, then we have to start reading metadata off of disk
			maybe we modify the is_plausble_file routine to ...fuck. It's going to block. Well, modify it to load stuff into cache as necessary
			So - worst-case scenario:
			1) We look in RAM cache. Don't find it. (if clause, above).
			2) We try to walk the directory hierarchy. Loading and/or evicting various directory metadata pieces, and directory -DATA-data - pieces.
			  - those loads are going to block unless we put them in the event loop. Which we'll of course have to do.
			  - maybe that means that the 'phase' becomes 'Searching', so we can keep track of where we are at, and why we're looking at 
			  - a directory or a piece of metadata or whatever
			3) Nothing comes up as fresh
			4) We go to the WEB. Mode is now 'Downloading' (should be something like 'Webbing' or something, I dunno. Whatever.)
			5) The response we get gets encached as well as written to disk (mode still Downloading? Or 'Saving'?)
			6) We send that response back to the requestor. (SendingResponse). WHEW.
		
			ALTERNATIVELY  - - - - -- 
			This stuff *only* deals with cache.
			your request gets immediately returned with "didn't find it"
			_get_resource now has the job - THREADEDLY - of finding metadata and directory-data, through ADDITIONAL queries back through
			the unix domain pipe
			directory iteration is done through here, metadata fetch is done through here.
			maybe we don't do the file descriptor thing AT ALL!!!!!!!!
			will still have to handle locks though - how will that work?
			e.g. get metadata for this element, get directory listing for that element, iterate responses...
			will, as a side-effect, encache the metadata for various directories - /a/b/c/d/e/f/g - will all get cached.
			and will also, as a side-effect, encache the various directories
			I think it's too feeble. I think it doesn't help enough. Not sure.
		
			re-formulate file_plausibility as a recursive function - e.g.
			find_plausbility("/a/b/c/d/e/f/", "g");
			????? always assume 'g' is encached or in-memory or some such. So we're really *just* waiting on the directory listing
			and if nothing good comes of that, then find_plausibility "/a/b/c/d/e/", "f", 
			etc. Either left-to-right or right-to-left, I don't remember which I decided.
			Searches becomes a high-level array same as Downloads and Requests
			more searches get done as other ones complete.
			search_for...file...directory... and the Connection can point to the search that's currently being done.
			if we hit an etag mismatch then we know it's stale and can stop the process. If we hit a 'fresh' dirlisting that's Manifest,
			and the etags continue to match then we know it's good.
			if we hit a blank etag, we're done - we have to go to web.
		
			metacache etnries will now have to have etags, and binary indicators for 'Manifest' or not. oh, wait, I have that already.
		
			so for 'g', we will need to load that metadata. That might take a while. We also need to load the directory-data. *that* might take a while
			do we also need the directory meta-data? Well, we need to know if it's manifest. And when we go to the next one, we'll want to know if *IT*
			is manifest, and we'll want to have its etag for later.
			so - for the 'searching', there is its own state-machine.
			Initializing - loading metadata for 'g'.
			Listing - loading directory data and directory meta-data for /a/b/c/d/e/f/. Can't do anything for certain till we have both.
			probably initialize all of those requests at once. Need _age_ of /a/b/c/d/e/f, *AND* Manifest-ness
			as the listing becomes available, a field gets populated in the Searches, as the listing metadata gets populated, a field gets populated
			and then finally the initial file thingee gets populated. 
			NB - these need to be COPIES of the entries from the cache, *NOT* pointers - because they could get evicted if we're thrashing.
			as part of the 'checking out the searches' bit, once we have a search with everything populated we go run through it all.
			we then set up the next search in this same 'slot' - pre-populating things like which metadata we're looking for, and so on,
			blanking ones we're not ready with.
			do we take stabs at pulling stuff out of cache? Sure.
		
			when the search is done, we walk through the list of connections to see who is waiting on it. Those guys get their states changed to
			SendingResponse (if we have something definitive) or Downloading (if we need to re-fetch or If-None-Since/Etag query).
		
			maybe we track 'metadata-loads' at root level too.
			*/
		}
	}
	brintf("WORKER: Searches: Could not find an empty search to use!\n");
}

void
check_searches(fd_set *readfds,int *maxfd)
{
	int i;
	for(i=0;i<MAXSEARCHES;i++) {
		if(searches[i].path && searches[i].metafd>-1) {
			brintf("WORKER: Searches - We have a search we're awaiting results on...search: %d, fd: %d, for resource: %s\n",i,searches[i].metafd,searches[i].path);
			BFD_SET(searches[i].metafd,readfds);
			if(searches[i].metafd>*maxfd) {
				*maxfd=searches[i].metafd;
			}
		}
	}
}

#define MAXETAG 128
//Why do we duplicate this? WHY!? WHY!? (FIXME)

char etagbuf[MAXETAG];

void
parse_headers_into_response(char *headers,response_t *resp,int *timestamp,int fd,char **etag)
{
	char responsebuf[1024];
	brintf("WORKER: Searches - Header retrieved is: %s\n",headers);
	int status=fetchstatus(headers);
	brintf("WORKER: Searches - Header - status is: %d\n",status);
	//WHOOOOOOLE crazy long switch statement a la 'crest_getattr' to figure out this thing's filetype
	fetchheader(headers,"last-modified",responsebuf,1024);
	brintf("WORKER: Searches - Header - last modified is: %s\n",responsebuf);
	resp->moddate=parsedate(responsebuf);
	fetchheader(headers,"date",responsebuf,1024);
	brintf("WORKER: Searches - Header - 'freshness' date is: %s\n",responsebuf);
	*timestamp=parsedate(responsebuf);
	fetchheader(headers,"etag",etagbuf,MAXETAG);
	if(strlen(etagbuf)>0) {
		*etag=etagbuf;
	} else {
		*etag=0;
	}
	switch(status) {
		case 304:
		brintf("ERROR - we should never be 'exposed' to a 304 header\n");
		exit(33); //fundamental structure of space and time has been distorted. I don't want to live in this world.
		break;
		
		case 200:
		case 201:
		case 204:
		brintf("WORKER: It is a file\n");
		resp->filetype=File;
		//brintf("Pre-mangulation headers: %s\n",header);
		fetchheader(headers,"content-length",responsebuf,1024);
		if(strlen(responsebuf)>0) {
			//use content-length in case this file was only HEAD'ed, otherwise seek to its end
			resp->size=atol(responsebuf);
		} else {
			if(fd == -1) {
				brintf("WEIRD! No cachefile given on a %d, and couldn't find content-length!!! Let's see if there's anything interesting in errno: %s\n",status,strerror(errno));
				resp->size=0;
			} else {
				//BLOCKING?
				brintf("WORKER: Searches - have to lseek to find end of file for response - BLOCKING\n");
				int seeker=lseek(fd,0,SEEK_END); //BLOCKING?!
				if(seeker<=0) {
					brintf("Can't seek to end of file! WTF!\n");
					exit(88); //Errr....yeah, all bets are off if taht happens
				}
				brintf("WORKER: Searches - lseeked to: %d\n",seeker);
				resp->size=seeker;
				lseek(fd,0,SEEK_SET); //BLOCKING?
			}
		}
		//brintf("Post-mangulation headers is: %s\n",header+1);
		//brintf("Post-mangulation headers is: %s\n",header);
		//brintf("BTW, date I'm trying to format: %s\n",date);
		//brintf("WEIRD...date: %s, length: %d\n",date,(int)stbuf->st_size);
		break;
		
		case 301:
		case 302:
		case 303:
		case 307:
		//http redirect, means either DIRECTORY or SYMLINK
		//start out by keeping it simple - if it ends in a
		//ZERO caching attempts, to start with...
		resp->filetype = Symlink;
		break;
		
		case 404:
		resp->filetype = NoFile;
		break;
		
		case 401:
		resp->filetype = IOError;
		break;
		
		default:
		brintf("Weird http status, don't know what to do with it!!! It is: %d\n",status);
		resp->filetype = IOError;
	}
}

#define HEADERLEN 16384
//FIXME - we are *REDEFINING* this - the CANONICAL one is in async_http.h
//HOW DO WE FIX THIS?!!?

void
handle_searches(fd_set *readfds)
{
	int i;
	for(i=0;i<MAXSEARCHES;i++) {
		if(!searches[i].path) {
			continue;
		}
		//now handle reading the results of any searches into our buffer, and parsing out the bits we care about.
		char buffer[HEADERLEN];
		if(searches[i].metafd!=-1 && FD_ISSET(searches[i].metafd,readfds)) { //don't try FD_ISSET for searches that aren't running
			brintf("WORKER: Searches - ready to read from file descriptor: %d for search %d\n",searches[i].metafd,i);
			int bytesread=read(searches[i].metafd,&buffer,sizeof(buffer));
			if(bytesread==0 || bytesread==-1) {
				brintf("WORKER: Searches - read resulted in zero or -1 (%d) bytes errno: %d, error: %s\n",bytesread,errno,strerror(errno));
				if(errno==2) { //no such file or directory. TO THE INTARNET!
					//searches[i].entry.filetype=NoFile;
					close(searches[i].metafd);
					searches[i].transfer=new_transfer(searches[i].path,"GET",0,-1,0,/*prevetag*/0,"handle_searches async_unfound_cachefile"); //there's no prevetag because there's no resource!
					//finish_search(&searches[i]);
				}
				//close(searches[i].fd);
				continue; //go on to the next thing, there's nothing else for you here.
			} else {
				if(bytesread==sizeof(buffer)) {
					brintf("Worker: Searches - WARNING - FULL BUFFER TRUNCATING LAST CHARACTER '%c'!!!\n",buffer[sizeof(buffer)-1]);
					buffer[sizeof(buffer)-1]='\0';
				} else {
					brintf("Worker: Searches - we managed to read %d bytes\n",bytesread);
					buffer[bytesread]='\0';
				}
				if(strstr(buffer,"\r\n\r\n")==0) {
					brintf("Header is NOT complete yet from read in handle_searches - don't go returning yet.\n");
					continue;
				}
				/* ((connections[i].resp.filetype==Directory || connections[i].resp.filetype==Manifest) ? 1: 0) */
				//So - how do we figure out if we open this datafile as a *file* or as a *directory*?!
				//DUMBASS - it's already set in the search. Just use the @#*$!#*$* hint.
				searches[i].datafd=open(datafile(searches[i].path,searches[i].directoryhint),O_RDONLY); //BLOCKING!
			}
		} else {
			if(!searches[i].transfer || !transfer_completed(searches[i].transfer,buffer,&searches[i].datafd)) {
				//Bad form, but *do* note that 'transfer_completed' has SIDE EFFECTS - of filling out the buffer, 
				//and filling out the datafd
				continue; //either we have no transfer or transfer is not complete
			}
			brintf("Transfer has been completed, and has given us File Descriptor #%d\n",searches[i].datafd);
		}
		int timestamp;
		char *etag=0;
		parse_headers_into_response(buffer,&searches[i].entry,&timestamp,searches[i].datafd,&etag);
		debug_response(&searches[i].entry,"WORKER: Searches - handle_searches");
		update_entry(searches[i].path,timestamp,searches[i].entry,etag);
		finish_search(&searches[i]);
	}
}

void
init_connections(void)
{
	memset(connections,0,sizeof(connections));//_should_ reset everything to 'Disconnected'...right?
}

char *phasenames[]={
	"Disconnected=0", //not connected at all
	"ReceivingRequest",
	"Searching",
	"Transferring",
	"SendingResponse"
};

void
debug_connections(void)
{
	int i;
	for(i=0;i<MAXCONNECTIONS;i++) {
		/*
			int reqdesc; //request descriptor
			request_t req;
			response_t resp;
			search_t *search;
			download_t *download;
			phase_t phase;
		 connection_t;
		*/

		brintf("Connection [%d], %s FD: %d requestRSRC: %s search: %p\n", i, phasenames[connections[i].phase], connections[i].reqdesc, connections[i].req.resource, connections[i].search);
	}
}

void
check_connections(fd_set *readfds,fd_set *writefds,int *maxfds)
{
	int i;
	for(i=0;i<MAXCONNECTIONS;i++) {
		//check reads from unix socket connections, and writes to the same
		if(connections[i].phase==ReceivingRequest) {
			brintf("WORKER: check_connections - going to listen for reads on connection #: %d\n",i);
			if(connections[i].reqdesc > *maxfds) {
				*maxfds=connections[i].reqdesc;
			}
			BFD_SET(connections[i].reqdesc,readfds);
		}
		if(connections[i].phase==SendingResponse) {
			brintf("WORKER: check_connections - going to listen for writes on connection #: %d\n",i);
			if(connections[i].reqdesc > *maxfds) {
				*maxfds=connections[i].reqdesc;
			}
			BFD_SET(connections[i].reqdesc,writefds);
		} 
	}
}

void
handle_new_connections(fd_set *readers,int *s) {
	if(FD_ISSET(*s,readers)) {
		 struct sockaddr_un remote;
		 memset(&remote,0,sizeof(remote));
		 brintf("WORKER: New connections pending! Welcome!\n");
		 unsigned int l=0;
	   int r = accept(*s, (struct sockaddr *) &remote, &l);
	   if (r == -1) {
				//our File Descriptor was marked as 'interesting for listening'
				//and yet, our accept() call has returned zero.
				//our socket MUST be disconnected. Let's stop listening for it.
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

#include <time.h>

void handle_reading_connections(fd_set *readset)
{
	int i;
	//handle reading in requests first
	for(i=0;i<MAXCONNECTIONS;i++) {
		if((connections[i].phase==ReceivingRequest) && FD_ISSET(connections[i].reqdesc,readset)) {
			brintf("WORKER: Found a file descriptor to read stuff from! Connection: %d, FD#: %d\n",i,connections[i].reqdesc);
			int bytes=recv(connections[i].reqdesc,&connections[i].req,sizeof(request_t),0);
			if(bytes==sizeof(request_t)) {
				brintf("WORKER: Request received. Resource: %s, Purpose: %s, Headers only? %d, read only? %d\n",connections[i].req.resource,connections[i].req.purpose,connections[i].req.headers_only,connections[i].req.read_only);
				connections[i].phase=Searching;
				connections[i].search=0; //redundantly setting this to zero - if you don't, it won't work.
			} else {
				if(bytes==0 || bytes==-1) {
					brintf("WORKER: Socket returned zero or -1 - is it now closed?\n");
					close(connections[i].reqdesc);
					connections[i].phase=Disconnected;
				} else {
					brintf("WORKER: Warning! Did not receive enough bytes for a full request. Got: %d, wanted: %ld :/\n",bytes,sizeof(request_t));
				}
			}
		}
		
		//handle invoking searches from a connection that is ready to do one (may have just received the request(s), in fact, above)
		if(connections[i].phase==Searching && connections[i].search==0) {
			new_search(&connections[i]); //new_search opens the file nonblockingly.
			/* FIXME - TODO
			Once new_search() is fully-aware of how to do impossible-file detection and freshness-detection (via manifest),
			we might skip this optimization of checking the cache immediately and returning - instead just run through
			new_search and trust that it will return very quickly ... not quite 'instantly' though - well, if it's not, no 'event' will happen
			and that's not good - in a land of infinite sleep() duration, that basically won't work.
			
			Anyways, some refactoring and reordering is in the cards here.
			
			NB - one quick thought is that you just hop straight into new_search, and since it has full and direct access to the
			connection, it's perfectly permitted to fill in 'resp' and change your state to SendingResponse (or something like that).
			*/
			brintf("WORKER: Searches - Active connection: %d, starting to search..\n",i);
			//TRY TO PULL STUFF OUT OF CACHE!!!!
		}				
	}
}

void
handle_writing_connections(fd_set *writers)
{
	int i;
	for(i=0;i<MAXCONNECTIONS;i++) {
		//handle writing out responses next
		if(FD_ISSET(connections[i].reqdesc,writers)) {
			//brintf("WORKER: Weird: A file descriptor is ready for writing and I don't know what to write to it. Connection: %d, FD#: %d\n",i,connections[i].reqdesc);
			brintf("WORKER: Ready to write to file descriptor: Connection: %d, fd#: %d\n",i,connections[i].reqdesc);
			struct msghdr mymsg;
			struct iovec io_vector;
			memset(&mymsg,0,sizeof(mymsg));
			mymsg.msg_iov=&io_vector;
			mymsg.msg_iovlen=1;
			char *d=debug_response(&connections[i].resp,"WORKER: handle_writing_connections ");
			brintf("WORKER: debug_response before SENDING! %s\n",d);
			free(d);
			io_vector.iov_base=&connections[i].resp; //ACTUAL response data.
			io_vector.iov_len=sizeof(response_t);
			
			char ancillary_element_buffer[CMSG_SPACE(sizeof(int))];
		  int available_ancillary_element_buffer_space = CMSG_SPACE(sizeof(int));
		  mymsg.msg_control = ancillary_element_buffer;
		  mymsg.msg_controllen = available_ancillary_element_buffer_space;
		  
		  struct cmsghdr *control_message = NULL;
		  control_message = CMSG_FIRSTHDR(&mymsg);
		  control_message->cmsg_level = SOL_SOCKET;
		  control_message->cmsg_type = SCM_RIGHTS;
		  control_message->cmsg_len = CMSG_LEN(sizeof(int));
			*((int *) CMSG_DATA(control_message)) = connections[i].datafilefd;

			brintf("WORKER: Connection - Writing to connection - opened file descriptor is: %d\n",connections[i].datafilefd);

/*			char funkybuf[65535];
			memset(funkybuf,0,65535);
			int funkybytes=read(connections[i].datafilefd,funkybuf,sizeof(funkybuf));
			brintf("WORKER: About to send FD, but first wanted to see what it refers to: Got %d bytes, and %s\n",funkybytes,funkybuf);
			int seeko=lseek(connections[i].datafilefd,0,SEEK_SET);
			brintf("WORKER: seeko: %d\n",seeko); */

			int bytessent=sendmsg(connections[i].reqdesc,&mymsg,0); //did THIS blcok?!
			//	int bytessent=send(connections[i].reqdesc,"poop",5,0);
			brintf("WORKER: Bytessent: %d\n",bytessent);
			if(bytessent!=sizeof(response_t)) {
				brintf("WORKER: unable to send full response back - wanted to send: %ld, but only sent: %d\n",sizeof(response_t),bytessent);
				if(bytessent==0 || bytessent==1) {
					brintf("Sent zero or negative-one bytes; Assuming this connection is busted?\n");
					close(connections[i].reqdesc);
					connections[i].phase=Disconnected;
				}
			} else {
				brintf("WORKER: response sent. Closing original FD.!\n");
				close(connections[i].datafilefd);
				
				connections[i].phase=ReceivingRequest; //transaction complete
				strcpy(connections[i].req.resource,"<reset>");
			}
		}
	}
}

#include <sys/stat.h>
#include <unistd.h> //for STDIN_FILENO, etc.

void
init_worker(char *socketname,char *cacheroot)
{
	unlink(socketname);
	if(fork()==0) {
		int devnull=open("/dev/null",O_RDWR);
		brintf("WORKER: /dev/null IS: %d\n",devnull);
		dup2(devnull,STDIN_FILENO);
		dup2(devnull,STDOUT_FILENO);
		dup2(devnull,STDERR_FILENO);
		close(devnull);
		devnull=-1;
		int s;
		struct sockaddr_un local;
		int len;
		int maxfds;
		
		brintf("initializing connections...\n");
		init_connections();
		brintf("initializing searches...\n");
		init_searches();
		brintf("initializing transfers...\n");
		init_transfers();
		brintf("Prepping socket\n");
		s = socket(AF_UNIX, SOCK_STREAM, 0);

		fcntl(s,F_SETFL,O_NONBLOCK); //non-blocking IO baby!

		local.sun_family = AF_UNIX;  /* local is declared before socket() ^ */
		strcpy(local.sun_path, socketname); //need full path here
	//	unlink(local.sun_path); //this is done on SERVER side
		len = strlen(local.sun_path) + sizeof(local.sun_family)+1;
		bind(s, (struct sockaddr *)&local, len);

		if(listen(s, 5)) {
			brintf("WORKER: Listen Error: %s\n",strerror(errno));
			exit(49);
		}

		//listen for things on 's' as well as everything else
		if(chdir(cacheroot)==-1) {
			brintf("WORKER: Could not change to Cache Directory! %s\n",strerror(errno));
			exit(10);
		}
		brintf("ready to enter forever loop in WORKER\n");
		fd_set readers,writers;
		while(1) {
			FD_ZERO(&readers);
			FD_ZERO(&writers);
			if(s!=-1) {
				maxfds=s; //our new-connection-listener is always there, if nothing else, *it* is the last FD
				BFD_SET(s,&readers); //and we certainly want to know about new connections coming in
			} else {
				maxfds=-1; //nothing valid to listen to! We're probably going to quit out at some point
			}

			check_connections(&readers,&writers,&maxfds);
			check_transfers(&readers,&writers,&maxfds);
			check_searches(&readers,&maxfds);

			//check_idlesockets(&readers,&writers);

			//check_uploads(&readers,&writers);

			if(s==-1) {
				brintf("WORKER: We have nothing to listen to anymore. Quitting.\n");
				exit(0);
			}

			brintf("WORKER: SELECT is about to run - maxfd is: %d\n",maxfds);
			struct timeval to;
			to.tv_sec=10;
			to.tv_usec=0;

			int results=select(maxfds+1,&readers,&writers,0,&to); //never time out
			brintf("WORKER: SELECT HAS RETURNED! Results are: %d\n",results);
			if(results==0) {
				brintf("WORKER: SELECT - Timed out!\n");
				debug_connections();
				//debug_searches();
				continue;
			}
			if(results==-1) {
				brintf("WORKER: SELECT Error - %s\n",strerror(errno));
				debug_connections();
				continue;
			}

			//I *think* we need to handle things in a sort-of inside-out manner - handle *writes* first, then processing, then reads.
			//this is because otherwise we'll end up nudging things from read, to processing, and to write, and might end up asking FD_ISSET()
			//of things we never had specifically set ourselves for that express purpose. Capiche? No.

			//handle new connections
			handle_new_connections(&readers,&s); //this is one specific socket.

			//handle existing connections
			handle_reading_connections(&readers);

			//handle existing downloads
			handle_transfers(&readers,&writers); //downloads could convert a d/l into a 'write', not sure?

			handle_searches(&readers);

			handle_writing_connections(&writers);


			//handle_uploads(); //don't know where this goes?
			brintf("WORKER: SELECT - work after select() has completed, restarting select() loop\n");
		}
	} else {
		//don't return until the socket is there; exit if it isn't after 'n' seconds (currently 10?)
		struct stat st;
		int counter=0;
		while(stat(socketname,&st)!=0 && counter<10) {
			counter++;
			sleep(1);
		}
		if(!S_ISSOCK(st.st_mode)) {
			printf("Still not a socket.\n");
			exit(1);
		}
	}
}