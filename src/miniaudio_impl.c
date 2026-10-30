// miniaudio implementation, with stb_vorbis for .ogg sounds
#define STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>
#define MA_NO_GENERATION
#define MA_NO_ENCODING
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
#undef STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>
