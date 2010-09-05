#include <sys/file.h> //for flock?

/* Given, a 'path', of which the first element is a hostname, and the rest are path elements,
   Check the cache and refresh it (if necessary), returning finally a FILE * (read-only), which is
   the Content part of the HTTP resource. The headers can optionally be loaded into the headers buffer, (which can be 0),
   up to headerlength.

   This function will be aggressive in terms of pre-fetching data it believes could be useful, or that has been useful in the past. It
   may convert a HEAD request into a GET request if it feels it necessary. It may make no requests at all if the caches are recent enough.

   It probably won't know the difference between a file and a directory, nor will it know how to convert path names into cachefile names
   That's the responsibility of the caller. No, wait, in the case where there is not yet an existing cache file or directory, the caller
   won't know either!!! The idea is we want to 'hide' the cache from callers, so they don't need to know how it's organized or works, they
   can just depend upon this function to return a file pointer they can walk through. So it will be cleverer than I said.

   nonexistent resources will return 0

	404 cachefiles - interesting problem. If I ask for a completely stupid path - /a/b/c/d - my algorithm will look for /a/b/c.
			since there's no /a/b, I won't be able to drop a 404 file for /a/b/c. If stat()'s always walk up the dirpath
			that's probably okay, but I am not sure about that.
 */

#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <libgen.h> //for 'dirname'
#include <utime.h>

#include "resource.h"
#include "common.h"
#include "http.h"
//path will look like "/domainname.com/directoryname/file" - with leading slash

/*
	examples:
	
	desk.nu/infinix <--resource is actually a directory.
	
	check caches
*/

#include <time.h>

int
impossible_file(const char *origpath)
{
	char path[1024];
	strlcpy(path,origpath,1024);
	strlcat(path,"/",1024); //just to fool the while loop, I know it's ugly, I'm sorry.
	char *slashloc=(char *)path+1;
	
	//first, is file on the upload list? If so, cannot be impossible.
	if(check_put(origpath)) {
		return 0; //cannot discount a newly-put file as 'impossible'
	}

	//brintf("TESTIG FILE IMPOSSIBILITY FOR: %s\n",slashloc);
	
	while((slashloc=strchr(slashloc,'/'))!=0) {
		char foldbuf[1024];
		char metafoldbuf[1024];
		char dirnamebuf[1024];
		char basenamebuf[1024];
		char *dn=0;
		char *bn=0;
		int slashoffset=slashloc-path;
		FILE *metaptr;
		FILE *dataptr;
		
		strlcpy(foldbuf,path,1024);
		foldbuf[slashoffset]='\0'; //why? I guess the pointer is being advanced PAST the slash?
		//brintf("Testing component: %s, ",foldbuf);
		strlcpy(dirnamebuf,foldbuf,1024);
		dn=dirname(dirnamebuf);
		
		strlcpy(basenamebuf,foldbuf,1024);
		bn=basename(basenamebuf);

		//brintf("Component: %s, basename: %s, dirname: %s\n",foldbuf,bn,dn);
		
		strlcat(dirnamebuf,DIRCACHEFILE,1024);
		strlcpy(metafoldbuf,METAPREPEND,1024);
		strlcat(metafoldbuf,"/",1024);
		strlcat(metafoldbuf,dirnamebuf,1024);
		
		//brintf("metafolder: %s, dirname is: %s\n",metafoldbuf+1,dirnamebuf+1);
		
		if((metaptr=fopen(metafoldbuf+1,"r"))) {
			//ok, we opened the metadata for the directory..
			char headerbuf[65535]="";
			struct stat statbuf;
			
			fstat(fileno(metaptr),&statbuf);
			int hdrlen=fread(headerbuf,1,65535,metaptr);
			fclose(metaptr);
			headerbuf[hdrlen+1]='\0';
			//brintf("Buffer we are checking out is: %s",headerbuf);
			if(time(0) - statbuf.st_mtime <= maxcacheage && statbuf.st_size > 8) {
				//okay, the metadata is fresh...
				//brintf("Metadata is fresh enough!\n");
				if((dataptr=fopen(dirnamebuf+1,"r"))) {
					//okay, we managed to open the directory data...
					char foundit=0;
					char filename[1024];
					directory_iterator iter;
					init_directory_iterator(&iter,headerbuf,dataptr);
					while(directory_iterate(&iter,filename,1024,0,0)) {
						brintf("Comparing %s to %s\n",filename,bn);
						if(strcmp(filename,bn)==0) {
							foundit=1;
							break;
						}
					}
					free_directory_iterator(&iter);
					fclose(dataptr);
					if(foundit==0) {
						brintf("The file %s is impossible",origpath);
						return 1;
					}
					brintf("The file %s doesn't seem impossible by %s",origpath,bn);	
				} else {
					//brintf("Can't open directory contents file.\n");
				}
			} else {
				//brintf("Metadata file is too stale to be sure\n");
			}
		} else {
			//brintf("Could not open metadata file %s\n",metafoldbuf+1);
		}

		slashloc+=1;//iterate past the slash we're on
	}
	//brintf("File '%s' appears not to be impossible, move along...\n",origpath);
	return 0; //must not be impossible, or we would've returned previously
}



#include <utime.h>
#include <sys/stat.h>

void directory_freshen(const char *path,char *headers,FILE *dirfile)
{
	char contenttype[1024];
	
	fetchheader(headers,"content-type",contenttype,1024);
	
	if(strcasecmp(contenttype,"x-vnd.bespin.corp/directory-manifest")==0) {
		brintf("RECEIVED MANIFEST - MANIPULATING CACHED ITEMS!\n");
		
		directory_iterator iter;
		char etag[128];
		char filename[1024];
		char headerbuf[8192]="";
		char fileetag[1024];
		struct utimbuf stamp;
		init_directory_iterator(&iter,headers,dirfile);
		while(directory_iterate(&iter,filename,1024,etag,128)) {
			if(strlen(etag)==0) {
				brintf("BLANK etag for filename %s, skipping!\n",filename);
				continue;
			}
			char direlem[2048];
			snprintf(direlem,2048,"%s%s/%s" ,METAPREPEND,path,filename);
			struct stat typeit;
			if(lstat(direlem+1,&typeit)==0 && S_ISDIR(typeit.st_mode)) {
				strncat(direlem,DIRCACHEFILE,2048); //already has a slash in front of it...GRRR
			}
			FILE *metafile=fopen(direlem+1,"r"); //lock your damned self you ingrate
			brintf("I would like to open: %s, please? Results: %p\n",direlem+1,metafile);
			if(metafile) {
				int hbytes=fread(headerbuf,1,8192,metafile);
				headerbuf[hbytes+1]='\0';
				fclose(metafile);
				fetchheader(headerbuf,"etag",fileetag,1024);
				struct stat oldtime;
				stat(direlem+1,&oldtime);
				stamp.actime=oldtime.st_atime; //always leave atimes alone! This says nothing about 'access'!
				int timeresults=-1;
				if(strcmp(etag,fileetag)==0) {
					stamp.modtime=time(0); //NOW!
					brintf("%s UTIMING NEW!\n",filename);
					//utime(direlem+1,&stamp); //Match! So this file is GOOD as of right now!
					timeresults=utime(direlem+1,0);
				} else {
					stamp.modtime=0; //EPOCH! LONG TIME AGO!
					brintf("%s UTIMING OLD!\n",filename);
					timeresults=utime(direlem+1,&stamp); //unmatch! This file is BAD as of right now, set its time BACK!
				}
				brintf("Results of stamping were: %d\n",timeresults);
			} else {
				brintf("NO METAFILE I COULD OPEN FOR %s - doing *NOTHING!!!*\n",direlem+1);
			}
			//check etag for that file.
			//if it MATCHES, touch the file
			//otherwise, UNTOUCH the file
			//the 'file' may be a directory
		}
		//rewind(dirfile); //doesn't 'free' do this on its own?
		free_directory_iterator(&iter);
	}
}

int dont_fclose(FILE *f)
{
	brintf("actually fclosing File: %p\n",f);
	return fclose(f);
}

FILE *
get_resource(const char *path,char *headers,int headerlength, int *isdirectory,const char *preferredverb,char *purpose,char *cachefilemode)
{
	//first, check cache. How's that looking?
	struct stat cachestat;
	char webresource[1024];
	char cachefilebase[1024];
	char headerfilename[1024];
	char selectedverb[80];
	FILE *headerfile=0;
	char etag[256]="";
	httpsocket mysocket;
	char *slashlessmetaprepend=0;
	int dirmode=0;
	int dontuseetags=0;
	
	char *headerbuf=calloc(65535,1);

	headerbuf[0]='\0';
	if(preferredverb==0) {
		preferredverb="GET";
	}
	
	strncpy(cachefilebase,path+1,1024); //default non-directory path for a cache file...
	strncpy(webresource,path,1024); //default web resource corresponding...
	slashlessmetaprepend=METAPREPEND;
	slashlessmetaprepend++;
	strncpy(headerfilename,slashlessmetaprepend,1024);
	strncat(headerfilename,path,1024); //need the prepended '/' from path, not cachefilebase

	strncpy(selectedverb,preferredverb,80);
	//if there is an etag, and the request is a HEAD, upgrade to a GET.
		
	*isdirectory=0; //we can say this because the ONLY way to really 'get' a directory is to GET directory+'/', which requires
			//a reinvoke of this function (witht the appropriate cache directory already in place)
			//if that happens, it will appropriately rewrite isdirectory.
			//if the resource you're looking for turns out to actually *be* a directory, this function will get reinvoked
			//this means we could get into an infinite loop if a you request a resource "foo" that returns a redirect to "foo/"
			//(which implies it's a directory), and then when you try to GET foo/ , you get a 404 - this will loop forever.
	//FIXME - better than this would be to see inthe directory listing somewher eif this entry exist
	//and has a / after it - if so, hint it to being a directory!!!!!!!!
	//that will improve 'raw' ls -lR performance
	char host[1024];
	char pathpart[1024];
	pathparse(path,host,pathpart,1024,1024);
	brintf("Dir test: Host: %s, pathpart: '%s'\n",host,pathpart);
	if((lstat(path+1,&cachestat)==0 && S_ISDIR(cachestat.st_mode)) || strcmp(pathpart,"/")==0) { 
		//note we don't use stat() time for anything!!! we're just checking for directory mode.
		//cache file/directory/link/whatever *does* exist, if it's a directory, push to 'directory mode'
		//that's all we do with this 'stat' value - the bulk of the logic is based on *header* data, not data-data.
		//NB - if the path part of the 'path' is exactly "/", then we *KNOW* this is a directory (the root one for that host.)
		//brintf("Cache entity seems to exist? (or we're the root of some host)\n");
		brintf("ENGAGING DIRECTORY MODE - because the cache entity *IS* a directory!\n");
		// resource we'll need to HEAD or GET will need '/' appended to it
		// cachefile with stuff in it we'll care about will be (path+1)+"/.crestfs_directory_cachenode"
		// with HTTP headers in .crestfs_metadata_rootnode/(path+1)+"/.crestfs_directory_cachenode"
		
		//and FURTHERMORE - the type we're assuming this resource is may no longer be true - 
		//a file could've converted into a directory or some such!
		strncat(cachefilebase,DIRCACHEFILE,1024); //need to append the "/.crestfs_directory_cachenode"	
		strncat(webresource,"/",1024);
		strncat(headerfilename,DIRCACHEFILE,1024); //append same to metadata filename?
		if(isdirectory) {
			*isdirectory=1;
		}
		dirmode=1; //regardless of *isdirectory, I need to know this if we 404 later.
		// I MIGHT LIKE TO DO THE 'MODE-escalation' and etags stuff here? But I haven't *READ* the headers yet...
		//don't matter. Whether or not I got etags, I want this directory's contents
		if(strcmp(preferredverb,"HEAD")==0) {
			strcpy(selectedverb,"GET");
		}
	} else {
		//if entity does not exist, so we do NOT want to start using etags for things!
		if(lstat(path+1,&cachestat)!=0) {
			dontuseetags=1;
		}
	}
	
	//see if we have a cachefile of some sort. how old is it? Check the st_mtime of the _headerfile_ (the metadata)
	//does this need to be LOCKed??????????  ************************  read LOCK ??? ***********************
	headerfile=fopenr(headerfilename,"r+"); //the cachefile may not exist - we may have just had the 'real' file, or the directory
						//if so, that's fine, it will parse out as 'too old (1/1/1970)'
	if(headerfile) {
		struct stat statbuf;
		int s=-1;
		safe_flock(fileno(headerfile),LOCK_SH,headerfilename);
		s=stat(headerfilename,&statbuf); //assume infallible
		//read up the file and possibly fill in the headers buffer if it's been passed and it's not zero and headerlnegth is not 0
		int hdrbytesread=fread(headerbuf,1,65535,headerfile);
		headerbuf[hdrbytesread+1]='\0';
		//brintf("Headers from cache are: %s, statresults is: %d\n",headerbuf,s);
		if(!dontuseetags) {
			//only time we DONT want to use etags is if the original cache entity (the data file)
			//didn't exist at all. an etags request would wipe out additional header info that could be useful
			//e.g. content-length. Wiping out that info is totally fine if you have the actual data - you can just
			//seek to the end of it to see how big it is. But if you only HEAD'ed the file in the past, it sucks.
			fetchheader(headerbuf,"etag",etag,256);
		}

		if(strlen(etag)>0 && strcmp(selectedverb,"HEAD")==0) {
			//we asked to HEAD this file, but we have an etag on file. We'll upgrade to a GET.
			strncpy(selectedverb,"GET",80);
		}
		
		int has_file_been_PUT=check_put(path);
		int is_directory_pending_uploads=dirmode & (statbuf.st_nlink>1);
		brintf("Has the file been PUT? Answer: %d\n",has_file_been_PUT);
		brintf("Is directory pending for uploads? %d\n",is_directory_pending_uploads);
		
		if((time(0) - statbuf.st_mtime <= maxcacheage && statbuf.st_size > 8) || has_file_been_PUT || is_directory_pending_uploads) {
			//our cachefile is recent enough and big enough, or there's been an intervening PUT
			FILE *tmp=0;
			/* If your status isn't 200 or 304, OR it actually is, but you can succesfully open your cachefile, 
					then you may return succesfully. Or if it's a HEAD reequest (where you don't care about such things)
			*/
			/* okay, there's a bug here and I don't know what to do about ti:
				if you fetch a file, and your cache is old, you will call out a new If-none-match request, and update your 
				metadata cache. Great. If you then ls -al the file BEFORE the cache has expired, it will show up as having
				0 bytes. Why? Because your second request went through this short-circuit routine. This if-clause saw
				that your selected verb was 'HEAD' (you're asking for getattr when you ls -al a file), so it never bothers
				to open a file. When getattr gets its data back, first thing it tries is to look at the headers for a 
				content-length. There ain't one, because the last fetch you did just returned a 304. So next it tries to run
				to the end (using fseek/ftell) of the data-file pointer. Which is zero. Then it just gives up.
				
				one fix is to make crest_getattr always GET instead of HEAD - then it would have data to rely on.
				
				But then that means if you do an ls -al in a big directory, you'll be GET'ing all those files, even
				though you don't want them.
				
				I think the 'real' solution is to do the etags-fetching business (below) up here somewhere instead, so we've
				already 'upgraded' the selectedverb to GET. Yeah, that sounds good?
			*/
			
			brintf("NEW CACHE - COMPLICATED QUESTION: verb: %s, status: %d\n",selectedverb,fetchstatus(headerbuf));
			
			/* problem - this is *not* using cached data for HEAD's that don't return files - 
			e.g. - I stat() a file without grabbing it. That causes a HEAD.
			I don't ever fetch the file. Later, I stat() it again. That result should be cached
			But it isn't (because there's no file-file behind it).
			*/
			
			int mystatus=fetchstatus(headerbuf);
			if(!S_ISLNK(cachestat.st_mode) && ( // NEVER open a symlink... AND....
				strcmp(selectedverb,"GET")==0 || //if you asked for a GET, or
				(mystatus>=200 && mystatus<=299) ||  //you got a 200 series code...
				mystatus==304 || //or a 304 etag-match message...  
				has_file_been_PUT))   //or file was recently PUT
			{
				//THEN you need a real live file pointer returned to you
				tmp=fopen(cachefilebase,cachefilemode);
				//if it was a HEAD request, it's okay if you couldn't get the file
				if(tmp || strcmp(selectedverb,"HEAD")==0) { 
					if(headers && headerlength>0) {
						strncpy(headers,headerbuf,headerlength);
					}
					dont_fclose(headerfile); //RELEASE LOCK!!!!!
					if(dirmode) {
						brintf("FRESHENING OFF OF CACHE HIT! path: %s, headers: %s, ptr: %p\n",path,headerbuf,tmp);
						directory_freshen(path,headerbuf,tmp);
					}
					free(headerbuf);
					return tmp;
				} else {
					brintf("You tried to open the cachefile %s in mode %s and FAILED, so fallthrough.\n",
						cachefilebase,cachefilemode);
				}
			} else {
				//you don't need no stinkin' files!
				brintf("No files are needed, allegedly. returning probably a nil for file pointer\n");
				if(headers && headerlength >0) {
					strncpy(headers,headerbuf,headerlength);
				}
				dont_fclose(headerfile); //releasing that same lock...
				free(headerbuf);
				return 0;
			}
			brintf("I guess I couldn't open my cachefile...going through default behavior (bleh)\n");
			dontuseetags=1;
			
		} else {
			brintf("Cache file is too old (or too small); continuing with normal behavior. cachefilename: %s Age: %ld maxcacheage: %d, filesize: %d\n",
				headerfilename,(int)time(0)-statbuf.st_mtime,maxcacheage,(int)statbuf.st_size);
		}
	} else {
		//no cachefile exists, we ahve to create one while watching out for races.
		/**** EXCLUSIVE WRITE LOCK! *****/
		//RACE Condition - new file that's never been seen before, when we get *here*, we will have no header cache nor data cache file upon
		//which one might flock() ... but creating said file - when it might be a directory - could be bad. What to do, what to do?
		//fock()
		brintf("Instantiating non-existent metadata cachefile for something.\n");
		if(!(headerfile=fopenr(headerfilename,"w+x"))) {
			brintf("RACE CAUGHT ON FILE CREATION for %s! NOt sure what to do about it though... reason: %s. How bout we just fail it?\n",headerfilename,strerror(errno));
			free(headerbuf);
			return 0;
			//headerfile=fopenr(headerfilename,"w"); //this means a race was found and prevented(?)
		}
		safe_flock(fileno(headerfile),LOCK_SH,headerfilename); //shared lock will immediately be upgraded...
	}
	
	//either there is no cachefile for the headers, or it's too old.
	//the weird thing is, we have to be prepared to return our (possibly-outdated) cache, if we can't get on the internet
	
	//Here's the new impossible file detection - wait till you had a chance to return cached data first
	//then check if the file is impossible at the LAST MINUTE - *RIGHT* before you'd hit the Internets	
	if(impossible_file(path)) {
		if(headers && headerlength>0) {
			headers[0]='\0';
		}
		free(headerbuf);
		dont_fclose(headerfile);
		return 0;
	}
	char acceptheader[80]="";
	if(dirmode) {
		strcpy(acceptheader,"Accept: x-vnd.bespin.corp/directory-manifest, */*; q=0.5");
	}
	mysocket=http_request(webresource,selectedverb,etag,purpose,acceptheader,0); //extraheaders? (first param)
	
	//brintf("Http request returned socket: %d\n",mysocket);
	
	if(http_valid(mysocket)) {
		char *received_headers=0;
		if(recv_headers(&mysocket,&received_headers)) {
			char crestheader[1024]="";
			FILE *datafile=0;

			brintf("Here's the headers, btw: %s\n",received_headers);

			//special case for speed - on a 304, the headers probably haven't changed
			//so don't rewrite them (fast fast!)
			// We also want to skip the anti-starbucksing protocol (underneath)
			// because it's too hard to override apache's conservative view of which headers are 'allowed'
			// in a 304 response. expletive expletive.
			if(fetchstatus(received_headers)==304) {
				brintf("FAST 304 Etags METHOD! Not touching much (just utimes)\n");
				utime(headerfilename,0);
				if(headers && headerlength>0 ) {
					//So - why do we do this? Why not use the on-file headers instead?
					strlcpy(headers,headerbuf,headerlength);
				}
				if(fetchstatus(headerbuf)==200) {
					brintf("Original file was a file, gonna fopen the datafile\n");
					//this can be problematic if we were a symlink
					
					datafile=fopen(cachefilebase,cachefilemode); //use EXISTING basefile...
					brintf("FOPENED!\n");
				} else {
					datafile=0; //this could've been a symlink, don't try to open it
				}
				if(dirmode) {
					brintf("WE ARE IN DIRMODE - GOING TO TRY TO FRESHEN!\n");
					brintf("(HEADERBUF SEZ: %s)\n",headerbuf);
					directory_freshen(path,headerbuf,datafile);
				}
				brintf("Bout to dont_fclose\n");
				dont_fclose(headerfile); // RELEASE LOCK
				free(received_headers);
				free(headerbuf);
				http_close(&mysocket);
				brintf("Ready to return..\n");
				return datafile;
			}

			//brintf("And I think the headers ARE: %s",mybuffer);
			fetchheader(received_headers,"x-bespin-crest",crestheader,1024);
			if(strlen(crestheader)==0) {
				brintf("COULD NOT Find Crest-header - you have been STARBUCKSED. Going to cache!\n");
				brintf("Busted headers are: %s\n",received_headers);
			} else {
				int truncatestatus=0;
				char location[1024]="";
				char dn[1024];
				char tempfile[1024];
				int tmpfd=-1;
				/* problems with the anti-starbucksing protocol - need to handle redirects and 404's
					without at least the 404 handling, you can't negatively-cache nonexistent files.
				*/
				//brintf("We should be fputsing it to: %p\n",headerfile);
				//need to upgrade read-lock to WRITE lock
				safe_flock(fileno(headerfile),LOCK_EX,headerfilename); //lock upgrade!
				truncatestatus=ftruncate(fileno(headerfile),0); // we are OVERWRITING THE HEADERS - we got new headers, they're good, we wanna use 'em
				rewind(headerfile); //I think I have to do this or it will re-fill with 0's?!
				//brintf(" Craziness - the number of bytes we should be fputsing is: %d\n",strlen(mybuffer));
				//brintf("truncating did : %d",truncatestatus);
				if(truncatestatus) {
					brintf("truncating did : %d, cuzz: %s\n",truncatestatus,strerror(errno));
				}
				brintf("\n");			
				fputs(received_headers,headerfile);
				if(headers && headerlength>0) {
					//is this common enough to hoist up a little?
					//no, cases which have to fall back to the stale caches will rewrite the headers (no?)
					//wait, yes - if they do do that, they will overwrite the headers on their own!
					strncpy(headers,received_headers,headerlength);
					//if these headers don't get used, they will be rewritten when the stale cache gets loaded up
				}

				//copy the newly-found headers to the headerfile and keep fetching into the file buffer
				//unlock?!!? ***** UNlocK ******
				//rename() file into place, rewind file pointer thingee,
				//freopen file pointer thingee to be read-only, and then
				//return the file buffer
				//write the remnants of the recv'ed header...
				//brintf("HTTP Header is: %d\n",fetchstatus(received_headers));
				//brintf("Content length is: %s\n",contentlength);

				switch(fetchstatus(received_headers)) {
					case 200:
					//ONE THING WE NEED TO NOTE - if this was a HEAD instead of a GET,
					//we need to be sensitive about the cachefile?
					if(strcmp(selectedverb,"HEAD")==0) {
						dont_fclose(headerfile); //RELEASE LOCK!
						free(received_headers);
						brintf("DUDE TOTALLY SUCKY!!!! Somebody HEAD'ed a resource and we had to unlink its data file.\n");
						unlink(cachefilebase);
						http_close(&mysocket);
						free(headerbuf);
						return 0;// NO CONTENTS TO DEAL WITH HERE!
					}
					strncpy(dn,cachefilebase,1024);
					dirname(dn);
					strncpy(tempfile,dn,1024);
					strncat(tempfile,"/.tmpfile.XXXXXX",1024);
					redirmake(tempfile); //make sure intervening directories exist, since we can't use fopenr()
					tmpfd=mkstemp(tempfile); //dumbass, this creates the file.
					//brintf("tempfile will be: %s, it's in dirname: %s\n",tempfile,dn);
					datafile=fdopen(tmpfd,"w+");
					if(!datafile) {
						brintf("Cannot open datafile for some reason?: %s",strerror(errno));
					}
					
					contents_handler(mysocket,datafile);
					if(rename(tempfile,cachefilebase)) {
						brintf("Could not rename file to %s because: %s\n",cachefilebase,strerror(errno));
					}
					chmod(cachefilebase,0777);
					datafile=freopen(cachefilebase,"r",datafile);
					if(!datafile) {
						brintf("Failed to reopen datafile?!: %s\n",strerror(errno));
					}
					rewind(datafile);
					if(dirmode) {
						brintf("DIRMODE - FRSHEN %s!\n",path);
						directory_freshen(path,received_headers,datafile);
					}
					dont_fclose(headerfile); //RELEASE LOCK
					free(received_headers);
					free(headerbuf);
					http_close(&mysocket);
					return datafile;
					break;

					case 301:
					case 302:
					case 303:
					case 307:
					//check for directory
					//IF SO: make a cache dir to represent this directory, AND RETRY REQUEST!
					//IF NOT! Treat as symlink(?!). Write headers to headerfile. return no data (or empty file?)
					//NB. Requires a change to readlink()
					//there is NO datafile to work with here, but we don't return 0...how's that gonna work?
					wastebody(mysocket);

					fetchheader(received_headers,"location",location,1024);
					
					if( (strncmp(location,"/",1)==0 && //Host-relative url (starting with '/')
						strncmp(location,pathpart,strlen(pathpart))==0 && 
						strlen(location)==strlen(pathpart)+1 &&
						location[strlen(location)-1]=='/'
					      ) || 
					      (	strncmp(location,"http://",7)==0 &&  //absolute URL
						strncmp(location+7,path,strlen(path))==0 && 
						strlen(location+7)==strlen(path)+1 && 
						location[strlen(location)-1]=='/')) 
					{
						brintf("Location discovered to be: %s, assuming DIRECTORY and rerunning!\n",location);
						//assume this must be a 'directory', but we requested it as if it were a 'file' - rerun!
						redirmake(cachefilebase); //make enough of a path to hold what will go here
						unlink(cachefilebase); //if it's a file, delete the file; it's not a file anymore
						mkdir(cachefilebase,0700); 
						unlink(headerfilename); //the headerfile being a file will mess things up too
									//and besides, it's just going to say '301', which isn't helpful
						dont_fclose(headerfile);
						free(received_headers);
						char modpurpose[1024];
						strlcpy(modpurpose,purpose,1024);
						strlcat(modpurpose,"+directoryrefetch",1024);
						free(headerbuf);
						http_close(&mysocket);
						//TEMP WORKAROUND FIX FOR POSSIBLE BUG ISOLATION STUFF?!
						//delete_keep(mysocket);
						//close(mysocket);
						return get_resource(path,headers,headerlength,isdirectory,preferredverb,modpurpose,cachefilemode);
					} else {
						brintf("We are thinking this is a symlink. location: %s, pathpart: %s, strlen(loc): %d, strlen(pathpart): %d",location,pathpart,strlen(location),strlen(pathpart));
					}
					//otherwise (no slash at end of location path), we must be a plain, boring symlink or some such.
					//yawn.
					brintf("Not a directory, treating as symlink...\n");
					dont_fclose(headerfile); //release lock
					free(received_headers);
					free(headerbuf);
					http_close(&mysocket);
					return 0; //do we return a filepointer to an empty file or 0? NOT SURE!
					break;

					case 404:
					//Only weird case we got is *IF* we 'accelerated' this into a directory request, and we 404'ed,
					//it may be a regular file now. DELETE THE DIRECTORY, possibly the directoryfilenode thing,
					//and RETRY REQUEST!!!

					//be prepared to drop a 404 cachefile! This will prevent repeated requests for nonexistent entities
					if(dirmode) {
						wastebody(mysocket);
						//we 404'ed (or 403'ed) in dirmode. Shit.
						char headerdirname[1024];
						brintf("Directory mode resulted in 404. Retrying as regular file...\n");
						unlink(cachefilebase);
						unlink(headerfilename);
						rmdir(path+1);
						strncpy(headerdirname,slashlessmetaprepend,1024);
						strncat(headerdirname,path,1024);
						rmdir(headerdirname);
						brintf("I should be yanking directories %s and %s\n",path+1,headerdirname);
						free(received_headers);
						char modpurpose[1024];
						strlcpy(modpurpose,purpose,1024);
						strlcat(modpurpose,"+plainrefetch",1024);
						http_close(&mysocket);
						free(headerbuf);
						if (headerfile) dont_fclose(headerfile); //need this?! release lock?
						return get_resource(path,headers,headerlength,isdirectory,preferredverb,modpurpose,cachefilemode);
					}
					//NOTE! We are *NOT* 'break'ing after this case! We are deliberately falling into the following case
					//which basically returns a 0 after clearning out everything that's bust.

					case 403: //forbidden
					case 401: //requires authentication
					wastebody(mysocket);
					brintf("404/403/401 mode, I *may* be closing the cache header file...\n");
					if(headerfile) {
						brintf(" Results: %d\n",dont_fclose(headerfile)); //Need to release locks on 404's too! //BAD
					} else {
						brintf(" No headerfile to close\n");
					}
					free(received_headers);
					http_close(&mysocket);
					free(headerbuf);
					return 0; //nonexistent file, return 0, hold on to cache, no datafile exists.
					break;

					default:
					brintf("Unknown HTTP status (%d): %s\n",fetchstatus(received_headers),received_headers);
					http_close(&mysocket); //SOMETHING HARSHER HERE PERHAPS?!
					//return_keep(mysocket);
					free(received_headers);
					free(headerbuf);
					if(headerfile) {
						dont_fclose(headerfile); //release lock!if you have it...
					}
					return 0;
					break;
				} //end switch on http status
			} //end 'else' clause about whether we have CREST headers or not
		} //end if clause as to whether we got a valid response on recv_headers
		brintf("Past block for receiving data, strerror says: %s\n",strerror(errno));
		free(received_headers);
	} 
	//if things went remotely well and internet-connectedly, you shouldn't end up here.
	//if you did, something screwed up. Try to at least return a cachefile or something.
	//try to return the cached stuff?
	//it could be stale.
	
	// OPEN QUESTION - how does this work for NONEXISTENT files when the internet isn't there?! How can I tell the difference?!
	FILE *staledata=0;
	brintf("BAD INTERNET CONNECTION, returning possibly-stale cache entries for webresource: %s\n",webresource);
	brintf("Headers - which I woudl think wouldn't exist for nonexistent files - are: %s len(%d)\n",headerbuf,strlen(headerbuf));
	if(headers && headerlength>0) {
		brintf("Headerbuf has been filled, copying it to result headers\n");
		brintf("Current headers are: %s",headerbuf);
		//brintf("Did that crash us or something?\n");
		strlcpy(headers,headerbuf,headerlength);
	}
	free(headerbuf);
	//brintf("Header buffer freed\n");
	brintf("The cache file base we'd _like_ to have open will be: %s\n",cachefilebase);
	//careful, doing stat() here and not lstat - if it's a symlink, just follow it.
	if(lstat(cachefilebase,&cachestat)==0 && (S_ISREG(cachestat.st_mode) || S_ISLNK(cachestat.st_mode))) {
		staledata=fopen(cachefilebase,cachefilemode); //could be 0
		if(!staledata) {
			brintf("Crap, coudln't open datafile even though we could stat it. Why?!?! %s\n",strerror(errno));
		} else {
			brintf("Data file opened (%p)\n",staledata);
		}
	} else {
		brintf("Either couldn't stat cache or cache ain't a plain file: %s\n",strerror(errno));
	}
	dont_fclose(headerfile); //RELEASE LOCK!
	brintf("dont_fclose'd headerfile\n");
	brintf("About to return data file: %p\n",staledata);
	http_close(&mysocket);
	return staledata;
}
