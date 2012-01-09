
#include "worker.h"
#include "metacache.h"

#include <stdio.h>

#include "common.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if 0
#define TESTMODE
#endif

#ifdef TESTMODE
#define MAXMETACACHE 3
#else
#define MAXMETACACHE 1024
#endif

#ifdef TESTMODE
void brintf(char *format,...) __attribute__ ((format (printf, 1,2)));

#include <stdarg.h>

void brintf(char *format,...)
{
	va_list myargs;
	va_start(myargs,format);
	vprintf(format,myargs);
	va_end(myargs);
	fflush(stdout);
}
#endif

#define MAXETAG 128

typedef struct entry_t {
	char *path;
	response_t entry;
	int timestamp;
	struct entry_t *next;
	char etag[MAXETAG];
	//if we want to enable 304-style requests, we might need an 'etag' here too.
} entry_t;

entry_t *metaroot=0; //why the fuck am I doing this as a list, it could just be an array at this rate. Whatever.
//I guess it dynamically allocates and that's ... interesting. And we could switch to a b-tree or something relatively easily.
//or whatever. I dunno

//appends to end, or replaces the oldest entry
void update_entry(char *path,int when,response_t what, char *etag)
{
	entry_t **where=&metaroot;
	int entrynum=0;
	entry_t *oldest=0;
	int oldest_age=INT_MAX;
	while(*where!=0 && entrynum<MAXMETACACHE) {
		if(strcmp(path,(*where)->path)==0) {
			brintf("Found existing entry! Replacing...");
			oldest=*where;
			break;
		}
		if((*where)->timestamp < oldest_age) {
			oldest=*where;
			oldest_age=(*where)->timestamp;
		}
		where=&((*where)->next);
		entrynum++;
	}
	brintf("Star-where: %p\n",*where);
	if(entrynum>=MAXMETACACHE || *where) {
		brintf("Doing oldest thing (or replace)!\n");
		//we must've run into MAXMETACACHE
		//or we found this path already in cache.
		//replace oldest entry
		where=&oldest; //we're editing an entry in-place, not modifying someone's next-pointer, so this is OK. I think.
		brintf("Cache max size reached. Deleting oldest entry. age: %d, path: %s\n",oldest->timestamp,oldest->path);
		free(oldest->path); //free previous path entry.
	} else {
		//appending to end
		*where=malloc(sizeof(entry_t));
		(*where)->next=0;
	}
	(*where)->path=strdup(path);
	(*where)->entry=what;
	(*where)->timestamp=when;
	strncpy((*where)->etag,etag,MAXETAG);
}

int find_entry(char *path,response_t *what,int *timestamp, char *etag,int etaglen)
{
	entry_t *where=metaroot;
	while(where && strcmp(where->path,path)!=0) {
		where=where->next;
	}
	if(!where) {
		return 0;
	}
	if(what) {
		*what=where->entry;
	}
	if(timestamp) {
		*timestamp=where->timestamp;
	}
	if(etag && etaglen>0) {
		strncpy(etag,where->etag,etaglen);
	}
	return 1; //either 0 if not found, or the actual entry otherwise
}

void debug_entries(void)
{
	entry_t *iter=metaroot;
	brintf("ROOT: %p\n",metaroot);
	while(iter) {
		brintf("Entry: %s, timestamp: %d, size: %ld\n",iter->path,iter->timestamp,iter->entry.size);
		iter=iter->next;
	}
	brintf("DONE\n");
}

#ifdef TESTMODE
int main(void)
{
	response_t a={.size=1},b={.size=2},c={.size=3},d={.size=4};
	printf("Before doing anything:\n");
	debug_entries();
	update_entry("blargh",1,a);
	printf("After one entry inserted:\n");
	debug_entries();
	update_entry("second",2,b);
	printf("After two entries inserted:\n");
	update_entry("third",3,c);
	printf("After three entries inserted:\n");
	debug_entries();
	printf("Inserting fourth, shoudl cause eviction of 'a'\n");
	update_entry("fourth",4,d);
	debug_entries();
	printf("Inserting replacement for 'second'\n");
	update_entry("second",99,d);
	debug_entries();
	return 0;
}
#endif
