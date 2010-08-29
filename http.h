
typedef enum {
  unknown_encoding=0,
  regular,
  chunked
} transfer_encoding;

typedef struct {
  int fd; //set on http_request *
  char http11; //set on recv_headers *
  char closed; //set on recv_headers *
  short status; //set on recv_headers *
  transfer_encoding encoding;  //set on recv_headers *
  int contentlength; //set on recv_headers *
  char headed; //set on http_request *
} httpsocket;

httpsocket 
http_request(const char *fspath,char *verb,char *etag, char *referer,char *extraheaders,FILE *body);

int
http_read(httpsocket,int length);

int
http_close(httpsocket *);

int
http_valid(httpsocket);

int recv_headers(httpsocket *fd,char **headerpointer);
//ALLOCATES headerpointer (fills it), and allocates and fills bodypiece and returns its size
//bodypiece is a small chunk of the body section

int
contents_handler(httpsocket fd,FILE *datafile);

void wastebody(httpsocket mysocket);

