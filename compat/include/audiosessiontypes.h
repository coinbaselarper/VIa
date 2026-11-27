#pragma once
#include <windows.h>

typedef enum _AUDCLNT_SHAREMODE { AUDCLNT_SHAREMODE_SHARED = 0, AUDCLNT_SHAREMODE_EXCLUSIVE = 1 } AUDCLNT_SHAREMODE;

#define AUDCLNT_STREAMFLAGS_CROSSPROCESS 0x00010000
#define AUDCLNT_STREAMFLAGS_LOOPBACK 0x00020000
#define AUDCLNT_STREAMFLAGS_EVENTCALLBACK 0x00040000
#define AUDCLNT_STREAMFLAGS_NOPERSIST 0x00080000
#define AUDCLNT_STREAMFLAGS_RATEADJUST 0x00100000
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000

typedef enum _AUDIO_STREAM_CATEGORY {
    AudioCategory_Other = 0,
    AudioCategory_ForegroundOnlyMedia = 1,
    AudioCategory_BackgroundCapableMedia = 2,
    AudioCategory_Communications = 3,
    AudioCategory_Alerts = 4,
    AudioCategory_SoundEffects = 5,
    AudioCategory_GameEffects = 6,
    AudioCategory_GameMedia = 7,
    AudioCategory_GameChat = 8,
    AudioCategory_Speech = 9,
    AudioCategory_Movie = 10,
    AudioCategory_Media = 11
} AUDIO_STREAM_CATEGORY;

typedef enum _AUDCLNT_STREAMOPTIONS {
    AUDCLNT_STREAMOPTIONS_NONE = 0,
    AUDCLNT_STREAMOPTIONS_RAW = 0x1,
    AUDCLNT_STREAMOPTIONS_MATCH_FORMAT = 0x2,
    AUDCLNT_STREAMOPTIONS_AMBISONICS = 0x4
} AUDCLNT_STREAMOPTIONS;
