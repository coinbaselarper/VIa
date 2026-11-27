#pragma once
#include <fcntl.h>
#include <unistd.h>
#include <gta6rt/msvc_crt.h>
#define _close close
#define _dup dup
#define _dup2 dup2
#define _read read
#define _write write
#define _isatty isatty
#define _fileno fileno
#define _access access
#define _unlink unlink
#define _O_RDONLY O_RDONLY
#define _O_WRONLY O_WRONLY
#define _O_RDWR O_RDWR
#define _O_CREAT O_CREAT
#define _O_TRUNC O_TRUNC
#define _O_APPEND O_APPEND
#define _O_BINARY 0
#define _O_TEXT 0
