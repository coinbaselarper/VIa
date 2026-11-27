#pragma once
#include <windows.h>
#include <guiddef.h>

/* Wave format structures. Layouts are the Windows ones (packed, 1-byte alignment). */
#pragma pack(push, 1)
typedef struct tWAVEFORMATEX {
    WORD wFormatTag;
    WORD nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD nBlockAlign;
    WORD wBitsPerSample;
    WORD cbSize;
} WAVEFORMATEX, *PWAVEFORMATEX, *NPWAVEFORMATEX, *LPWAVEFORMATEX;
typedef const WAVEFORMATEX* LPCWAVEFORMATEX;

typedef struct {
    WAVEFORMATEX Format;
    union {
        WORD wValidBitsPerSample;
        WORD wSamplesPerBlock;
        WORD wReserved;
    } Samples;
    DWORD dwChannelMask;
    GUID SubFormat;
} WAVEFORMATEXTENSIBLE, *PWAVEFORMATEXTENSIBLE;
#pragma pack(pop)

#define WAVE_FORMAT_PCM 1
#define WAVE_FORMAT_IEEE_FLOAT 0x0003
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE

#define SPEAKER_FRONT_LEFT 0x1
#define SPEAKER_FRONT_RIGHT 0x2
#define SPEAKER_FRONT_CENTER 0x4
#define SPEAKER_LOW_FREQUENCY 0x8
#define SPEAKER_BACK_LEFT 0x10
#define SPEAKER_BACK_RIGHT 0x20
#define SPEAKER_BACK_CENTER 0x100
#define SPEAKER_SIDE_LEFT 0x200
#define SPEAKER_SIDE_RIGHT 0x400
