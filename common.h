void brintf(char *format,...) __attribute__ ((format (printf, 1,2)));

void pathparse(const char *path,char *hostname,char *pathonly,int hostlen,int pathlen);

int asprintf(char **ret, const char *format, ...) __attribute__ ((format (printf, 2,3)));

#if defined(__linux__) && !defined(__dietlibc__) && !defined(__UCLIBC__)

int strlcat(char *dest, const char * src, int size);

int strlcpy(char *dest,const char *src, int size);

#endif

void redirmake(const char *path);

FILE *fopenr(char *filename,char*mode);

#define DIRBUFFER	1*1024*1024
// 1 MB
#define DIRREGEX	"<a[^>]href=['\"]([^'\"]+)['\"][^>]*>([^<]+)</a>"

extern int maxcacheage;

extern char authfile[256];

#define TOOMANYFILES 1000

#include <regex.h>

void reanswer(char *string,regmatch_t *re,char *buffer,int length);

int recv_headers(int fd,char **headerpointer,void **bodypiece); //http-related? 
//ALLOCATES headerpointer (fills it), and allocates and fills bodypiece and returns its size
//bodypiece is a small chunk of the body section

void fetchheader(char *headers,char *name,char *results,int length);//http-related? 
	
int fetchstatus(const char *headers);//http-related? 

int parsedate(char *datestring);//http-related? 

char *wants_auth(const char *path); //http-related? New file?

char *rootauthurl(); //http-related? New file?

void hashname(const char *filename,char hash[22]); //need length? (belongs in resource (it's for cache-specific stuff for putting))

void fill_authorization(const char *path,char *authstring,int authlen); //http-related? New file?

void markdirty(const char *filename); //cache-related for sure.

int check_put(const char *path); //cache-related, prolly belongs in resource

void invalidate_parents(const char *path); //cache-related, belongs in resource

void append_parents(const char *path); //definitely cache related (similar to above)

void faux_freshen_metadata(const char *path); //cache-related, also resource? 

void *putting_routine(void *unused); //no clue where this should go. maybe crestfs?
