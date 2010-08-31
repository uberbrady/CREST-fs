/* For more information, see http://www.macdevcenter.com/pub/a/mac/2007/03/06/macfuse-new-frontiers-in-file-systems.html. */ 

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <netinet/in.h>

#include <dirent.h>

#define FUSE_USE_VERSION  26
#include <fuse.h>
#include <sys/time.h>
//#include <xlocale.h>

#include <strings.h>

#include "resource.h"
#include "common.h"
#include "http.h"

#include <pthread.h>

//global variables (another one is down there but it's use is jsut there.
char rootdir[1024]="";
char cachedirname[1024]="";
int maxcacheage=-1;


//we may want this to be configurable in future - a command line option perhaps
/*************************/

//retrieve the value of an HTTP header
//be warned the headers may not be a nul-terminated string
//so don't do stupid stuff like strlen(headers) because it 
//may not work
/******************* END UTILITY FUNCTIONS< BEGIN ACTUALLY DOING OF STUFF!!!!! *********************/

#define METALEN 1024

#define HEADERLEN 65535

static int
crest_getattr(const char *path, struct stat *stbuf)
{
	char header[HEADERLEN]="";

	if (strcmp(path, "/") == 0) { /* The root directory of our file system. */
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 1; //set this to '1' to force find(1) to be able to iterate
	} else {
		char hostname[80];
		char pathpart[1024];
		pathparse(path,hostname,pathpart,80,1024);
		brintf("getattr: path parses to hsotname: %s and path: '%s'\n",hostname,pathpart);
		
		if(strcmp(pathpart,"/")==0 || strcmp(path,"/")==0) {
			brintf("ROOT OF A HOST - *MUST* be a directory!\n");
			//root of a host, MUST be a directory no matter what.
			stbuf->st_mode = S_IFDIR | 0755; //I am a a directory!
			stbuf->st_nlink = 1; //use '1' to allow find(1) to decend within...
		} else {
			FILE *cachefile=0;
			int isdirectory=-1;
			/**** ALL WORK IS DONE WITH get_resource! ****/
			
			cachefile=get_resource(path,header,HEADERLEN,&isdirectory,"HEAD","getattr","r");
			
			/*** THAT DID A LOT! ****/
			
			if(isdirectory) {
				brintf("Getattr for directory mode detected.\n");
				stbuf->st_mode = S_IFDIR | 0755;
				stbuf->st_nlink = 1;
			} else {
				int httpstatus=fetchstatus(header);
				char date[80]="";
				char length[80]="";

				brintf("Status: %d on path %s, Header: %s\n",httpstatus,path,header);
				switch(httpstatus) {
					case 304:
					brintf("ERROR - we should never be 'exposed' to a 304 header\n");
					exit(33); //fundamental structure of space and time has been distorted. I don't want to live in this world.
					break;
					
					case 200:
					case 201:
					case 204:
					//brintf("It is a file\n");
					stbuf->st_mode = S_IFREG | 0700;
					//brintf("Pre-mangulation headers: %s\n",header);
					fetchheader(header,"last-modified",date,80);
					fetchheader(header,"content-length",length,80);
					if(strlen(length)>0) {
						//use content-length in case this file was only HEAD'ed, otherwise seek to its end
						stbuf->st_size=atoi(length);
					} else {
						if(!cachefile) {
							brintf("WEIRD! No cachefile given on a %d, and couldn't find content-length!!! Let's see if there's anything interesting in errno: %s\n",httpstatus,strerror(errno));
							stbuf->st_size=0;
						} else {
							int seeker=fseek(cachefile,0,SEEK_END);
							if(seeker) {
								brintf("Can't seek to end of file! WTF!\n");
								exit(88); //Errr....yeah, all bets are off if taht happens
							}
							stbuf->st_size= ftell(cachefile);
						}
					}
					//brintf("Post-mangulation headers is: %s\n",header+1);
					//brintf("Post-mangulation headers is: %s\n",header);
					//brintf("BTW, date I'm trying to format: %s\n",date);
					//brintf("WEIRD...date: %s, length: %d\n",date,(int)stbuf->st_size);
					stbuf->st_mtime = parsedate(date);
					brintf("parsedate: Uh, Date %s parsed to: %d\n",date,(int)stbuf->st_mtime);
					break;
					
					case 301:
					case 302:
					case 303:
					case 307:
					//http redirect, means either DIRECTORY or SYMLINK
					//start out by keeping it simple - if it ends in a
					//ZERO caching attempts, to start with...
					stbuf->st_mode = S_IFLNK;
					stbuf->st_nlink = 1;
					break;
					
					case 404:
					if(cachefile) {
						brintf("Why did I get a valid cachefile on a 404?!");
						fclose(cachefile);
					}
					return -ENOENT;
					break;
					
					case 401:
					if(cachefile) {
						fclose(cachefile);
					}
					return -EACCES;
					break;
					
					default:
					brintf("Weird http status, don't know what to do with it!!! It is: %d\n",httpstatus);
				}
				
			}
			
			if(cachefile)
				fclose(cachefile);
			else
				brintf("TRY TO CLOSE NO CACHEFILE!\n");
		}
	}

	return 0;
}

#include <libgen.h> //needed for dirname and friends, used to calculate symlink targets

static int
crest_readlink(const char *path, char * buf, size_t bufsize)
{
	//we'll start with the assumption this actuallly IS a symlink.
	char location[4096]="";
	char header[HEADERLEN]="";
	FILE *cachefile;
	int is_directory=-1;
	
	cachefile=get_resource(path,header,HEADERLEN,&is_directory,"HEAD","readlink","r");
	
	if(is_directory) {
		if(cachefile) {
			fclose(cachefile);
		}
		return -EINVAL;
	}
			
	fetchheader(header,"Location",location,4096);
	
	//from an http://www.domainname.com/path/stuff/whatever
	//we must get to a local path.
	if(strncmp(location,"http://",7)!=0) {
		//relative symlink. Either relative to _SERVER_ root, or relative to dirname...
		if(location[0]=='/') {
			brintf("No absolute directory-relative symlinks yet, sorry\n");
			return -EIO;
			strlcpy(buf,rootdir,bufsize); //fs cache 'root'
			
		} else if(strlen(location)>0) {
			strlcpy(buf,rootdir,bufsize); //fs cache 'root'
			char dn[1024];
			strlcpy(dn,path,1024);
			strlcat(buf,dirname(dn),bufsize); //may modify argument, hence the copy
			strlcat(buf,"/",bufsize);
			strlcat(buf,location,bufsize);
			
		} else {
			strlcpy(buf,rootdir,bufsize); //fs cache 'root'
			brintf("Unsupported protocol, or missing 'location' header: %s\n",location);
			return -ENOENT;
		}
	} else {
		strlcpy(buf,rootdir,bufsize); //fs cache 'root'
		strlcat(buf,"/",bufsize);
		strlcat(buf,location+7,bufsize);
	}
	
	if(cachefile) {
		brintf("Freaky, got a valid cachefile for a symlink?!\n");
		fclose(cachefile);
	}
	struct stat st;
	int stres=lstat(path+1,&st);
	if(stres==0 && S_ISLNK(st.st_mode)) {
		char linkbuf[1024]="";
		int linklen=readlink(path+1,linkbuf,1024);
		if(linklen>1023) {
			brintf("Too long of a link :(\n");
			exit(54); //buffer overflow
		}
		linkbuf[linklen+1]='\0';
		if(stres==0 && strncmp(linkbuf,buf,1024)==0) {
			//brintf("Perfect match on symlink! Not doing anything\n");
			return 0;
		} else {
			brintf("Nope, link is badly made, it is currently: %s\n",linkbuf);
		}
	}
	brintf("I will unlink: %s, and point it to: %s\n",path+1,buf);
	int unlinkstat=unlink(path+1); (void)unlinkstat;
	int linkstat=symlink(buf,path+1); (void)linkstat;
	brintf("Going to return link path as: %s (unlink status: %d, link status: %d)\n",buf,unlinkstat,linkstat);
	return 0;
}

static int
crest_open(const char *path, struct fuse_file_info *fi)
{
	FILE *rsrc=0;
	char headers[4096];
	
	int is_directory=-1;
	int wantwrite=(fi->flags & O_ACCMODE) != O_RDONLY; //not read-only means write only or read-write.
	if(!wants_auth(path)) {
		//not under the root auth URL, so no writing allowed
		if (wantwrite) { /* Only reading allowed. */
			return -EACCES;
		}
	}
	rsrc=get_resource(path,headers,4096,&is_directory,"GET","open",(wantwrite? "r+": "r"));
	if(is_directory) {
		if(rsrc) {
			fclose(rsrc);
		}
		return -EISDIR;
	}
	if(fetchstatus(headers)==404) {
		if(rsrc) {
			brintf("Wow - weird, got a 404 but a valid resource? I guess\n");
			fclose(rsrc);
		}
		return -ENOENT;
	}
	if(rsrc) {
		brintf("Going to set filehandle to POINTER: %p\n",rsrc);
		fi->fh=(long int)rsrc;
		return 0;
	} else {
		brintf("Uh, no resource retrieved? Setting fh to zero?\n");
		fi->fh=0;
		return 0;
	}
}

static int
crest_release(const char *path __attribute__ ((unused)), struct fuse_file_info *fi)
{
	brintf("Closing filehandle %p: for file: %s\n",(FILE *)(unsigned int)fi->fh,path);
	if(fi->fh) {
		return fclose((FILE *)(unsigned int)fi->fh);
	} else {
		return 0;
	}
}

static int
crest_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
              off_t offset, struct fuse_file_info *fi)
{
	FILE *dirfile=0;
	int is_directory=-1;
	if(fi) {
		//brintf("Well, dunno why this is, but we have file info: %p, filehandle: %d\n",fi,(int)fi->fh);
	}

	//should do a directory listing in the domains directory (the first one)
	// but let's do that later.
	
	if(strcmp(path,"/")==0) {
		brintf("PATH IS /!!!!!\n");
		DIR *mydir;
		struct dirent *dp;
		int id=1;
		
		mydir=opendir(".");
		while((dp=readdir(mydir))!=0) {
			brintf("Potential dir is: %s, id: %d (vs offset: %d)\n",dp->d_name,id,(int)offset);
			if(id>offset) {
				if(filler(buf,dp->d_name,NULL,id)!=0) {
					break;
				}
			}
			id++;
		}
		brintf("Done with dir listing, returning...");
		closedir(mydir);
		return 0;
	}

	char headers[8192];
	dirfile=get_resource(path,headers,8192,&is_directory,"GET","readdir","r");
	brintf("Just fetched the resource for this directory you're reading, just FYI.");
	if(!is_directory) {
		if(dirfile) {
			fclose(dirfile);
		}
		return -ENOTDIR;
	}
	if(dirfile==0) {
		//got no directory listing file, this directory doesn't even exist.
		return -ENOENT;
	}
	
	directory_iterator it;
	char filename[1024];
	init_directory_iterator(&it,headers,dirfile);
	int failcounter=0;
	if(offset<++failcounter) filler(buf, ".", NULL, failcounter); /* Current directory (.)  */
	if(offset<++failcounter) filler(buf, "..", NULL, failcounter);          /* Parent directory (..)  */
	while(directory_iterate(&it,filename,1024,0,0)) {
		failcounter++;
		brintf("Returned filename IS: %s - ",filename);
		if(offset<failcounter) {
			if(filler(buf, filename, NULL, failcounter)!=0) {
				brintf("Filler said not zero.\n");
				break;
			} else {
				brintf("OK\n");
			}
		} else {
			brintf("Less than offset %d, so ignoring\n",(int)offset);
		}
	}
	free_directory_iterator(&it);
	fclose(dirfile);
	return 0;
}


#if 0
			
			while(status==0 && failcounter < TOOMANYFILES) {
				failcounter++;
				char hrefname[255];
				char linkname[255];
				
				weboffset+=rm[0].rm_eo;
				//brintf("Weboffset: %d\n",weboffset);
				status=regexec(&re,dirbuffer+weboffset,3,rm,0); // ???
				if(status==0) {
					reanswer(dirbuffer+weboffset,&rm[1],hrefname,255);
					reanswer(dirbuffer+weboffset,&rm[2],linkname,255);

					//brintf("Href? %s\n",hrefname);
					//brintf("Link %s\n",linkname);
					//brintf("href: %s link: %s\n",hrefname,linkname);
					if(strcmp(hrefname,linkname)==0) {
						//brintf("ELEMENT: %s\n",hrefname);
						if(offset<failcounter) 
							if(filler(buf, hrefname, NULL, failcounter)!=0) {
								brintf("Filler said not zero.\n");
								break;
							}
					}
				}
				//brintf("staus; %d, 0: %d, 1:%d, 2: %d, href=%s, link=%s\n",status,rm[0].rm_so,rm[1].rm_so,rm[2].rm_so,hrefname,linkname);
				//filler?
				//filler(buf,rm[])
			}
			//brintf("while loop - complete\n");
			if(failcounter>=TOOMANYFILES) {
				brintf("Fail due to toomany\n");
			}
			free(dirbuffer);
			regfree(&re);
			return 0; //success? or does that mean fail?
		}
	}

#endif 

#define FILEMAX 64*1024*1024

static int
crest_read(const char *path __attribute__ ((unused)), char *buf, size_t size, off_t offset,
           struct fuse_file_info *fi)
{
	FILE *cachefile;
	
	if(fi) {
		//brintf("Good filehandle for path: %s, pointer:%p\n",path,(FILE *)(unsigned int)fi->fh);
	} else {
		brintf("BAD FILE*INFO* IS ZERO for path: %s\n",path);
		brintf("File was not properly opened?\n");
		return -EIO;
	}
	cachefile=(FILE *)(unsigned int)fi->fh;
	
	if(cachefile) {
		if(fseek(cachefile,offset,SEEK_SET)==0) {
			return fread(buf,1,size,cachefile);
		} else {
			//fail to seek - what'ssat mean?
			return 0;
		}
	} else {
		return -EIO;
	}
}

#include <sys/file.h>

static int
crest_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	brintf("I'm going tow rite noe\n");
	if(!fi) {
		brintf("File Info is ZERO for path: %s\n",path);
		return -EIO;
	}
	FILE *cachefile=(FILE *)(unsigned int)fi->fh;
	//need to prolly flock() the *metadata* file or something here?
	char metafilename[1024];
	strlcpy(metafilename,METAPREPEND,1024);
	strlcat(metafilename,"/",1024);
	strlcat(metafilename,path,1024);
	FILE *metafile=fopenr(metafilename+1,"w+");
	if(!metafile) {
		brintf("Couldn't open metafile on behalf of data-write: %s\n",metafilename);
		return -EIO;
	}
	brintf("Uhm, our metafile (for writing) is: %s\n",metafilename+1);
	safe_flock(fileno(metafile),LOCK_EX,metafilename+1); // begin 'transactioney-thing'. EXCLUSIVE LOCK.
	if(cachefile) {
		if(fseek(cachefile,offset,SEEK_SET)==0) {
			int results=fwrite(buf,1,size,cachefile);
			markdirty(path);
			//faux_freshen_metadata(path);
			fclose(metafile); //end transactoiney thing
			//brintf("Truncate: %d, write: %d\n",truncres,writeres);
			return results;
		} else {
			brintf("Failed to seek in file %s\n",path);
			fclose(metafile);
			return -EIO;
		}
	} else {
		brintf("No cachefile ready for this file: %s.\n",path);
		fclose(metafile);
		return -EIO;
	}
}


int	cachedir = 0;
pthread_t putter;

static void *
crest_init(struct fuse_conn_info *conn __attribute__((unused)) )
{
	brintf("I'm not intending to do anything with this connection: %p\n",conn);
	fchdir(cachedir);
	close(cachedir);
	
	if(strcmp(authfile,"/dev/null")!=0) {
		printf("Creating write thread...\n");
		pthread_create(&putter, NULL, putting_routine,0);
		printf("Write Thread Created\n");
	} else {
		printf("WRITE SUPPORT DISABLED VIA /dev/null AUTHFILE\n");
	}	
	return 0;
}

static int
crest_chmod(const char *p __attribute__((unused)), mode_t mode __attribute__((unused)))
{
	return 0;
}

static int
crest_chown(const char *p __attribute__((unused)), uid_t u __attribute__((unused)), gid_t g __attribute__((unused)))
{
	return 0;
}

static int
crest_utime	(const char *p __attribute__((unused)), struct utimbuf *ut __attribute__((unused)))
{
	return 0;
}

static int
crest_trunc (const char *path, off_t length)
{
	return truncate(path+1,length);
}

/* static int 
crest_create(const char *path, mode_t m __attribute__((unused)), struct fuse_file_info *fi)
{
*//*	if(!wants_auth(path)) {
		return -EACCES;
	} */ /*
	//I think crest_open does all the possible access-checking we'd need, anyways, so just call it
	return crest_open(path,fi); //I think
} */


static int
crest_mknod(const char*path,mode_t m, dev_t d __attribute__((unused)))
{
	brintf("Beginning mknod for %s\n",path);
	//and create a file
	if(!S_ISREG(m)) {
		brintf("File you're requesting to make isn't regular, failo\n");
		return -EACCES;
	}
	brintf("mknod: it's a regular file...\n");
	char metafilename[1024];
	strncpy(metafilename,METAPREPEND,1024);
	strncat(metafilename,path,1024);
	FILE * meta=fopenr(metafilename+1,"w+"); //CREATE IF NO EXIST!
	if(!meta) {
		brintf("COULD NOT CREATE METAFILE! %s",metafilename+1);
		return -EIO;
	}
	brintf("mknod: meta file created.\n");
	safe_flock(fileno(meta),LOCK_EX,metafilename+1); //you're just 'touch()'ing a file, no need to go that nuts on it dude.
	FILE * data=fopenr((char *)path+1,"w+"); //CREATE if no exist.
	if(!data) {
		brintf("COULD not CREATE REAL FILE! %s",path+1);
		fclose(meta);
		return -EIO;
	}
	brintf("OK, fine - files created *AND* truncated!\n");
	
	//should we be setting a DATE on this file? This would make go away some of our 1/1/1970 problems.
	//but it may bring us other problems...well, let's jsut try it, we've got the code hanging out.
	
	markdirty(path); //path marked dirty so it will get 'put'
	freshen_metadata(path,200,0); //shiny metadata taill the PUT rewrites it
	append_parents(path); //make it look like I've been appended to my parents directory listings!
	fclose(data);
	fclose(meta); //causes unlock
	return 0;
}

//there are all kinds of cross-polination of concerns leaking all over the place here.
//first, we're directly referencing FILE *'s
//next we're directly calling things with PENDINGDIR

static int
crest_unlink(const char *path)
{
	//unlink path+1
	char metapath[1024];
	
	strlcpy(metapath,METAPREPEND,1024);
	strlcat(metapath,path,1024);
	FILE *metafile=fopenr(metapath+1,"w+");
	if(!metafile) {
		brintf("Could not open metafile for %s for deletion, bailing?\n",path);
		return -EIO;
	}
	safe_flock(fileno(metafile),LOCK_EX,metapath+1);

	char hash[23];
	hashname(path,hash);
	char putpath[1024];
	strlcpy(putpath,PENDINGDIR,1024);
	strlcat(putpath,"/",1024);
	strlcat(putpath,hash,1024);
	brintf("The path I would unlink would be: %s\n",putpath);
	int unl=unlink(putpath); //if this fails, that's fine.
	brintf("Unlink of possible put file? %d\n",unl);

	httpsocket pointless=http_request(path,"DELETE",0,"unlink",0,0);
	if(!http_valid(pointless)) {
		fclose(metafile);
		return -EAGAIN;
	}
	char *resultheaders=0;
	recv_headers(&pointless,&resultheaders);
	wastebody(pointless);
	
	http_close(&pointless);

	if((resultheaders && fetchstatus(resultheaders) >=200 && fetchstatus(resultheaders) <300)|| unl==0) {
		invalidate_parents(path);
		free(resultheaders);
		fclose(metafile);
		return 0;
	} else {
		brintf("FALIURE Unlinking: %s\n",resultheaders);
		printf("deletion HTTP request failed for %s: %s",path,resultheaders);
		fclose(metafile);
		printf("You neither deleted a putable file, nor something off the server.\n");
		return -EIO;
	}
}

static int
crest_rmdir(const char *path)
{
	//unlink path+1
	char metapath[1024];
	char slashedpath[1024];
	
	strlcpy(slashedpath,path,1024);
	strlcat(slashedpath,"/",1024);
	
	strlcpy(metapath,METAPREPEND,1024);
	strlcat(metapath,path,1024);
	strlcat(metapath,DIRCACHEFILE,1024);
	FILE *metafile=fopenr(metapath+1,"w+");
	if(!metafile) {
		brintf("Could not open metafile for %s for deletion, bailing?\n",path);
		return -EIO;
	}
	safe_flock(fileno(metafile),LOCK_EX,metapath+1);

	char hash[23];
	hashname(path,hash);
	char putpath[1024];
	strlcpy(putpath,PENDINGDIR,1024);
	strlcat(putpath,"/",1024);
	strlcat(putpath,hash,1024);
	brintf("The path I would unlink would be: %s\n",putpath);
	int unl=unlink(putpath); //if this fails, that's fine.
	brintf("Unlink of possible put file? %d\n",unl);

	httpsocket pointless=http_request(slashedpath,"DELETE",0,"unlink",0,0);
	if(!http_valid(pointless)) {
		fclose(metafile);
		return -EAGAIN;
	}
	char *resultheaders=0;
	recv_headers(&pointless,&resultheaders);
	wastebody(pointless);
	
	http_close(&pointless);

	if((resultheaders && fetchstatus(resultheaders) >=200 && fetchstatus(resultheaders) <300)|| unl==0) {
		//we should toss our DATA-DATA directory (which is going to be getting into other's way)
		//and our METADATA directory (which will do the same)
		invalidate_parents(path);
		free(resultheaders);
		fclose(metafile);
		return 0;
	} else {
		brintf("FALIURE Unlinking: %s\n",resultheaders);
		printf("deletion HTTP request failed for %s: %s",path,resultheaders);
		fclose(metafile);
		printf("You neither deleted a putable file, nor something off the server.\n");
		return -EIO;
	}
}

static int
crest_mkdir(const char* path,mode_t mode __attribute__((unused)))
{
	//THIS SHOULD BE DONE ASYNC INSTEAD! THIS IS WRONG! WILL NOT WORK DISCONNECTEDLY!!!!!!!
	char dirpath[1024];
	//char *resultheaders=0;
	strlcpy(dirpath,path,1024);
	strlcat(dirpath,"/",1024);
	/* httpsocket dontcare=http_request(dirpath,"PUT",0,"mkdir",0,0);
	if(!http_valid(dontcare)) {
		return -EAGAIN;
	}
	recv_headers(&dontcare,&resultheaders);
	wastebody(dontcare); //we may have received a body, get rid.
	//close(dontcare); //no, that's poor manners.
	http_close(&dontcare); //that's nicer. 
	if(resultheaders && fetchstatus(resultheaders) >=200 && fetchstatus(resultheaders) < 300) {
		free(resultheaders); */
		append_parents(path);
		//should unlink my own metadeata, no?
		char mymeta[1024];
		strlcpy(mymeta,METAPREPEND,1024);
		strlcat(mymeta,path,1024);
		int res=unlink(mymeta+1); //unlink my plain-jane metadata file, if it existed (whew!)
		(void)res;
		brintf("I tried to unlink my personal metadata file: %s and got %d\n",mymeta+1,res);
		res=mkdir(mymeta+1,0700);
		brintf("Just tried to make my metadata directory %s and got %d\n",mymeta+1,res);
		res=mkdir(path+1,0700); //boom. Just cut number of HTTP requests in HALF. HALF baby.
		brintf("And making my directory-directory resulted in: %d, here's the dir we tried to make: %s, and here's errno: %s",
			res,path+1,strerror(errno));
		//we are re-using dirpath, above.
		//it looks like this:      /domainname/dirname/dirname/
		strlcat(dirpath,DIRCACHEFILE,1024); //so it'll have two slashes. So what. Shut up.
		freshen_metadata(dirpath,200,0); //make our metadata look nice 'n' fresh!
		markdirty(path); //make the symlink!
		return 0;
/* 	} else {
		free(resultheaders);
		return -EACCES;
	} */
}

static int
crest_symlink(const char *link, const char *path)
{
	/*
	    symlink(symlinktarget,symlinkfile)
		=->
	    NB - 
		relative link - symlinktarget="poo"
		absolute link - symlinktarget="/crestmountpoint/domainname/directory/file"
		absolute out-of-tree -        "/tmp/something/something"
		relative, walkey             ="../../../whoknows/what/it/is"
		
		either file:// or http:// - out of tree is file:// ?
	*/
//this whole nifty 'resolve target' thing has to be scrapped for now
/*	char resolvedpath[PATH_MAX]="";
	realpath(link,resolvedpath);
	brintf("symlinking %s to %s, resolves to: %s\n",path,link,resolvedpath);
	if(strncmp(resolvedpath,cachedirname,strlen(cachedirname))!=0) { //for now only permit symlinks back into CREST-space...
		brintf("Not starting with cachedir: %s\n",cachedirname);
		return -EACCES;
	} 
	char newtarget[1024]="";
	strlcpy(newtarget,"http://",1024);
	strlcat(newtarget,resolvedpath+strlen(cachedirname)+2,1024);
*/	//what a mess

	brintf("SYMLINKIN: %s -> %s\n",path+1,link);
	char locheader[1024]="Location: ";
	//should unlink my own metadeata, no? COPY-PASE FROM DIRECTORY THING ABOVE
	if(link[0]=='/') {
		//absolute symlink, how do we handle this?
		
		//there are a few options:
		
		// #1) Symlink points to somewhere within same *host* e.g.
		//	/http/samplehost.com/testfile -> /http/samplehost.com/linktarget
		//	Translate to:
		//	Location: /linktarget
		//	?
		
		// #2) Symlink points to somewhere on DIFFERENT host e.g.
		//	/http/samplehost.com/testfile -> /http/differenthost.com/linktarget
		//	Translate to:
		//	Location: http://differenthost.com/linktarget
		
		// #3) Symlink points outside of crest-fs - e.g.
		//	/http/samplehost.com/testfile -> /etc/passwd
		//	Translate to:
		//	Location: file:///etc/passwd
		
		// Option 3 we will just not permit to start with.
		
		// Answer: Wrap up everything in option #2 (which slighly subsumes #1)
		
		// For posterity's sake, here's what a relative symlink looks like:
		// #4) Relative symlink (necessarily points within CREST-fs)
		//	/http/samplehost.com/testfile -> linktarget
		//	Translates to:
		//	Location: linktarget
		
		if(strncmp(link,rootdir,strlen(rootdir))!=0 || link[strlen(rootdir)]!='/') {
			brintf("Can't symlink outside of CREST-fs\n");
			return -EACCES;
		}
		//option 3 is now rejected.
		
		strlcat(locheader,"http://",1024);
		strlcat(locheader,link+strlen(rootdir)+1,1024);
		brintf("Calulated Location Header is: %s\n",locheader);
	} else {
		//relative link? Super easy.
		strlcat(locheader,link,1024);
	}
	int res=symlink(link,path+1); //let the caching handle this till we start doing stuff async
	if(res==0) {
		append_parents(path);
		markdirty(path);
		freshen_metadata(path,302,locheader); //be prepared to pass Location header over...otherwise these symlinks won't resolve
		//int res=unlink(mymeta+1); //unlink my plain-jane metadata file, if it existed (whew!)
		//(void)res;
		//brintf("(SYMLINK) I tried to unlink my personal metadata file: %s and got %d\n",mymeta+1,res);
	} else {
		brintf("Failed to make symlink: %s\n",strerror(errno));
		return -EIO;
	}
	return 0;
}

static struct fuse_operations crest_filesystem_operations = {
    .init    = crest_init,    /* mostly to keep access to cache     */
    .getattr = crest_getattr, /* To provide size, permissions, etc. */
    .readlink= crest_readlink,/* to handle funny server redirects */
    .open    = crest_open,    /* To enforce read-only access.       */
    .release = crest_release, /* to hold on to a filehandle to the cache so that reads are consistent with one generation of the file */
    .read    = crest_read,    /* To provide file content.           */
    .readdir = crest_readdir, /* To provide directory listing.      */
    .write   = crest_write,   /* to write data, of course           */
    .truncate= crest_trunc, 
    //.create = crest_create,
    .mknod   = crest_mknod,   /* mostly just for creating files     */

    .mkdir   = crest_mkdir,   /* what do you think it's for, smart guy? */
    .rmdir   = crest_rmdir,

    .symlink = crest_symlink,

    .chmod = crest_chmod,     //do-nothings
    .chown = crest_chown,     //ditto
    .utime = crest_utime,     //same-a-rino
    
    .unlink = crest_unlink,
/*
    .mkdir = PUT (fuck MKCOL. fuck DAV. PUT with a slash at the end. M'k?)
    .unlink = (delete?)
    .rmdir = (still delete?)
    .symlink = (POST)
    .rename = MOVE - not liking new verbs today. maybe not. maybe DELETE followed by PUT
    .statfs - would be nice but I don't know how that would work....
    .flush??????????
    .fsync?
*/
};

/********************** Test routines **********************/

#define HOSTLEN 64
#define PATHLEN 1024


void pathtest(char *fullpath)
{
	char host[HOSTLEN];
	char path[PATHLEN];
	pathparse(fullpath,host,path,HOSTLEN,PATHLEN);
	brintf("Okay, from path '%s' host is: '%s' and path is: '%s'\n",fullpath,host,path);
}

void hdrtest(char *header,char *name)
{
	char pooh[90];
	fetchheader(header,name,pooh,80);
	brintf("Header name: %s: value: %s\n",name,pooh);
}

void strtest()
{
	char buffera[32];
	char bufferb[32];
	char bufferc[64];
	int result=0;

	result=strlcpy(bufferb,"Hello",32);
	brintf("strlcpy: %s, %d",bufferb,result);
	
	
	
	result=strlcpy(buffera,"HelloHelloHelloHelloHelloHelloHello",32);
	brintf("strlcpy: %s, %d",buffera,result);
	
	result= strlcat(bufferc,buffera,64);
	brintf("Strlcat: %s, %d",bufferc,result);

	result= strlcat(bufferc,buffera,64);
	brintf("strlcat: %s, %d",bufferc,result);

	result= strlcat(bufferc,buffera,64);
	brintf("strlcat: %s, %d",bufferc,result);
}


void pretest()
{
	char buf[1024*1024*1024];
/*	pathtest("/desk.nu");
	pathtest("/desk.nu/pooh.html");
	pathtest("/desk.nu/braydix");
	pathtest("/desk.nu/braydix/"); */
	//exit(0);
				
	//REGEX TESTING STUFF:
 /*	if(status) {     
		char error[80];
		regerror(status,&re,error,80);
		brintf("error: %s\n",error);
	} else {
		brintf("NO FAIL!!!!!!!!!!!\n");
	}
	exit(0);
	regfree(&re); */
	
	//webfetch("/desk.nu/braydix",buf,DIRBUFFER,"HEAD","");
	hdrtest(buf,"connection");
	hdrtest(buf,"Scheissen");
	hdrtest(buf,"etag");
	hdrtest(buf,"date");
	hdrtest(buf,"content-type");
	hdrtest(buf,"sErVeR");
	brintf("Header status: %d\n",fetchstatus(buf));
	brintf("Lookit: %s\n",buf);
	
	strtest();
	exit(0);
}

void authtest()
{
	char myauthstring[1024];
	fill_authorization("Dontcare",myauthstring,1024);
	
	printf("Authorization string is: %s\n",myauthstring);
	fill_authorization("my.lightdesktop.com/something/something",myauthstring,1024);
	printf("THen it's: %s\n",myauthstring);
	exit(0);
}

/********************** END TESTING **********************/

void
addparam(int *argc,char ***argv,char *string)
{
	char **previous=*argv;
	int newsize=(sizeof(char **))*(*argc+1);
	int i;
	//brintf("BEFORE\n");
	for(i=0;i<*argc;i++) {
		//brintf("[%d] %s, ",i,(*argv)[i]);
	}
	//brintf("\n");
	*argv=malloc(newsize);
	//brintf("malloc'ed: %d\n",newsize);
	if(*argc!=0) {
		int prevsize=(sizeof(char **))*(*argc);
		//brintf("Previous size: %d\n",prevsize);
		//brintf("previous sub-zero is: %s\n",previous[0]);
		memcpy(*argv,previous,prevsize);
		free(previous);
		(*argv)[*argc]=0;
	}
	//brintf("ABOUT TO ASSIGN '%s' to *argc: %d\n",string,*argc);
	(*argv)[*argc]=string;
	(*argc)++;
	for(i=0;i<*argc;i++) {
		//brintf("[%d] %s, ",i,(*argv)[i]);
	}
	//brintf("\n");
}

#ifndef TESTFRAMEWORK

char authfile[256];

#ifdef gnulibc
#include <mcheck.h>
#endif

int
main(int argc, char **argv)
{
	int myargc=0;
	char **myargs=0;
	int i=-1;
	
	//visible USAGE: 	./crestfs /tmp/doodle [cachedir]
//	pretest();
	
	//INVOKE AS: 	./crestfs /tmp/doodle /tmp/cacheplace -r -s -d -o nolocalcaches
	
	// single user, read-only. NB!
	
	#ifdef gnulibc	
	//mcheck(0);
	#endif
	
	if(argc<5) {
		printf("Not right number of args, you gave me %d\n",argc);
		printf("Usage: %s mountdir cachedir maxcacheage rootauthfile [options]\n",argv[0]);
		int j;
		for(j=0;j<argc;j++) {
			printf("%d: %s ",j,argv[j]);
		}
		printf("\n");
		exit(1);
	}
	//printf("Decent. Arg count: %d\n",argc);
	addparam(&myargc,&myargs,argv[0]);
	//myargs[0]=argv[0];
	addparam(&myargc,&myargs,argv[1]);
	#ifdef gnulibc
	mtrace();//do it AFTER addparam so we don't have to hear crap about it, it's supposed to leak
	#endif
	if(argv[1][0]!='/') {
		char *here=getcwd(NULL,0);
		strlcpy(rootdir,here,1024);
		strlcat(rootdir,"/",1024);
	}
	strlcat(rootdir,argv[1],1024); //first parameter, is mountpoint
	cachedir= open(argv[2], O_RDONLY); //second parameter is cachedir
	if(cachedir==-1) {
		printf("%d no open cachedir\n",cachedir);
		exit(2);
	}
	strlcpy(cachedirname,argv[2],1024);
	maxcacheage=atoi(argv[3]);
	if(maxcacheage<=0) {
		printf("%d is not a valid max cache age\n",maxcacheage);
		exit(3);
	}
	strlcpy(authfile,argv[4],256);
	printf("Authfile: %s\n",authfile);
		
	for(i=5;i<argc;i++) {
		addparam(&myargc,&myargs,argv[i]);
	}
	/////////addparam(&myargc,&myargs,"-r"); //no longer so nasty about read-only...areas indicated by teh authfile are NOT
	// -s ? -f ? -d ? 
	#ifdef __APPLE__
	addparam(&myargc,&myargs,"-o");
	addparam(&myargc,&myargs,"nolocalcaches");
	#else
	addparam(&myargc,&myargs,"-o");
	addparam(&myargc,&myargs,"nonempty");
	#endif
	//#endif
/* 	#ifdef __APPLE__
	char *myargs[]= {0, 0, "-r", "-s", "-f", "-d","-o","nolocalcaches", 0 };
	#define ARGCOUNT 8
	#else
	char *myargs[]= {0, 0, "-r", "-s", "-f", "-d", "-o", "nonempty", 0 };
	#define ARGCOUNT 8
	#endif */
	return fuse_main(myargc, myargs, &crest_filesystem_operations, NULL);
}
#else
int
main(int argc, char **argv)
{
	char headers[65535];
	char teenybuffer;
	FILE *resource=0;
	int isdirectory=-1;
	void *stupid=crest_filesystem_operations.init; //just to keep from complaining
	//FILE *get_resource(const char *path,char *headers,int headerlength, int *isdirectory,const char *preferredverb)
	resource=get_resource(argv[1],headers,65535,&isdirectory,argv[2],"testing");
	printf("IS Directory?: %d\n",isdirectory);
	if(resource) {
		while(fread(&teenybuffer,1,1,resource)) {
			printf("%c",teenybuffer);
		}
	} else {
		printf("NO RESOURCE FOUND. 'k?");
	}
	isdirectory=(int)stupid;
	return 0;
}
#endif
