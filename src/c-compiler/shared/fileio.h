/** File i/o
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef fileio_h
#define fileio_h

#include <stddef.h>
#include <stdint.h>

extern char **fileSearchPaths;

// Load a file into an allocated string, return pointer or NULL if not found
char *fileLoad(char *fn);

// Extract a filename only (no extension) from a path
char *fileName(char *fn);

// Concatenate folder, filename and extension into a path
char *fileMakePath(char *dir, char *srcfn, char *ext);

// Create a new source file url relative to current, substituting new path and .cone extension
char *fileSrcUrl(char *cururl, char *srcfn, int newfolder);

// Find the source file srcfn names, relative to cururl and then on each search
// path, trying 'srcfn.cone' and then 'srcfn/srcfn.cone' -- the designated-file
// convention. Returns the path it found, or NULL.
//
// Locating a file is separate from reading it, because what must happen exactly
// once is the reading: the path is what the file registry is keyed by, and it
// has to be in hand before anything can ask whether this file is already held
char *fileFindSrc(char *cururl, char *srcfn);

// Number of characters in a path up to and including the slash before its
// filename, or 0 where the path carries no folder
size_t fileFolder(char *fn);

// The name of the current directory, or NULL where there is none to read
char *fileCurFolderName();

// A growable list of names or paths
typedef struct FileNames {
    char **names;
    uint32_t count;
    uint32_t avail;
} FileNames;

void fileNamesInit(FileNames *list);
void fileNamesAdd(FileNames *list, char *name);

// Scan a folder for the '.cone' files and the subfolders it holds, returning 0
// where the folder cannot be read. Either list may be NULL to skip it. Each
// list comes back sorted, so that a folder's contents are read in one order
// whatever order the filesystem reports them in
int fileFolderScan(char *folder, FileNames *cones, FileNames *folders);

#endif
