#ifndef SYS_CFB_H
#define SYS_CFB_H

#include "ultra64.h"
#include "macros.h"

void SysCfb_SetLoResMode(void);
void SysCfb_SetHiResMode(void);
void SysCfb_Init(void);
void* SysCfb_GetFramebuffer(s32 index);
void* SysCfb_GetZBuffer(void);
void* SysCfb_GetWorkBuffer(void);
u16 SysCfb_GetZBufferPixel(s32 x, s32 y);
s32 SysCfb_GetZBufferInt(s32 x, s32 y);

extern void* gFramebuffers[2];
extern OSViMode* gActiveViMode;
extern u16* gZBufferPtr;
extern void* gWorkBuffer;
extern u64* gGfxSPTaskOutputBufferPtr;
extern void* gGfxSPTaskOutputBufferEnd;

// a Z/work buffer pixel: the N64's u16 on the Xbox (u16* elsewhere: each pixel a pointer, 4-8 bytes)
#ifdef LUS_XBOX
#define CFB_PIXEL u16
#else
#define CFB_PIXEL u16*
#endif

extern CFB_PIXEL gZBufferLoRes[SCREEN_WIDTH * SCREEN_HEIGHT];
extern CFB_PIXEL gWorkBufferLoRes[SCREEN_WIDTH * SCREEN_HEIGHT];
extern u64* gGfxSPTaskOutputBufferLoRes[0x3000];
extern void* gGfxSPTaskOutputBufferEndLoRes;

extern CFB_PIXEL gZBufferHiRes[HIRES_BUFFER_WIDTH * HIRES_BUFFER_HEIGHT];
extern CFB_PIXEL gWorkBufferHiRes[HIRES_BUFFER_WIDTH * HIRES_BUFFER_HEIGHT];
extern u64* gGfxSPTaskOutputBufferHiRes[0x3000];
extern void* gGfxSPTaskOutputBufferEndHiRes;
extern s16 gCfbWidth;
extern s16 gCfbHeight;
extern s16 gCfbLeftAdjust;
extern s16 gCfbUpperAdjust;
extern u8 gSysCfbHiResEnabled;

#endif
