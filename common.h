
#ifndef SHUTUP

void brintf(char *format,...) __attribute__ ((format (printf, 1,2)));

#else

#define brintf(args...) {}

#endif

#define MANIFESTTYPE "x-vnd.bespin.corp/directory-manifest"

void metafile_for_path(const char *path,char *buffer, int buflen, int isdir);

void datafile_for_path(const char *path,char *buffer, int buflen, int isdir);

void putfile_for_path(const char *path,char*buffer, int buflen);

void putdir_for_path(const char *path,char*buffer,int buflen);

int safe_flock(int filenum,int lockmode,char *filename);

void pathparse(const char *path,char *hostname,char *pathonly,int hostlen,int pathlen);

int asprintf(char **ret, const char *format, ...) __attribute__ ((format (printf, 2,3)));

#if defined(__linux__) && !defined(__dietlibc__) && !defined(__UCLIBC__)

#define gnulibc

int strlcat(char *dest, const char * src, int size);

int strlcpy(char *dest,const char *src, int size);

#endif

void directoryname(const char *path,char *dirbuf, int dirbufsize,char *basebuf, int basebufsize);

#if defined(__apple__)
FILE *
fmemopen(void *buf, size_t size, const char *mode)
#endif

void redirmake(const char *path);

FILE *fopenr(char *filename,char*mode);

extern int maxcacheage;

extern char authfile[256];

#define TOOMANYFILES 1000

void fetchheader(char *headers,char *name,char *results,int length);//http-related? 
	
int fetchstatus(const char *headers);//http-related? 

int parsedate(char *datestring);//http-related? 

char *wants_auth(const char *path); //http-related? New file?

char *rootauthurl(); //http-related? New file?

void hashname(const char *filename,char hash[22]); //need length? (belongs in resource (it's for cache-specific stuff for putting))

void fill_authorization(const char *path,char *authstring,int authlen); //http-related? New file?

void markdirty(const char *filename); //cache-related for sure.

int check_put(const char *path); //cache-related, prolly belongs in resource

void delete_from_parents(const char *path); //cache-related, belongs in resource

void append_parents(const char *path); //definitely cache related (similar to above)

void freshen_metadata(const char *path,int mode, char *extraheaders); //cache-related, also resource? 
						//specifically, newly created directories use this

void *putting_routine(void *unused); //no clue where this should go. maybe crestfs?

void invalidate_metadata(const char *metafile);

// DIRECTORY ITERATION HELPERS

#include <regex.h>

typedef struct {
	char *directory_buffer;
	regmatch_t rm[3];
	int filecounter;
	int weboffset;
} html_iterator;

typedef struct {
	FILE *fptr;
	int prevoffset;
} manifest_iterator;

typedef union {
	html_iterator htmlmode;
	manifest_iterator manifestmode;
} mode_union;

typedef enum {
	unknown=0,
	html,
	manifest
} mode_switch;

typedef struct {
	mode_switch mode;
	mode_union iterator;
} directory_iterator;

void init_directory_iterator(directory_iterator *iter,const char *headers,FILE *fp);
int directory_iterate(directory_iterator *iter,char *filename,int filenamelen,char *etag, int etaglen);
void free_directory_iterator(directory_iterator *iter); //doesn't close the file pointer you opened it, you close it godadmit!
