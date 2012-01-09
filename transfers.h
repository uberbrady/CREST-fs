
//this should be a relatively self-contained module having to do with transfers.

//the basic data-type is a 'transfer'.

//it is all asynchronous.
//it should have routines for modifying read and write filedescriptors for select,
//and ones for determining whether or not 'handling' downloads (transfers) as well.

//we should also track the active download list here - though, again, we don't need to expose that.

//concurrency-mangling is the responsibility of the 'search' methods. We just get requests, and perform them.
//Are we in charge of our own file-descriptors for 'writes'?
//Not really sure.

//how do we handle things like 'uploads' and whatnot? I'm not sure either.

// all HTTP-specific stuff should go in async_http - this just keeps track of files and so on.
// it seems pretty thin so far. HTTP is pretty huge.

typedef struct opaque_transfer transfer_t;

void init_transfers(void);

transfer_t *new_transfer(char *resource,char *verb,char *headers,int bodyfd,int bodylen,char *prevetag,char *purpose); 
//bodyfd can be -1 for no-body requests (common) (bodylen should be 0)

int transfer_completed(transfer_t *,char *headers,int *datafd); //if the transfer is completed, return 1, 
																																//and fill in headers with resultant headers and sets *datafd to the datafile FD.
																																//otherwise return 0

//int completed_transfer_fd(transfer_t *xfr);	//if the transfer is *not* finished, return -1, 
																						//else return the FD for the file. It's their job to close that fd. And their job to 'delete';

//void transfer_free(transfer_t *xfr); //frees the transfer object for next use. Client's resposibility, not ours.

//select() routines to set up file descriptors for reads and writes and whatnots

void check_transfers(fd_set *readers,fd_set *writers, int *maxfd); //modifies the readers and writers sets for writing to files, etc.

void handle_transfers(fd_set *readers,fd_set *writers);