#pragma once
#include <windows.h>

#ifndef __RPCNDR_H_VERSION__
#define __RPCNDR_H_VERSION__ 500
#endif
#ifndef CONST_VTBL
#ifdef CONST_VTABLE
#define CONST_VTBL const
#else
#define CONST_VTBL
#endif
#endif
typedef void* RPC_IF_HANDLE;
typedef unsigned char byte;
typedef unsigned char boolean;
