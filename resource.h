extern FILE *BADFILE;

#define get_resource(path,headers,headerlength,isdirectory,preferredverb,purpose,cachefilemode) _get_resource(path,headers,headerlength,isdirectory,preferredverb,purpose,cachefilemode,0)

FILE *_get_resource(const char *path,char *headers,int headerlength, int *isdirectory,const char *preferredverb,char *purpose,char *mode,int count);


