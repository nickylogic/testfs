/*
  testfs: Virtual test file system built on FUSE
          by Matt Taylor <nickylogic@gmail.com>
          based on FUSE sample code by Miklos Szeredi <miklos@szeredi.hu> (GPL)

  Build:
  (install FUSE 2.9.2 first)
  	gcc -Wall testfs.c `pkg-config fuse --cflags --libs` -o testfs

  Start:
	mkdir <mount_dir>
	./testfs <mount_dir>

  Stop:
	fusermount -u <mount_dir>

  Use:
	Any path of the following form is the root a virtual directory tree:

		<mount_dir>/<file_size>x<c1>x<c2>x...x<cN>

		file_size = the size of each virtual file, consisting of an
                            integer followed by K (for kB), M (for MB) or 
                            G (for GB).

		cK        = the number of files or subdirectories in layer K
                            of the directory tree

	File paths within the virtual directory tree take the following form:

		<mount_dir>/<file_size>x<c1>x<c2>x...x<cN>/<a1>/<a2>/.../<aN>

		aK = a number from 0 to cK-1

	If the virtual file is opened and read, it appears to contain <file_size>
        random lowercase letters, with some newlines mixed in.

	The <mount_dir> itself is not presented as a valid directory, even though its
	virtual subdirectories are valid.

  Example:

	$ ./testfs /tmp/testfs
	fuse: warning: library too old, some operations may not not work
	$ ls -l /tmp/testfs/1kx5x4
	total 0
	drwxr-xr-x 2 root root 0 Dec 31  1969 0
	drwxr-xr-x 2 root root 0 Dec 31  1969 1
	drwxr-xr-x 2 root root 0 Dec 31  1969 2
	drwxr-xr-x 2 root root 0 Dec 31  1969 3
	drwxr-xr-x 2 root root 0 Dec 31  1969 4
	$ ls -l /tmp/testfs/1kx5x4/2
	total 0
	-r--r--r-- 1 root root 1024 Dec 31  1969 0
	-r--r--r-- 1 root root 1024 Dec 31  1969 1
	-r--r--r-- 1 root root 1024 Dec 31  1969 2
	-r--r--r-- 1 root root 1024 Dec 31  1969 3
	$ cat /tmp/testfs/1kx5x4/2/3
	hovcjqxelszgnubipwdkryfmtahovcj
	xelszgnubipwdkryfmtahovcjqxelsz
	nubipwdkryfmtahovcjqxelszgnubip
	dkryfmtahovcjqxelszgnubipwdkryf
	tahovcjqxelszgnubipwdkryfmtahov
	...
	$ find /tmp/testfs/1kx10x10x10x10 -type f -print | wc -l
	10000
	$ du -sb /tmp/testfs/1kx10x10x10x10
	10240000	/tmp/testfs/1kx10x10x10x10
	$ fusermount -u /tmp/testfs 
*/

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>

#define TOKLEN 32
#define MAXTOK 16
#define MAXWIDTH 1000000

// the root directory name of a virtual tree
struct root_struct {
	int fsize;		// size of virtual files in the tree
	int depth;		// depth of the tree
	int width[MAXTOK];	// number of subdirectories or files at each layer
};

static int parse_root(char *str,struct root_struct *root)
{
	static char buf[256];
	char *p;
	int val;

	strcpy(buf,str);

	// Parse the file size, recognizing K=1024, M=1024^2, G=1024^3
	// for kilobytes, megabytes or gigabytes.  
	p = strtok(buf,"x");
	if ( p == NULL ) {
		return -1;
	} else if ( sscanf(p,"%dk",&val) == 1 ) {
		root->fsize = val*1024;
	} else if ( sscanf(p,"%dK",&val) == 1 ) {
		root->fsize = val*1024;
	} else if ( sscanf(p,"%dm",&val) == 1 ) {
		root->fsize = val*1024*1024;
	} else if ( sscanf(p,"%dm",&val) == 1 ) {
		root->fsize = val*1024*1024;
	} else if ( sscanf(p,"%dM",&val) == 1 ) {
		root->fsize = val*1024*1024;
	} else if ( sscanf(p,"%dg",&val) == 1 ) {
		root->fsize = val*1024*1024*1024;
	} else if ( sscanf(p,"%dG",&val) == 1 ) {
		root->fsize = val*1024*1024*1024;
	} else if ( sscanf(p,"%d",&val) == 1 ) {
		root->fsize = val;
	} else {
		return -1;
	}

	// Parse the width of each directory tree layer, and
	// count the number of layers as the depth
	memset (&(root->width[0]),0,sizeof(int)*MAXTOK);
	root->depth = 0;
	p = strtok(NULL,"x");
	while ( p != NULL && root->depth < MAXTOK ) {
		if ( strlen(p) == 0 ) continue;
		val = atoi(p);
		if ( val <= 0 || val > MAXWIDTH ) return -1;
		root->width[root->depth] = val;
		root->depth++;
		p = strtok(NULL,"x");
	}
	return 0;
}
	

static int seed_from_path(char *path)
{
	unsigned int x = 0;
	unsigned int seed = 0;
	char *p;

	// Repeatably generate a pseudo-random number from a path string
	for ( p=path; *p; p++ ) {
		seed += (*p)*x;
		x = *p;
	}
	return seed;
}

static int parse_path(char *path, char *tok[])
{
	char *p;
	int n = 0;
	static char buf[256];

	strcpy(buf,path);
	p = strtok(buf,"/");
	while ( p != NULL && n < MAXTOK ) {
		tok[n] = p;
		p = strtok(NULL,"/");
		n++;
	}
	return n;
}


static int testfs_getattr(const char *path, struct stat *stbuf)
{
	char pathbuf[256];
	char *tok[MAXTOK];
	int ntok = 0;
	int res = 0;
	struct root_struct root;

	// root directory of testfs is invalid
	if (strcmp(path, "/") == 0) return -ENOENT;

	memset(stbuf, 0, sizeof(struct stat));
	strcpy(pathbuf,path);
	ntok = parse_path(pathbuf,&(tok[0]));
	if (ntok < 1) return -ENOENT;
	if (parse_root(tok[0],&root) < 0) return -ENOENT;

	if (ntok <= root.depth) {
		// looks like a directory
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	} else if (ntok == root.depth+1) {
		// looks like a file
		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_nlink = 1;
		stbuf->st_size = root.fsize;
	} else {
		// path is too deep
		res = -ENOENT;
	}

	return res;
}

static int testfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	(void) offset;
	(void) fi;
	char pathbuf[256];
	char *tok[MAXTOK];
	int ntok = 0;
	int i;
	struct root_struct root;

	// root directory of testfs is invalid
	if (strcmp(path, "/") == 0) return -ENOENT;

	strcpy(pathbuf,path);
	ntok = parse_path(pathbuf,&(tok[0]));
	if (ntok < 1) return -ENOENT;
	if (parse_root(tok[0],&root) < 0) return -ENOENT;
	if (ntok > root.depth) return -ENOENT; // path is too deep, or its a file

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	for ( i=0; i<root.width[ntok-1]; i++ ) {
		// List the numbers 0 to (width) as subdirectories or files
		char fname[64];
		sprintf(fname,"%d",i);
		filler(buf, fname, NULL, 0);
	}

	return 0;
}

static int testfs_open(const char *path, struct fuse_file_info *fi)
{
	char pathbuf[256];
	char *tok[MAXTOK];
	int ntok = 0;
	struct root_struct root;
	int val;

	strcpy(pathbuf,path);
	ntok = parse_path(pathbuf,&(tok[0]));
	if (ntok < 1) return -ENOENT;
	if (parse_root(tok[0],&root) < 0) return -ENOENT;
	if (ntok != root.depth+1) return -ENOENT; // not a file
	if (sscanf(tok[ntok-1],"%d",&val) != 1) return -ENOENT; // file name is not a number
	if (val<0 || val >= root.width[root.depth-1]) return -ENOENT;
	if ((fi->flags & 3) != O_RDONLY) return -EACCES;

	return 0;
}

static int testfs_read(const char *path, char *buf, size_t size, off_t offset,
		      struct fuse_file_info *fi)
{
	(void) fi;
	char pathbuf[256];
	char *tok[MAXTOK];
	int ntok = 0;
	struct root_struct root;
	int val;
	int i,j;
	int seed;

	strcpy(pathbuf,path);
	ntok = parse_path(pathbuf,&(tok[0]));
	if (ntok < 1) return -ENOENT;
	if (parse_root(tok[0],&root) < 0) return -ENOENT;
	if (ntok != root.depth+1) return -ENOENT; // not a file
	if (sscanf(tok[ntok-1],"%d",&val) != 1) return -ENOENT; // file name is not a number
	if (val<0 || val >= root.width[root.depth-1]) return -ENOENT; // file number out of range
	if ((fi->flags & 3) != O_RDONLY) return -EACCES;

	if ( root.fsize-offset < size ) size = root.fsize-offset;
	if ( size <= 0 ) return 0;

	// Generate quasi-random letters representing the contents of the virtual file
	seed = seed_from_path(pathbuf);
	for ( i=0; i<size; i++ ) {
		j = i+offset;
		if ( j%32==31 || j==root.fsize-1 ) {
			buf[i] = '\n';
		} else {
			buf[i] = 'a' + (seed+(j*1723))%26; 
		}
	}

	return size;
}

static struct fuse_operations testfs_oper = {
	.getattr	= testfs_getattr,
	.readdir	= testfs_readdir,
	.open		= testfs_open,
	.read		= testfs_read,
};

int main(int argc, char *argv[])
{
	return fuse_main(argc, argv, &testfs_oper, NULL);
}
