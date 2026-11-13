// port_nogui.cpp -- 2Ship2Harkinian with no GUI: what the game calls in the files the Xbox build
// leaves out (tools/build.py's MM_EXCLUDE). Game logic that lives in a GUI file is copied here
// unchanged; GUI-only features answer "none".
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/Enhancements/Audio/AudioCollection.h"
#include <string>
#include <vector>

extern "C" {
#include <z64.h>
#include <z64save.h>
#include <macros.h>
#include <variables.h>
#include <functions.h>
#include "overlays/actors/ovl_En_Test4/z_en_test4.h"
#include "overlays/actors/ovl_Obj_Tokei_Step/z_obj_tokei_step.h"

extern PlayState* gPlayState;
extern SaveContext gSaveContext;
// as SaveEditor.cpp declares them
void ObjTokeiStep_SetupOpen(ObjTokeiStep* objTokeiStep);
void ObjTokeiStep_DrawOpen(Actor* actor, PlayState* play);
void ObjTokeiStep_DoNothing(ObjTokeiStep* objTokeiStep, PlayState* play);
void EnTest4_GetBellTimeOnDay3(EnTest4* thisx);
void EnTest4_GetBellTimeAndShrinkScreenBeforeDay3(EnTest4* thisx, PlayState* play);
}

// the layout libultraship's interpreter walks display lists by (tools/build.py: -mno-ms-bitfields)
static_assert(sizeof(Gfx) == 8, "Gfx must be the N64's 8 bytes on the Xbox's 32-bit pointers");

// Enhancements/Audio/AudioEditor.cpp: the sequence the audio editor's shuffle put in a sequence's place
extern "C" u16 AudioEditor_GetReplacementSeq(u16 seqId) {
    return AudioCollection::Instance->GetReplacementSequence(seqId);
}

extern "C" u16 AudioEditor_GetOriginalSeq(u16 seqId) {
    return AudioCollection::Instance->GetOriginalSequence(seqId);
}

// PresetManager/: no presets without the menu
std::vector<std::string> PresetManager_GetPresetNames() {
    return {};
}

bool PresetManager_ApplyPresetByName(const std::string& name) {
    return false;
}

// Rando/CheckTracker/: the tracker window
namespace Rando {
namespace CheckTracker {
void Init() {
}
void OnFileLoad() {
}
} // namespace CheckTracker
} // namespace Rando

// DeveloperTools/SaveEditor.cpp, unchanged: the Song of Double Time enhancement and the randomizer's
// traps move the clock with it
static EnTest4* FindEnTest4Actor() {
    if (gPlayState == NULL) {
        return NULL;
    }

    Actor* enTest4Search = gPlayState->actorCtx.actorLists[ACTORCAT_SWITCH].first;

    while (enTest4Search != NULL) {
        if (enTest4Search->id == ACTOR_EN_TEST4) {
            return (EnTest4*)enTest4Search;
        }
        enTest4Search = enTest4Search->next;
    }

    return NULL;
}

void UpdateGameTime(u16 gameTime) {
    bool newTimeIsNight = (gameTime > CLOCK_TIME(18, 0)) || (gameTime < CLOCK_TIME(6, 0));
    bool prevTimeIsNight = (CURRENT_TIME > CLOCK_TIME(18, 0)) || (CURRENT_TIME < CLOCK_TIME(6, 0));

    gSaveContext.save.time = gameTime;

    if (gPlayState == NULL) {
        return;
    }

    // Clear weather from day 2
    gWeatherMode = WEATHER_MODE_CLEAR;
    gPlayState->envCtx.lightningState = LIGHTNING_OFF;

    // When transitioning over night boundaries, stop the sequences and ask to replay, then respawn actors
    if (newTimeIsNight != prevTimeIsNight) {
        // AMBIENCE_ID_13 is used to persist a scenes sequence through night, so we shouldn't
        // change anything if thats active
        if (gPlayState->sceneSequences.ambienceId != AMBIENCE_ID_13) {
            SEQCMD_STOP_SEQUENCE(SEQ_PLAYER_AMBIENCE, 0);
            SEQCMD_STOP_SEQUENCE(SEQ_PLAYER_BGM_MAIN, 240);
            gSaveContext.seqId = NA_BGM_DISABLED;
            gSaveContext.ambienceId = AMBIENCE_ID_DISABLED;
            Environment_PlaySceneSequence(gPlayState);
        }

        // Kills/Spawns half-day actors
        gPlayState->numSetupActors = -gPlayState->numSetupActors;
    }

    EnTest4* enTest4 = FindEnTest4Actor();

    // Update EnTest4 actor to be in sync with the new time
    // This ensures that day transitions are not triggered with the change
    if (enTest4 != NULL) {
        enTest4->prevTime = gameTime;
        enTest4->prevBellTime = gameTime;
        enTest4->daytimeIndex = newTimeIsNight ? 0 : 1;

        // Sets the nextBellTime based on the new current time
        if (CURRENT_DAY == 3) {
            EnTest4_GetBellTimeOnDay3(enTest4);
        } else {
            EnTest4_GetBellTimeAndShrinkScreenBeforeDay3(enTest4, gPlayState);
        }

        // Unset any screen scaling from the above funcs
        gSaveContext.screenScale = 1000.0f;
        gSaveContext.screenScaleFlag = false;
    }

    // Open the Clock Tower rooftop
    if (((CURRENT_DAY == 3) && (CURRENT_TIME < CLOCK_TIME(6, 0)))) {
        ObjTokeiStep* objTokeiStep = (ObjTokeiStep*)Actor_FindNearby(gPlayState, &GET_PLAYER(gPlayState)->actor,
                                                                     ACTOR_OBJ_TOKEI_STEP, ACTORCAT_BG, 99999.9f);
        if (objTokeiStep != NULL && objTokeiStep->actionFunc == ObjTokeiStep_DoNothing) {
            objTokeiStep->dyna.actor.draw = ObjTokeiStep_DrawOpen;
            ObjTokeiStep_SetupOpen(objTokeiStep);
        }
    }
}
