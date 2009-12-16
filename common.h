void brintf(char *format,...) __attribute__ ((format (printf, 1,2)));

void pathparse(const char *path,char *hostname,char *pathonly,int hostlen,int pathlen);

int asprintf(char **ret, const char *format, ...) __attribute__ ((format (printf, 2,3)));

#define DIRBUFFER	1*1024*1024
// 1 MB
#define DIRREGEX	"<a[^>]href=['\"]([^'\"]+)['\"][^>]*>([^<]+)</a>"

extern int maxcacheage;

extern char authfile[256];

#define TOOMANYFILES 1000

#include <regex.h>

void reanswer(char *string,regmatch_t *re,char *buffer,int length);

void fetchheader(char *headers,char *name,char *results,int length);
	
int fetchstatus(char *headers);

int parsedate(char *datestring);

char *wants_auth(const char *path);

char *rootauthurl();

void hashname(const char *filename,char hash[22]); //need length?

void fill_authorization(const char *path,char *authstring,int authlen);