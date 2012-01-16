void init_http(void);

void check_keepalives(fd_set *readers,int *maxfd); // check keepalive table for keeps not-in-use - maybe should be 'last thing called'

void handle_keepalives(fd_set *readers); // handle keepalives that aren't in use in case they've close()'ed or something

typedef struct opaque_http http_t;

#define HEADERLEN 16384

void check_http(fd_set *r,fd_set *w,int *maxfd);

void handle_http(fd_set *r,fd_set *w);

char *assemble_header(char *verb,char *fspath,char *extraheaders,char *purpose,char *etag);

http_t *new_http(char *resource,char *verb,char *headers,int bodyfd,int bodylen,char *prevetag,char *purpose);

void finish_http(http_t *,char *hostname,char *xferheader);

int get_block_http(http_t *,void **); //returns bytes available in block pointed to by void *, you do NOT deallocate pointer.

//int ready_http(http_t *); //returns 1 if http connection is ready to go

//int request_sent_http(http_t *); //returns 1 if the headers have been sent.

int receiving_body(http_t *);

int more_data_http(http_t *,int writtenbytes); //pass in the number of bytes you actually used out of the HTTP connection.
//returns 1 if there is more data to be had, or 0 if you've received it all

int has_body_http(http_t *); //returns 1 if you're supposed to have a body

char *metatmp_http(http_t *); //returns the tempfile that was created via http
