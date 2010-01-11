
#define METAPREPEND		"/.crestfs_metadata_rootnode"
#define DIRCACHEFILE "/.crestfs_directory_cachenode"

int http_request(const char *fspath,char *verb,char *etag, char *referer,char *extraheaders,FILE *body);

FILE *get_resource(const char *path,char *headers,int headerlength, int *isdirectory,const char *preferredverb,char *purpose,char *mode);

void return_keep(int fd);