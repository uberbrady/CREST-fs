void init_worker(char *socketname,char *cachepath);

typedef struct {
	char resource[1024];
	char purpose[128];
	int headers_only:1;
	int read_only:1;
} request_t;

//that's the request - the response will be:

typedef enum {
	NoFile=0,
	IOError,
	File,
	Directory, //HTML-style directory listing
	Manifest, //Manifest directory listing
	Symlink
	//Manifest2 would go here, for example. I think. Maybe it doesn't matter.
} filetype_t;

typedef struct {
	filetype_t filetype; //Directory, file, symlink? NOTHING?
	int moddate; //unix epoch style times
	long int size; //should that be some 64-bit contraption?
	
//File descriptor for the contents.
//YOUR job to close it. Nyah.
} response_t;

char *debug_response(response_t *resp,char *context); //returns a NEW MALLOC STRING - YOU MUST free() IT!
