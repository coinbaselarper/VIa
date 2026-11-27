#pragma once
#include <alloca.h>
#include <stdlib.h>
#include <malloc/malloc.h>
#include <gta6rt/msvc_crt.h>
#ifndef _alloca
#define _alloca alloca
#endif
#define _msize(p) malloc_size(p)
