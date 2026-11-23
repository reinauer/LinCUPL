/* Host C runtime differences used by the command-line tools. */
#ifndef LINCUPL_PLATFORM_H
#define LINCUPL_PLATFORM_H

#include <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <stdlib.h>
#include <sys/stat.h>

#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define access _access
#define chdir _chdir
#define getcwd _getcwd
#define getpid _getpid
#define unlink _unlink
#define fileno _fileno
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#define LINCUPL_EXE_SUFFIX ".exe"
#define LINCUPL_PATH_SEPARATOR ';'

static inline void lincupl_init_file_mode(void)
{
    /* The writers handle JEDEC CR/LF and checksums themselves. */
    _set_fmode(_O_BINARY);
}
#else
#include <strings.h>
#include <unistd.h>
#define LINCUPL_EXE_SUFFIX ""
#define LINCUPL_PATH_SEPARATOR ':'

static inline void lincupl_init_file_mode(void) {}
#endif

#endif
