#pragma once
#include <minwindef.h>

typedef struct tagRECT {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
} RECT, *PRECT, *LPRECT;
typedef const RECT* LPCRECT;
typedef struct _RECTL {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
} RECTL, *PRECTL, *LPRECTL;
typedef struct tagPOINT {
    LONG x;
    LONG y;
} POINT, *PPOINT, *LPPOINT;
typedef struct _POINTL {
    LONG x;
    LONG y;
} POINTL, *PPOINTL;
typedef struct tagSIZE {
    LONG cx;
    LONG cy;
} SIZE, *PSIZE, *LPSIZE;
typedef SIZE SIZEL;
typedef DWORD COLORREF;
typedef struct tagPALETTEENTRY {
    BYTE peRed;
    BYTE peGreen;
    BYTE peBlue;
    BYTE peFlags;
} PALETTEENTRY, *PPALETTEENTRY, *LPPALETTEENTRY;
