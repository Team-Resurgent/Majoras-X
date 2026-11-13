/*
 * File: z_eff_ss_dust.c
 * Overlay: ovl_Effect_Ss_Dust
 * Description: Dust Particle Effect
 */

#include "z_eff_ss_dust.h"
#include "objects/gameplay_keep/gameplay_keep.h"

#define rPrimColorR regs[0]
#define rPrimColorG regs[1]
#define rPrimColorB regs[2]
#define rPrimColorA regs[3]
#define rEnvColorR regs[4]
#define rEnvColorG regs[5]
#define rEnvColorB regs[6]
#define rEnvColorA regs[7]
#define rTexIndex regs[8] // this reg is also used to set specific colors in the fire update function
#define rScale regs[9]
#define rScaleStep regs[10]
#define rDrawFlags regs[11]
#define rLifespan regs[12]

#define PARAMS ((EffectSsDustInitParams*)initParamsx)

u32 EffectSsDust_Init(PlayState* play, u32 index, EffectSs* this, void* initParamsx);
void EffectSsDust_Update(PlayState* play, u32 index, EffectSs* this);
void EffectSsDust_UpdateFire(PlayState* play, u32 index, EffectSs* this);
void EffectSsDust_Draw(PlayState* play, u32 index, EffectSs* this);

EffectSsProfile Effect_Ss_Dust_Profile = {
    EFFECT_SS_DUST,
    EffectSsDust_Init,
};

static EffectSsUpdateFunc sUpdateFuncs[] = {
    EffectSsDust_Update,
    EffectSsDust_UpdateFire,
};

static TexturePtr sDustTextures[] = {
    gEffDust1Tex, gEffDust2Tex, gEffDust3Tex, gEffDust4Tex, gEffDust5Tex, gEffDust6Tex, gEffDust7Tex, gEffDust8Tex,
};

u32 EffectSsDust_Init(PlayState* play, u32 index, EffectSs* this, void* initParamsx) {
    EffectSsDustInitParams* initParams = PARAMS;

    Math_Vec3f_Copy(&this->pos, &initParams->pos);
    Math_Vec3f_Copy(&this->velocity, &initParams->velocity);
    Math_Vec3f_Copy(&this->accel, &initParams->accel);
    this->gfx = gEffDustDL;
    this->life = initParams->life;
    this->update = sUpdateFuncs[initParams->updateMode];
    this->draw = EffectSsDust_Draw;

    if (initParams->drawFlags & DUST_DRAWFLAG_RAND_COLOR_OFFSET) {
        s32 randColorOffset = Rand_ZeroOne() * 20.0f - 10.0f;

        this->rPrimColorR = initParams->primColor.r + randColorOffset;
        this->rPrimColorG = initParams->primColor.g + randColorOffset;
        this->rPrimColorB = initParams->primColor.b + randColorOffset;
        this->rEnvColorR = initParams->envColor.r + randColorOffset;
        this->rEnvColorG = initParams->envColor.g + randColorOffset;
        this->rEnvColorB = initParams->envColor.b + randColorOffset;
    } else {
        this->rPrimColorR = initParams->primColor.r;
        this->rPrimColorG = initParams->primColor.g;
        this->rPrimColorB = initParams->primColor.b;
        this->rEnvColorR = initParams->envColor.r;
        this->rEnvColorG = initParams->envColor.g;
        this->rEnvColorB = initParams->envColor.b;
    }

    this->rPrimColorA = initParams->primColor.a;
    this->rEnvColorA = initParams->envColor.a;
    this->rTexIndex = 0;
    this->rScale = initParams->scale;
    this->rScaleStep = initParams->scaleStep;
    this->rLifespan = initParams->life;
    this->rDrawFlags = initParams->drawFlags;

    return 1;
}

#ifdef LUS_XBOX
// The game drew these at 320x240. At 720p a puff next to the camera covers most of the screen, and a trail of them is
// more blended pixels than the GPU paints in a frame (over 40 screens' worth in a tick, rolling through Termina Field).
// Two things keep them affordable. Each puff is drawn as an eight-sided shape around the pixels its texture really has,
// not the whole square: the same picture for about 40% fewer pixels, over half on the oldest textures. And a tick's
// smoke has a budget: when it's over, the tallest puffs on screen fade out, those being the costliest. Under it, all of
// the smoke is drawn.
#define DUST_BUDGET_SCREENS 5.0f // 4.8 of them held 60 in Termina Field
#define DUST_LIMIT_MAX 2.0f      // of the screen's height: a taller puff has the camera in it
#define DUST_LIMIT_MIN 0.3f
#define DUST_LIMIT_FALL 0.7f // a tick's change: it follows the smoke, so puffs fade and don't pop
#define DUST_LIMIT_RISE 1.05f
#define DUST_FADE_FROM 0.6f // of the limit
#define DUST_QUAD_SIZE 40.0f // gEffDustDL's quad at scale 1
#define DUST_MAX_PUFFS 128

// an outline's corner, from the texture's 1/32 texel coordinates: on the quad's own mapping, x -19..20 to s 0..1024
#define DUST_X(s) (((s) * 39 + 512) / 1024 - 19)
#define DUST_S(s) ((DUST_X(s) + 19) * 1024 / 39)
#define DUST_VTX(s, t) VTX(DUST_X(s), DUST_X(t), 0, DUST_S(s), DUST_S(t), 0, 127, 0, 255)

// the eight sides around each texture's texels of alpha 8 and up (the renderer's alpha test drops the rest), a texel out
static Vtx sDustOutlines[8][8] = {
    { DUST_VTX(912, 752), DUST_VTX(752, 912), DUST_VTX(304, 912), DUST_VTX(80, 688), DUST_VTX(80, 336),
      DUST_VTX(304, 112), DUST_VTX(720, 112), DUST_VTX(912, 304) },
    { DUST_VTX(944, 752), DUST_VTX(752, 944), DUST_VTX(304, 944), DUST_VTX(48, 688), DUST_VTX(48, 336),
      DUST_VTX(304, 80), DUST_VTX(720, 80), DUST_VTX(944, 304) },
    { DUST_VTX(976, 752), DUST_VTX(752, 976), DUST_VTX(304, 976), DUST_VTX(16, 688), DUST_VTX(16, 336),
      DUST_VTX(304, 48), DUST_VTX(720, 48), DUST_VTX(976, 304) },
    { DUST_VTX(976, 784), DUST_VTX(784, 976), DUST_VTX(304, 976), DUST_VTX(16, 688), DUST_VTX(16, 304),
      DUST_VTX(272, 48), DUST_VTX(688, 48), DUST_VTX(976, 336) },
    { DUST_VTX(976, 784), DUST_VTX(816, 944), DUST_VTX(272, 944), DUST_VTX(48, 720), DUST_VTX(48, 272),
      DUST_VTX(272, 48), DUST_VTX(688, 48), DUST_VTX(976, 336) },
    { DUST_VTX(976, 752), DUST_VTX(816, 912), DUST_VTX(272, 912), DUST_VTX(48, 688), DUST_VTX(48, 304),
      DUST_VTX(304, 48), DUST_VTX(688, 48), DUST_VTX(976, 336) },
    { DUST_VTX(912, 784), DUST_VTX(784, 912), DUST_VTX(368, 912), DUST_VTX(144, 688), DUST_VTX(144, 240),
      DUST_VTX(272, 112), DUST_VTX(752, 112), DUST_VTX(912, 272) },
    { DUST_VTX(912, 688), DUST_VTX(752, 848), DUST_VTX(336, 848), DUST_VTX(208, 720), DUST_VTX(208, 208),
      DUST_VTX(272, 144), DUST_VTX(752, 144), DUST_VTX(912, 304) },
};
// each outline's part of the square
static const f32 sDustOutlineAreas[8] = { 0.56f, 0.63f, 0.72f, 0.72f, 0.69f, 0.66f, 0.53f, 0.44f };

extern EffectSsInfo sEffectSsInfo;

// this tick's puffs and the screens they cover, for the log (z_play.c)
u32 gLusxDustDrawn, gLusxDustSkipped;
f32 gLusxDustArea, gLusxDustSkippedArea;
f32 gLusxDustLowestLimit = DUST_LIMIT_MAX;

static f32 sDustLimit = DUST_LIMIT_MAX;
static u32 sDustLimitFrame = 0xFFFFFFFF;
static Vec3f sDustLook;       // the camera's direction
static f32 sDustHeightFactor; // a puff's height on screen is this times its scale over its depth

static s32 EffectSsDust_Texture(EffectSs* this) {
    return (this->rTexIndex < ARRAY_COUNT(sDustTextures)) ? this->rTexIndex : ARRAY_COUNT(sDustTextures) - 1;
}

// the puff's height as a part of the screen's. A billboard behind the camera is clipped and costs nothing: 0.
static f32 EffectSsDust_ScreenHeight(PlayState* play, EffectSs* this) {
    Vec3f toPuff;
    f32 depth;

    Math_Vec3f_Diff(&this->pos, &play->view.eye, &toPuff);
    depth = sDustLook.x * toPuff.x + sDustLook.y * toPuff.y + sDustLook.z * toPuff.z;
    return (depth > 0.0f) ? sDustHeightFactor * this->rScale / depth : 0.0f;
}

// the part of the screen it paints
static f32 EffectSsDust_ScreenArea(f32 height, s32 texture) {
    f32 width = height * (9.0f / 16.0f);

    return ((height < 1.0f) ? height : 1.0f) * ((width < 1.0f) ? width : 1.0f) * sDustOutlineAreas[texture];
}

// once a tick: the height the budget reaches when the smoke is added up from the smallest puff
static void EffectSsDust_UpdateLimit(PlayState* play) {
    View* view = &play->view;
    f32 heights[DUST_MAX_PUFFS];
    f32 areas[DUST_MAX_PUFFS];
    f32 lookLength;
    f32 total = 0.0f;
    f32 target = DUST_LIMIT_MAX;
    s32 count = 0;
    s32 i;

    Math_Vec3f_Diff(&view->at, &view->eye, &sDustLook);
    lookLength = sqrtf(SQ(sDustLook.x) + SQ(sDustLook.y) + SQ(sDustLook.z));
    if (lookLength > 0.0f) {
        sDustLook.x /= lookLength;
        sDustLook.y /= lookLength;
        sDustLook.z /= lookLength;
    }
    sDustHeightFactor = DUST_QUAD_SIZE * 0.0025f / (2.0f * Math_FTanF(DEG_TO_RAD(view->fovy * 0.5f)));

    for (i = 0; (i < sEffectSsInfo.tableSize) && (count < DUST_MAX_PUFFS); i++) {
        EffectSs* effect = &sEffectSsInfo.table[i];
        f32 height;
        s32 at;

        if ((effect->life < 0) || (effect->draw != EffectSsDust_Draw)) {
            continue;
        }
        height = EffectSsDust_ScreenHeight(play, effect);
        if (height <= 0.0f) {
            continue;
        }
        for (at = count; (at > 0) && (heights[at - 1] > height); at--) {
            heights[at] = heights[at - 1];
            areas[at] = areas[at - 1];
        }
        heights[at] = height;
        areas[at] = EffectSsDust_ScreenArea(height, EffectSsDust_Texture(effect));
        count++;
    }
    for (i = 0; i < count; i++) {
        total += areas[i];
        if (total > DUST_BUDGET_SCREENS) {
            target = (heights[i] > DUST_LIMIT_MIN) ? heights[i] : DUST_LIMIT_MIN;
            break;
        }
    }

    if (target < sDustLimit) {
        sDustLimit = (target > sDustLimit * DUST_LIMIT_FALL) ? target : sDustLimit * DUST_LIMIT_FALL;
    } else {
        sDustLimit = (target < sDustLimit * DUST_LIMIT_RISE) ? target : sDustLimit * DUST_LIMIT_RISE;
    }
    if (sDustLimit < gLusxDustLowestLimit) {
        gLusxDustLowestLimit = sDustLimit;
    }
}

// the puff's alpha: 0 for one not to draw
static s32 EffectSsDust_Alpha(PlayState* play, EffectSs* this) {
    f32 height;
    f32 area;
    f32 fadeFrom = sDustLimit * DUST_FADE_FROM;

    if (sDustLimitFrame != play->gameplayFrames) {
        sDustLimitFrame = play->gameplayFrames;
        EffectSsDust_UpdateLimit(play);
        fadeFrom = sDustLimit * DUST_FADE_FROM;
    }

    height = EffectSsDust_ScreenHeight(play, this);
    area = EffectSsDust_ScreenArea(height, EffectSsDust_Texture(this));
    if (height >= sDustLimit) {
        gLusxDustSkipped++;
        gLusxDustSkippedArea += area;
        return 0;
    }
    gLusxDustDrawn++;
    gLusxDustArea += area;
    if (height <= fadeFrom) {
        return 255;
    }
    return (s32)(255.0f * (sDustLimit - height) / (sDustLimit - fadeFrom));
}
#endif

void EffectSsDust_Draw(PlayState* play, u32 index, EffectSs* this) {
    GraphicsContext* gfxCtx = play->state.gfxCtx;
    MtxF mfTrans;
    MtxF mfScale;
    MtxF mfResult;
    MtxF mfTransBillboard;
    s32 pad;
    Mtx* mtx;
    f32 scale;
    s32 alpha = 255;

#ifdef LUS_XBOX
    alpha = EffectSsDust_Alpha(play, this);
    if (alpha == 0) {
        return;
    }
#endif

    OPEN_DISPS(gfxCtx);

    scale = this->rScale * 0.0025f;
    SkinMatrix_SetTranslate(&mfTrans, this->pos.x, this->pos.y, this->pos.z);
    SkinMatrix_SetScale(&mfScale, scale, scale, 1.0f);
    SkinMatrix_MtxFMtxFMult(&mfTrans, &play->billboardMtxF, &mfTransBillboard);
    SkinMatrix_MtxFMtxFMult(&mfTransBillboard, &mfScale, &mfResult);
    gSPMatrix(POLY_XLU_DISP++, &gIdentityMtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

    mtx = SkinMatrix_MtxFToNewMtx(gfxCtx, &mfResult);

    if (mtx != NULL) {
        gSPMatrix(POLY_XLU_DISP++, mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gDPPipeSync(POLY_XLU_DISP++);
        gSPSegment(POLY_XLU_DISP++, 0x08, Lib_SegmentedToVirtual(sDustTextures[this->rTexIndex]));
        POLY_XLU_DISP = Gfx_SetupDL(POLY_XLU_DISP, SETUPDL_0);
        gDPPipeSync(POLY_XLU_DISP++);

        if (this->rDrawFlags & DUST_DRAWFLAG1) {
            gDPSetCombineLERP(POLY_XLU_DISP++, PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, PRIMITIVE, 0, TEXEL0, 0,
                              COMBINED, 0, SHADE, 0, 0, 0, 0, COMBINED);
            gDPSetRenderMode(POLY_XLU_DISP++, G_RM_FOG_SHADE_A, G_RM_ZB_CLD_SURF2);
            gSPSetGeometryMode(POLY_XLU_DISP++, G_FOG | G_LIGHTING);
        } else if (this->rDrawFlags & DUST_DRAWFLAG2) {
            gDPSetRenderMode(POLY_XLU_DISP++, G_RM_PASS, G_RM_ZB_CLD_SURF2);
            gSPClearGeometryMode(POLY_XLU_DISP++, G_FOG | G_LIGHTING);
        } else { // DUST_DRAWFLAG0
            gSPClearGeometryMode(POLY_XLU_DISP++, G_LIGHTING);
        }

        gDPPipeSync(POLY_XLU_DISP++);
        gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, this->rPrimColorR, this->rPrimColorG, this->rPrimColorB, alpha);
        gDPSetEnvColor(POLY_XLU_DISP++, this->rEnvColorR, this->rEnvColorG, this->rEnvColorB, this->rEnvColorA);
#ifdef LUS_XBOX
        // gEffDustDL, but for its quad
        gSPTexture(POLY_XLU_DISP++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
        gDPLoadTextureBlock(POLY_XLU_DISP++, sDustTextures[EffectSsDust_Texture(this)], G_IM_FMT_I, G_IM_SIZ_8b, 32, 32,
                            0, G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP, 5, 5, G_TX_NOLOD, G_TX_NOLOD);
        gSPVertex(POLY_XLU_DISP++, sDustOutlines[EffectSsDust_Texture(this)], 8, 0);
        gSP2Triangles(POLY_XLU_DISP++, 0, 1, 2, 0, 0, 2, 3, 0);
        gSP2Triangles(POLY_XLU_DISP++, 0, 3, 4, 0, 0, 4, 5, 0);
        gSP2Triangles(POLY_XLU_DISP++, 0, 5, 6, 0, 0, 6, 7, 0);
#else
        gSPDisplayList(POLY_XLU_DISP++, this->gfx);
#endif
    }

    CLOSE_DISPS(gfxCtx);
}

void EffectSsDust_Update(PlayState* play, u32 index, EffectSs* this) {
    this->accel.x = (Rand_ZeroOne() * 0.4f) - 0.2f;
    this->accel.z = (Rand_ZeroOne() * 0.4f) - 0.2f;

    if ((this->life <= this->rLifespan) && (this->life >= (this->rLifespan - 7))) {
        if (this->rLifespan >= 5) {
            this->rTexIndex = this->rLifespan - this->life;
        } else {
            this->rTexIndex = ((this->rLifespan - this->life) * (ARRAY_COUNT(sDustTextures) / this->rLifespan));
        }
    } else {
        this->rTexIndex = ARRAY_COUNT(sDustTextures) - 1;
    }

    this->rScale += this->rScaleStep;
}

// this update mode is unused in the original game
void EffectSsDust_UpdateFire(PlayState* play, u32 index, EffectSs* this) {
    this->accel.x = (Rand_ZeroOne() * 0.4f) - 0.2f;
    this->accel.z = (Rand_ZeroOne() * 0.4f) - 0.2f;

    switch (this->rTexIndex) {
        case 0:
            this->rPrimColorR = 255;
            this->rPrimColorG = 150;
            this->rPrimColorB = 0;
            this->rEnvColorR = 150;
            this->rEnvColorG = 50;
            this->rEnvColorB = 0;
            break;
        case 1:
            this->rPrimColorR = 200;
            this->rPrimColorG = 50;
            this->rPrimColorB = 0;
            this->rEnvColorR = 100;
            this->rEnvColorG = 0;
            this->rEnvColorB = 0;
            break;
        case 2:
            this->rPrimColorR = 50;
            this->rPrimColorG = 0;
            this->rPrimColorB = 0;
            this->rEnvColorR = 0;
            this->rEnvColorG = 0;
            this->rEnvColorB = 0;
            break;
        case 3:
            this->rPrimColorR = 50;
            this->rEnvColorR = this->rPrimColorG = this->rEnvColorG = this->rPrimColorB = this->rEnvColorB = 0;
            break;
    }

    if (this->rTexIndex < (ARRAY_COUNT(sDustTextures) - 1)) {
        this->rTexIndex++;
    }

    this->rScale += this->rScaleStep;
}
