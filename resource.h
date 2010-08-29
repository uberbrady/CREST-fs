
#define METAPREPEND		"/.crestfs_metadata_rootnode"
#define DIRCACHEFILE "/.crestfs_directory_cachenode"

FILE *get_resource(const char *path,char *headers,int headerlength, int *isdirectory,const char *preferredverb,char *purpose,char *mode);


