/** File I/O
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "fileio.h"
#include "memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#ifdef _WIN32
#include <io.h>
#include <direct.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define fileGetCwd _getcwd
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#define fileGetCwd getcwd
#endif

char **fileSearchPaths = NULL;

/** Load a file into an allocated string, return pointer or NULL if not found */
char *fileLoad(char *fn) {
    FILE *file;
    size_t filesize;
    char *filestr;

    // Open the file - return null on failure
    if (!(file = fopen(fn, "rb")))
        return NULL;

    // Determine the file length (so we can accurately allocate memory)
    fseek(file, 0, SEEK_END);
    filesize=ftell(file);
    fseek(file, 0, SEEK_SET);

    // Load the data into an allocated string buffer and close file
    filestr = memAllocStr(NULL, filesize);
    fread(filestr, 1, filesize, file);
    filestr[filesize]='\0';
    fclose(file);
    return filestr;
}

/** Extract a filename only (no extension) from a path */
char *fileName(char *fn) {
    char *dotp;
    char *fnp = &fn[strlen(fn)-1];

    // Look backwards for '.' If not found, we are done
    while (fnp != fn && *fnp != '.' && *fnp != '/' && *fnp != '\\')
        --fnp;
    if (fnp == fn)
        return fn;
    if (*fnp == '/' || *fnp == '\\')
        return fnp + 1;

    // Look backwards for slash
    dotp = fnp;
    while (fnp != fn && *fnp != '/' && *fnp != '\\')
        --fnp;
    if (fnp != fn)
        ++fnp;

    // Create string to hold filename and return
    return memAllocStr(fnp, dotp-fnp);
}

/** Concatenate folder, filename and extension into a path */
char *fileMakePath(char *dir, char *srcfn, char *ext) {
    char *outnm;
    if (dir == NULL)
        dir = "";
    outnm = memAllocStr(dir, strlen(dir) + strlen(srcfn) + strlen(ext) + 2);
    if (strlen(dir) && outnm[strlen(outnm) - 1] != '/' && outnm[strlen(outnm) - 1] != '\\')
        strcat(outnm, "/");
    strcat(outnm, srcfn);
    strcat(outnm, ".");
    strcat(outnm, ext);
    return outnm;
}

// Get number of characters in string up to file name
size_t fileFolder(char *fn) {
    char *fnp = &fn[strlen(fn) - 1];

    // Look backwards for '/' If not found, we are done
    while (fnp != fn && *fnp != '/' && *fnp != '\\')
        --fnp;
    if (fnp == fn)
        return 0;
    return fnp - fn + 1;
}

// Return position of last period (after last slash)
char *fileExtPos(char *fn) {
    char *dotpos = strrchr(fn, '.');
    return dotpos && dotpos > strrchr(fn, '/') ? dotpos : 0;
}

// Return position of filename (after last slash)
char *fileNamePos(char *fn) {
    char *slashpos = strrchr(fn, '/');
    return slashpos? slashpos + 1 : fn;
}

// Create a new source file url relative to current, substituting new path and .cone extension
char *fileSrcUrl(char *cururl, char *srcfn, int newfolder) {
    if (cururl == NULL)
        cururl = "";
    char *extp = fileExtPos(srcfn);
    char *fnamep = fileNamePos(srcfn);

    // Calculate how large composed string needs to be, then allocate space
    size_t outnmsz = strlen(cururl) + strlen(srcfn) + 1;
    if (newfolder)
        outnmsz += strlen(fnamep) + 1;
    if (!extp)
        outnmsz += strlen(".cone");
    char *outnm = memAllocStr("", outnmsz);

    // Compose full file path
    if (cururl && srcfn[0]!='/')
        strncat(outnm, cururl, fileFolder(cururl));
    strcat(outnm, srcfn);
    if (newfolder) {
        // Look for file inside name as folder
        if (extp)
            *fileExtPos(outnm) = 0;
        strcat(outnm, "/");
        strcat(outnm, fnamep);
    }
    if (!extp)
        strcat(outnm, ".cone");
    return outnm;
}

// Whether a path names a file that can be opened for reading
static int fileReadable(char *fn) {
    FILE *file = fopen(fn, "rb");
    if (file == NULL)
        return 0;
    fclose(file);
    return 1;
}

// Find the source file srcfn names relative to cururl: 'srcfn.cone' first, then
// 'srcfn/srcfn.cone', which is the designated file of the folder srcfn names
static char *fileFindSrcWithFolder(char *cururl, char *srcfn) {
    char *fn = fileSrcUrl(cururl, srcfn, 0);
    if (fileReadable(fn))
        return fn;
    fn = fileSrcUrl(cururl, srcfn, 1);
    return fileReadable(fn) ? fn : NULL;
}

// The one spelling of a path, so that two ways of writing one file are one key.
//
// The file registry is keyed by the path, and what must happen exactly once is
// the reading -- so a path reaching a file the compiler already holds must hash
// to the same key however it was written. A source can write one that does not:
// 'import "../b/b"' composes a path down into a folder and back out of it, and
// without this the registry misses the entry the folder sweep made for that file
// and a SECOND module is built from it, spelling the same symbols.
//
// Separators become '/', a '.' segment goes, and a '..' segment cancels the
// segment in front of it. A leading '..' has nothing to cancel and is kept, so a
// path that climbs above where it started still names what it named.
char *fileCanonicalPath(char *path) {
    size_t len = strlen(path);
    char *out = memAllocStr(path, len);
    // Where each kept segment starts, so a '..' knows what it may cancel. What
    // comes before the first segment -- a leading '/', a drive letter -- is not
    // one and is never cancelled
    size_t *starts = (size_t*)memAllocBlk((len + 2) * sizeof(size_t));
    uint32_t depth = 0;
    size_t o = 0;
    size_t i = 0;
    if (len > 1 && path[1] == ':') {
        out[o++] = path[0];
        out[o++] = ':';
        i = 2;
    }
    while (path[i] == '/' || path[i] == '\\') {
        out[o++] = '/';
        ++i;
    }
    size_t prefix = o;
    while (i < len) {
        size_t seg = i;
        while (i < len && path[i] != '/' && path[i] != '\\')
            ++i;
        size_t seglen = i - seg;
        int sep = i < len;
        while (i < len && (path[i] == '/' || path[i] == '\\'))
            ++i;
        if (seglen == 0 || (seglen == 1 && path[seg] == '.'))
            continue;
        if (seglen == 2 && path[seg] == '.' && path[seg + 1] == '.' && depth > 0) {
            o = starts[--depth];
            continue;
        }
        starts[depth++] = o;
        memcpy(out + o, path + seg, seglen);
        o += seglen;
        if (sep)
            out[o++] = '/';
    }
    // A relative path that cancelled itself away names the folder it started in
    if (o == prefix && prefix == 0)
        out[o++] = '.';
    out[o] = '\0';
    return out;
}

// Find the source file srcfn names relative to cururl alone, and hand back the
// one spelling of what it found
char *fileFindLocal(char *cururl, char *srcfn) {
    char *fn = fileFindSrcWithFolder(cururl, srcfn);
    return fn ? fileCanonicalPath(fn) : NULL;
}

// Find the source file srcfn names on the package search path alone: each
// folder in order, the packages folder last (coneopts.c)
char *fileFindPackage(char *srcfn) {
    char **searchPaths = fileSearchPaths;
    if (searchPaths == NULL)
        return NULL;
    while (*searchPaths) {
        char *fn = fileFindSrcWithFolder(*searchPaths++, srcfn);
        if (fn)
            return fileCanonicalPath(fn);
    }
    return NULL;
}

// Find the source file srcfn names, relative to cururl and then on each search
// path, and hand back the one spelling of what it found
char *fileFindSrc(char *cururl, char *srcfn) {
    char *fn = fileFindLocal(cururl, srcfn);
    return fn ? fn : fileFindPackage(srcfn);
}

// The name of the current directory, or NULL where there is none to read. A file
// named with no folder in front of it sits here, and this is the only place its
// folder's name can be read: a file's module must not depend on the spelling of
// the path used to reach it
char *fileCurFolderName() {
    char buf[1024];
    if (fileGetCwd(buf, sizeof(buf)) == NULL)
        return NULL;
    // Drop the trailing separator a filesystem root carries
    size_t len = strlen(buf);
    while (len && (buf[len - 1] == '/' || buf[len - 1] == '\\'))
        buf[--len] = '\0';
    char *name = buf + len;
    while (name != buf && name[-1] != '/' && name[-1] != '\\')
        --name;
    return *name ? memAllocStr(name, strlen(name)) : NULL;
}

// The folder holding the running executable, canonical and ending in '/', or
// NULL where the platform will not say. Asked of the operating system rather
// than read from argv[0], which names whatever the shell was given
char *fileExeFolder() {
    char buf[4096];
    size_t len = 0;
#ifdef _WIN32
    DWORD got = GetModuleFileNameA(NULL, buf, sizeof(buf));
    if (got == 0 || got >= sizeof(buf))
        return NULL;
    len = got;
#elif defined(__APPLE__)
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0)
        return NULL;
    len = strlen(buf);
#elif defined(__linux__)
    ssize_t got = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (got <= 0)
        return NULL;
    len = (size_t)got;
#else
    return NULL;
#endif
    buf[len] = '\0';
    char *path = fileCanonicalPath(buf);
    size_t folder = fileFolder(path);
    if (folder == 0)
        return NULL;
    path[folder] = '\0';
    return path;
}

// The path of the designated file a folder holds, or NULL where it holds none.
// The folder's name and the file's basename are the same string, which is what
// lets a module's name be read off a path by a tool that cannot parse Cone
char *fileDesignatedFile(char *folder, char *name) {
    char *path = memAllocStr(folder, strlen(folder) + strlen(name) + 5);
    strcat(path, name);
    strcat(path, ".cone");
    return fileReadable(path) ? path : NULL;
}

// Order two names, so that a folder's contents are read in one order whatever
// order the filesystem reports them in
static int fileNameCmp(const void *left, const void *right) {
    return strcmp(*(const char **)left, *(const char **)right);
}

void fileNamesInit(FileNames *list) {
    list->names = NULL;
    list->count = 0;
    list->avail = 0;
}

// Append a copy of name to a list, growing it
void fileNamesAdd(FileNames *list, char *name) {
    if (list->count == list->avail) {
        list->avail = list->avail ? list->avail * 2 : 8;
        char **grown = (char **)memAllocBlk(list->avail * sizeof(char *));
        for (uint32_t i = 0; i < list->count; ++i)
            grown[i] = list->names[i];
        list->names = grown;
    }
    list->names[list->count++] = memAllocStr(name, strlen(name));
}

// Whether a name ends in '.cone'. The extension rule is what keeps '.orig',
// '.rej', 'foo.cone~' and editor droppings out of a module's file set
static int fileIsCone(char *name) {
    size_t len = strlen(name);
    return len > 5 && strcmp(name + len - 5, ".cone") == 0;
}

// Scan a folder for the '.cone' files and the subfolders it holds
int fileFolderScan(char *folder, FileNames *cones, FileNames *folders) {
    if (cones)
        fileNamesInit(cones);
    if (folders)
        fileNamesInit(folders);
    // Every composed path below is folder + a name, so the folder carries its
    // trailing slash. An empty folder is the current directory
    if (folder == NULL || *folder == '\0')
        folder = "./";
    else if (folder[strlen(folder) - 1] != '/' && folder[strlen(folder) - 1] != '\\') {
        char *slashed = memAllocStr(folder, strlen(folder) + 1);
        strcat(slashed, "/");
        folder = slashed;
    }

#ifdef _WIN32
    char *pattern = memAllocStr(folder, strlen(folder) + 1);
    strcat(pattern, "*");
    struct _finddata_t found;
    intptr_t handle = _findfirst(pattern, &found);
    if (handle == -1)
        return 0;
    do {
        if (found.name[0] == '.')
            continue;
        if (found.attrib & _A_SUBDIR) {
            if (folders)
                fileNamesAdd(folders, found.name);
        }
        else if (cones && fileIsCone(found.name))
            fileNamesAdd(cones, found.name);
    } while (_findnext(handle, &found) == 0);
    _findclose(handle);
#else
    DIR *dir = opendir(folder);
    if (dir == NULL)
        return 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;
        char *path = memAllocStr(folder, strlen(folder) + strlen(entry->d_name) + 1);
        strcat(path, entry->d_name);
        struct stat info;
        if (stat(path, &info) != 0)
            continue;
        if (S_ISDIR(info.st_mode)) {
            if (folders)
                fileNamesAdd(folders, entry->d_name);
        }
        else if (cones && fileIsCone(entry->d_name))
            fileNamesAdd(cones, entry->d_name);
    }
    closedir(dir);
#endif

    if (cones && cones->count > 1)
        qsort(cones->names, cones->count, sizeof(char *), fileNameCmp);
    if (folders && folders->count > 1)
        qsort(folders->names, folders->count, sizeof(char *), fileNameCmp);
    return 1;
}
