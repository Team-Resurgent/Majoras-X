// lusx_save_edit.cpp -- the port's menu's Edit Save page: the save being played, by name. Each change is made the way
// 2Ship's PC save editor makes it (2s2h/DeveloperTools/SaveEditor.cpp, left out of this build with the GUI); the game
// is stopped while the menu is open, so nothing else is touching any of it.
#include "lusx_options.h"

#include <stdio.h>

#include <libultraship/bridge/consolevariablebridge.h>

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
extern u8 gItemSlots[77];
void Interface_NewDay(PlayState* play, s32 day);
void Interface_LoadItemIconImpl(PlayState* play, u8 btn);
void ObjTokeiStep_SetupOpen(ObjTokeiStep* objTokeiStep);
void ObjTokeiStep_DrawOpen(Actor* actor, PlayState* play);
void ObjTokeiStep_DoNothing(ObjTokeiStep* objTokeiStep, PlayState* play);
void EnTest4_GetBellTimeOnDay3(EnTest4* thisx);
void EnTest4_GetBellTimeAndShrinkScreenBeforeDay3(EnTest4* thisx, PlayState* play);
s32 BetterMapSelect_Count(void);
const char* BetterMapSelect_Name(s32 scene);
s32 BetterMapSelect_Spawns(s32 scene);
void BetterMapSelect_Warp(s32 scene, s32 spawn, s32 form, s32 day, s32 stage);
}

#define PLAYER_DATA gSaveContext.save.saveInfo.playerData
#define ITEMS gSaveContext.save.saveInfo.inventory.items
#define ROW(label, arg, step, text) \
    { OPT_CUSTOM, label, NULL, 0, NULL, NULL, 0, -1, NULL, arg, step, text }
#define ACTION(label, step) \
    { OPT_ACTION, label, NULL, 0, NULL, NULL, 0, -1, NULL, 0, step, Select }
#define LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

static int Clamp(int v, int lowest, int highest) {
    return v < lowest ? lowest : v > highest ? highest : v;
}

static int Wrap(int v, int lowest, int highest) {
    return v < lowest ? highest : v > highest ? lowest : v;
}

static const char* Number(char* buffer, size_t size, int n) {
    snprintf(buffer, size, "%d", n);
    return buffer;
}

static const char* Select(int, char*, size_t) {
    return "Press A";
}

bool LusxEdit_Available(void) {
    return gPlayState != NULL && gSaveContext.gameMode == GAMEMODE_NORMAL && gSaveContext.fileNum != 0xFF;
}

// ---- General

static EnTest4* FindClock(void) {
    for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_SWITCH].first; a != NULL; a = a->next) {
        if (a->id == ACTOR_EN_TEST4) {
            return (EnTest4*)a;
        }
    }
    return NULL;
}

// as SaveEditor.cpp's UpdateGameTime: the music, the half-day actors and the clock actor kept with the new time
static void SetTime(u16 gameTime) {
    const bool night = gameTime > CLOCK_TIME(18, 0) || gameTime < CLOCK_TIME(6, 0);
    const bool wasNight = CURRENT_TIME > CLOCK_TIME(18, 0) || CURRENT_TIME < CLOCK_TIME(6, 0);

    gSaveContext.save.time = gameTime;
    gWeatherMode = WEATHER_MODE_CLEAR;
    gPlayState->envCtx.lightningState = LIGHTNING_OFF;
    if (night != wasNight) {
        if (gPlayState->sceneSequences.ambienceId != AMBIENCE_ID_13) {
            SEQCMD_STOP_SEQUENCE(SEQ_PLAYER_AMBIENCE, 0);
            SEQCMD_STOP_SEQUENCE(SEQ_PLAYER_BGM_MAIN, 240);
            gSaveContext.seqId = NA_BGM_DISABLED;
            gSaveContext.ambienceId = AMBIENCE_ID_DISABLED;
            Environment_PlaySceneSequence(gPlayState);
        }
        gPlayState->numSetupActors = -gPlayState->numSetupActors;
    }
    EnTest4* clock = FindClock();
    if (clock != NULL) {
        clock->prevTime = gameTime;
        clock->prevBellTime = gameTime;
        clock->daytimeIndex = night ? 0 : 1;
        if (CURRENT_DAY == 3) {
            EnTest4_GetBellTimeOnDay3(clock);
        } else {
            EnTest4_GetBellTimeAndShrinkScreenBeforeDay3(clock, gPlayState);
        }
        gSaveContext.screenScale = 1000.0f;
        gSaveContext.screenScaleFlag = false;
    }
    // the Clock Tower's rooftop opens at midnight on the last night
    if (CURRENT_DAY == 3 && CURRENT_TIME < CLOCK_TIME(6, 0)) {
        ObjTokeiStep* steps = (ObjTokeiStep*)Actor_FindNearby(gPlayState, &GET_PLAYER(gPlayState)->actor,
                                                              ACTOR_OBJ_TOKEI_STEP, ACTORCAT_BG, 99999.9f);
        if (steps != NULL && steps->actionFunc == ObjTokeiStep_DoNothing) {
            steps->dyna.actor.draw = ObjTokeiStep_DrawOpen;
            ObjTokeiStep_SetupOpen(steps);
        }
    }
}

static void StepDay(int, int direction) {
    gSaveContext.save.day = Wrap(CURRENT_DAY + direction, 1, 3);
    gSaveContext.save.eventDayCount = CURRENT_DAY;
    Interface_NewDay(gPlayState, CURRENT_DAY);
    gPlayState->numSetupActors = -gPlayState->numSetupActors; // the half-day actors, for the new day
    Environment_NewDay(&gPlayState->envCtx);
    gWeatherMode = WEATHER_MODE_CLEAR;
    gPlayState->envCtx.lightningState = LIGHTNING_OFF;
}

static const char* TextDay(int, char* buffer, size_t size) {
    return Number(buffer, size, CURRENT_DAY);
}

static void StepTime(int minutes, int direction) {
    SetTime((u16)(CURRENT_TIME + CLOCK_TIME(0, minutes) * direction));
}

static const char* TextTime(int, char* buffer, size_t size) {
    const int minutes = (int)TIME_TO_MINUTES_F(CURRENT_TIME);
    const int hour = minutes / 60;

    if (CVarGetInteger("gEnhancements.Graphics.24HoursClock", 0)) {
        snprintf(buffer, size, "%02d:%02d", hour, minutes % 60);
    } else {
        snprintf(buffer, size, "%d:%02d %s", hour % 12 ? hour % 12 : 12, minutes % 60, hour >= 12 ? "pm" : "am");
    }
    return buffer;
}

static void StepDawn(int, int) {
    SetTime(CLOCK_TIME(6, 0));
}

static void StepTimeSpeed(int, int direction) {
    gSaveContext.save.timeSpeedOffset = Clamp(gSaveContext.save.timeSpeedOffset + direction, -3, 7);
}

static const char* TextTimeSpeed(int, char* buffer, size_t size) {
    const int speed = gSaveContext.save.timeSpeedOffset;

    if (speed == -3) {
        return "Stopped";
    }
    snprintf(buffer, size, "%d%s", speed + 3, speed == 0 ? " (normal)" : "");
    return buffer;
}

static void StepRupees(int, int direction) {
    PLAYER_DATA.rupees = Clamp(PLAYER_DATA.rupees + direction * 50, 0, CUR_CAPACITY(UPG_WALLET));
}

static const char* TextRupees(int, char* buffer, size_t size) {
    return Number(buffer, size, PLAYER_DATA.rupees);
}

static void StepWallet(int, int direction) {
    Inventory_ChangeUpgrade(UPG_WALLET, Clamp(CUR_UPG_VALUE(UPG_WALLET) + direction, 0, 2));
    PLAYER_DATA.rupees = Clamp(PLAYER_DATA.rupees, 0, CUR_CAPACITY(UPG_WALLET));
}

static const char* TextWallet(int, char*, size_t) {
    static const char* const kNames[] = { "Child's", "Adult's", "Giant's", "Tycoon's" };
    return kNames[CUR_UPG_VALUE(UPG_WALLET) & 3];
}

static void StepMaxHearts(int, int direction) {
    PLAYER_DATA.healthCapacity = Clamp(PLAYER_DATA.healthCapacity / 16 + direction, 3, 20) * 16;
    PLAYER_DATA.health = Clamp(PLAYER_DATA.health, 0, PLAYER_DATA.healthCapacity);
}

static const char* TextMaxHearts(int, char* buffer, size_t size) {
    return Number(buffer, size, PLAYER_DATA.healthCapacity / 16);
}

static void StepHealth(int, int direction) {
    PLAYER_DATA.health = Clamp(PLAYER_DATA.health + direction * 16, 16, PLAYER_DATA.healthCapacity);
}

static const char* TextHealth(int, char* buffer, size_t size) {
    return Number(buffer, size, PLAYER_DATA.health / 16);
}

static void StepDefense(int, int) {
    PLAYER_DATA.doubleDefense = !PLAYER_DATA.doubleDefense;
    gSaveContext.save.saveInfo.inventory.defenseHearts = PLAYER_DATA.doubleDefense ? 20 : 0;
}

static const char* TextDefense(int, char*, size_t) {
    return PLAYER_DATA.doubleDefense ? "On" : "Off";
}

static void StepMagicLevel(int, int direction) {
    const int level = Clamp(PLAYER_DATA.magicLevel + direction, 0, 2);

    PLAYER_DATA.magicLevel = level;
    gSaveContext.magicCapacity = level * MAGIC_NORMAL_METER;
    PLAYER_DATA.magic = Clamp(PLAYER_DATA.magic, 0, gSaveContext.magicCapacity);
    PLAYER_DATA.isMagicAcquired = level >= 1;
    PLAYER_DATA.isDoubleMagicAcquired = level >= 2;
    if (level >= 1) {
        BUTTON_ITEM_EQUIP(PLAYER_FORM_DEKU, EQUIP_SLOT_B) = ITEM_DEKU_NUT;
    }
}

static const char* TextMagicLevel(int, char*, size_t) {
    static const char* const kNames[] = { "None", "Single", "Double" };
    return kNames[Clamp(PLAYER_DATA.magicLevel, 0, 2)];
}

static void StepMagic(int, int direction) {
    PLAYER_DATA.magic = Clamp(PLAYER_DATA.magic + direction * 12, 0, gSaveContext.magicCapacity);
}

static const char* TextMagic(int, char* buffer, size_t size) {
    return Number(buffer, size, PLAYER_DATA.magic);
}

static const Option kGeneral[] = {
    ROW("Day", 0, StepDay, TextDay),
    ROW("Time (1 hour steps)", 60, StepTime, TextTime),
    ROW("Time (15 minute steps)", 15, StepTime, TextTime),
    ACTION("Jump to dawn", StepDawn),
    ROW("Time speed", 0, StepTimeSpeed, TextTimeSpeed),
    ROW("Rupees", 0, StepRupees, TextRupees),
    ROW("Wallet", 0, StepWallet, TextWallet),
    ROW("Max hearts", 0, StepMaxHearts, TextMaxHearts),
    ROW("Hearts", 0, StepHealth, TextHealth),
    ROW("Double defense", 0, StepDefense, TextDefense),
    ROW("Magic meter size", 0, StepMagicLevel, TextMagicLevel),
    ROW("Magic", 0, StepMagic, TextMagic),
};

// ---- Items and masks

static const char* ItemName(int item) {
    switch (item) {
        case ITEM_NONE: return "None";
        case ITEM_OCARINA_OF_TIME: return "Ocarina of Time";
        case ITEM_BOW: return "Yes";
        case ITEM_ARROW_FIRE: case ITEM_ARROW_ICE: case ITEM_ARROW_LIGHT: return "Yes";
        case ITEM_OCARINA_FAIRY: return "Fairy Ocarina";
        case ITEM_BOMB: case ITEM_BOMBCHU: case ITEM_DEKU_STICK: case ITEM_DEKU_NUT: return "Yes";
        case ITEM_MAGIC_BEANS: case ITEM_POWDER_KEG: case ITEM_PICTOGRAPH_BOX: return "Yes";
        case ITEM_LENS_OF_TRUTH: case ITEM_HOOKSHOT: case ITEM_SWORD_GREAT_FAIRY: return "Yes";
        case ITEM_BOTTLE: return "Empty";
        case ITEM_POTION_RED: return "Red Potion";
        case ITEM_POTION_GREEN: return "Green Potion";
        case ITEM_POTION_BLUE: return "Blue Potion";
        case ITEM_FAIRY: return "Fairy";
        case ITEM_DEKU_PRINCESS: return "Deku Princess";
        case ITEM_MILK_BOTTLE: return "Milk";
        case ITEM_MILK_HALF: return "Half Milk";
        case ITEM_FISH: return "Fish";
        case ITEM_BUG: return "Bugs";
        case ITEM_BLUE_FIRE: return "Blue Fire";
        case ITEM_POE: return "Poe";
        case ITEM_BIG_POE: return "Big Poe";
        case ITEM_SPRING_WATER: return "Spring Water";
        case ITEM_HOT_SPRING_WATER: return "Hot Spring Water";
        case ITEM_ZORA_EGG: return "Zora Egg";
        case ITEM_GOLD_DUST: return "Gold Dust";
        case ITEM_MUSHROOM: return "Magic Mushroom";
        case ITEM_SEAHORSE: return "Seahorse";
        case ITEM_CHATEAU: return "Chateau Romani";
        case ITEM_HYLIAN_LOACH: return "Hylian Loach";
        case ITEM_OBABA_DRINK: return "Kotake's Drink";
        case ITEM_MOONS_TEAR: return "Moon's Tear";
        case ITEM_DEED_LAND: return "Land Title Deed";
        case ITEM_DEED_SWAMP: return "Swamp Title Deed";
        case ITEM_DEED_MOUNTAIN: return "Mountain Title Deed";
        case ITEM_DEED_OCEAN: return "Ocean Title Deed";
        case ITEM_ROOM_KEY: return "Room Key";
        case ITEM_LETTER_MAMA: return "Special Delivery to Mama";
        case ITEM_LETTER_TO_KAFEI: return "Letter to Kafei";
        case ITEM_PENDANT_OF_MEMORIES: return "Pendant of Memories";
        case ITEM_TINGLE_MAP: return "Tingle's Map";
        default: return "Yes"; // a mask
    }
}

// what a slot can hold, nothing first: as SaveEditor.cpp's safe items
static int SlotItems(int slot, u8* items) {
    const bool bottle = slot >= SLOT_BOTTLE_1 && slot <= SLOT_BOTTLE_6;
    const int wanted = bottle ? SLOT_BOTTLE_1 : slot;
    int n = 0;

    items[n++] = ITEM_NONE;
    for (int i = 0; i < LEN(gItemSlots); i++) {
        if (gItemSlots[i] != wanted || (bottle && i == ITEM_LONGSHOT) || (slot == SLOT_BOW && i != ITEM_BOW) ||
            (slot == SLOT_TRADE_KEY_MAMA && i == ITEM_SLINGSHOT) || (slot == SLOT_TRADE_DEED && i == ITEM_OCARINA_FAIRY)) {
            continue;
        }
        items[n++] = (u8)i;
    }
    return n;
}

static int AmmoLimit(int item) {
    int limit;

    switch (item) {
        case ITEM_BOW:
            limit = CUR_CAPACITY(UPG_QUIVER);
            break;
        case ITEM_BOMB:
        case ITEM_BOMBCHU:
            limit = CUR_CAPACITY(UPG_BOMB_BAG);
            break;
        case ITEM_DEKU_STICK:
            limit = CUR_CAPACITY(UPG_DEKU_STICKS);
            limit = limit > 0 ? limit : 10;
            break;
        case ITEM_DEKU_NUT:
            limit = CUR_CAPACITY(UPG_DEKU_NUTS);
            limit = limit > 0 ? limit : 20;
            break;
        case ITEM_MAGIC_BEANS:
            limit = 20;
            break;
        case ITEM_POWDER_KEG:
            limit = 1;
            break;
        default:
            limit = 0;
            break;
    }
    return Clamp(limit, 0, 99); // over that the game's counters go wrong
}

// an item that's counted, full, on being given; the bag or quiver it needs first
static void Fill(int item) {
    if (item == ITEM_BOW && CUR_UPG_VALUE(UPG_QUIVER) == 0) {
        Inventory_ChangeUpgrade(UPG_QUIVER, 1);
    } else if ((item == ITEM_BOMB || item == ITEM_BOMBCHU) && CUR_UPG_VALUE(UPG_BOMB_BAG) == 0) {
        Inventory_ChangeUpgrade(UPG_BOMB_BAG, 1);
    }
    if (AmmoLimit(item) > 0) {
        AMMO(item) = AmmoLimit(item);
    }
}

static void StepSlot(int slot, int direction) {
    u8 items[32];
    const int n = SlotItems(slot, items);
    const int current = ITEMS[slot];
    int at = 0;

    for (int i = 0; i < n; i++) {
        if (items[i] == current) {
            at = i;
        }
    }
    const int next = items[(at + direction + n) % n];
    if (next == current) {
        return;
    }
    if (next == ITEM_NONE) {
        if (AmmoLimit(current) > 0) {
            AMMO(current) = 0;
        }
        Inventory_DeleteItem(current, slot);
    } else if (current == ITEM_NONE) {
        ITEMS[slot] = next;
        Fill(next);
    } else {
        // (not Inventory_ReplaceItem: it finds the item, and with two empty bottles changes the wrong one)
        ITEMS[slot] = next;
        for (int button = EQUIP_SLOT_C_LEFT; button <= EQUIP_SLOT_C_RIGHT; button++) {
            if (GET_CUR_FORM_BTN_SLOT(button) == slot) {
                SET_CUR_FORM_BTN_ITEM(button, next);
                Interface_LoadItemIconImpl(gPlayState, button);
            }
        }
    }
}

static const char* TextSlot(int slot, char*, size_t) {
    return ItemName(ITEMS[slot]);
}

static void StepMask(int slot, int) {
    StepSlot(slot, 1);
}

static const char* TextMask(int slot, char*, size_t) {
    return ITEMS[slot] != ITEM_NONE ? "Yes" : "No";
}

static void StepAmmo(int item, int direction) {
    const int by = AmmoLimit(item) >= 20 ? 5 : 1;

    AMMO(item) = Clamp(AMMO(item) + direction * by, 0, AmmoLimit(item));
}

static const char* TextAmmo(int item, char* buffer, size_t size) {
    return Number(buffer, size, AMMO(item));
}

static void StepBag(int upgrade, int direction) {
    Inventory_ChangeUpgrade(upgrade, Clamp(CUR_UPG_VALUE(upgrade) + direction, 0, 3));
    if (upgrade == UPG_QUIVER) {
        AMMO(ITEM_BOW) = Clamp(AMMO(ITEM_BOW), 0, CUR_CAPACITY(UPG_QUIVER));
    } else {
        AMMO(ITEM_BOMB) = Clamp(AMMO(ITEM_BOMB), 0, CUR_CAPACITY(UPG_BOMB_BAG));
        AMMO(ITEM_BOMBCHU) = Clamp(AMMO(ITEM_BOMBCHU), 0, CUR_CAPACITY(UPG_BOMB_BAG));
    }
}

static const char* TextBag(int upgrade, char* buffer, size_t size) {
    if (CUR_UPG_VALUE(upgrade) == 0) {
        return "None";
    }
    return Number(buffer, size, CUR_CAPACITY(upgrade));
}

static void GiveSlots(int first, int last) {
    u8 items[32];

    for (int slot = first; slot <= last; slot++) {
        if (ITEMS[slot] == ITEM_NONE) {
            const bool bottle = slot >= SLOT_BOTTLE_1 && slot <= SLOT_BOTTLE_6;
            ITEMS[slot] = bottle ? ITEM_BOTTLE : items[SlotItems(slot, items) - 1];
        }
    }
}

static void GiveAllItems(int, int) {
    GiveSlots(SLOT_OCARINA, SLOT_BOTTLE_6);
    Inventory_ChangeUpgrade(UPG_BOMB_BAG, 3);
    Inventory_ChangeUpgrade(UPG_QUIVER, 3);
    for (int item : { ITEM_BOW, ITEM_BOMB, ITEM_BOMBCHU, ITEM_DEKU_STICK, ITEM_DEKU_NUT, ITEM_MAGIC_BEANS,
                      ITEM_POWDER_KEG }) {
        Fill(item);
    }
}

static void GiveAllMasks(int, int) {
    GiveSlots(SLOT_MASK_POSTMAN, SLOT_MASK_FIERCE_DEITY);
}

static const Option kItems[] = {
    ACTION("Give all items", GiveAllItems),
    ROW("Ocarina", SLOT_OCARINA, StepSlot, TextSlot),
    ROW("Bow", SLOT_BOW, StepSlot, TextSlot),
    ROW("Quiver size", UPG_QUIVER, StepBag, TextBag),
    ROW("Arrows", ITEM_BOW, StepAmmo, TextAmmo),
    ROW("Fire Arrow", SLOT_ARROW_FIRE, StepSlot, TextSlot),
    ROW("Ice Arrow", SLOT_ARROW_ICE, StepSlot, TextSlot),
    ROW("Light Arrow", SLOT_ARROW_LIGHT, StepSlot, TextSlot),
    ROW("Bombs", SLOT_BOMB, StepSlot, TextSlot),
    ROW("Bombchus", SLOT_BOMBCHU, StepSlot, TextSlot),
    ROW("Bomb Bag size", UPG_BOMB_BAG, StepBag, TextBag),
    ROW("Bomb count", ITEM_BOMB, StepAmmo, TextAmmo),
    ROW("Bombchu count", ITEM_BOMBCHU, StepAmmo, TextAmmo),
    ROW("Deku Sticks", SLOT_DEKU_STICK, StepSlot, TextSlot),
    ROW("Stick count", ITEM_DEKU_STICK, StepAmmo, TextAmmo),
    ROW("Deku Nuts", SLOT_DEKU_NUT, StepSlot, TextSlot),
    ROW("Nut count", ITEM_DEKU_NUT, StepAmmo, TextAmmo),
    ROW("Magic Beans", SLOT_MAGIC_BEANS, StepSlot, TextSlot),
    ROW("Bean count", ITEM_MAGIC_BEANS, StepAmmo, TextAmmo),
    ROW("Powder Keg", SLOT_POWDER_KEG, StepSlot, TextSlot),
    ROW("Pictograph Box", SLOT_PICTOGRAPH_BOX, StepSlot, TextSlot),
    ROW("Lens of Truth", SLOT_LENS_OF_TRUTH, StepSlot, TextSlot),
    ROW("Hookshot", SLOT_HOOKSHOT, StepSlot, TextSlot),
    ROW("Great Fairy's Sword", SLOT_SWORD_GREAT_FAIRY, StepSlot, TextSlot),
    ROW("Trade item 1", SLOT_TRADE_DEED, StepSlot, TextSlot),
    ROW("Trade item 2", SLOT_TRADE_KEY_MAMA, StepSlot, TextSlot),
    ROW("Trade item 3", SLOT_TRADE_COUPLE, StepSlot, TextSlot),
    ROW("Bottle 1", SLOT_BOTTLE_1, StepSlot, TextSlot),
    ROW("Bottle 2", SLOT_BOTTLE_2, StepSlot, TextSlot),
    ROW("Bottle 3", SLOT_BOTTLE_3, StepSlot, TextSlot),
    ROW("Bottle 4", SLOT_BOTTLE_4, StepSlot, TextSlot),
    ROW("Bottle 5", SLOT_BOTTLE_5, StepSlot, TextSlot),
    ROW("Bottle 6", SLOT_BOTTLE_6, StepSlot, TextSlot),
};

static const Option kMasks[] = {
    ACTION("Give all masks", GiveAllMasks),
    ROW("Deku Mask", SLOT_MASK_DEKU, StepMask, TextMask),
    ROW("Goron Mask", SLOT_MASK_GORON, StepMask, TextMask),
    ROW("Zora Mask", SLOT_MASK_ZORA, StepMask, TextMask),
    ROW("Fierce Deity's Mask", SLOT_MASK_FIERCE_DEITY, StepMask, TextMask),
    ROW("Postman's Hat", SLOT_MASK_POSTMAN, StepMask, TextMask),
    ROW("All-Night Mask", SLOT_MASK_ALL_NIGHT, StepMask, TextMask),
    ROW("Blast Mask", SLOT_MASK_BLAST, StepMask, TextMask),
    ROW("Stone Mask", SLOT_MASK_STONE, StepMask, TextMask),
    ROW("Great Fairy's Mask", SLOT_MASK_GREAT_FAIRY, StepMask, TextMask),
    ROW("Keaton Mask", SLOT_MASK_KEATON, StepMask, TextMask),
    ROW("Bremen Mask", SLOT_MASK_BREMEN, StepMask, TextMask),
    ROW("Bunny Hood", SLOT_MASK_BUNNY, StepMask, TextMask),
    ROW("Don Gero's Mask", SLOT_MASK_DON_GERO, StepMask, TextMask),
    ROW("Mask of Scents", SLOT_MASK_SCENTS, StepMask, TextMask),
    ROW("Romani's Mask", SLOT_MASK_ROMANI, StepMask, TextMask),
    ROW("Circus Leader's Mask", SLOT_MASK_CIRCUS_LEADER, StepMask, TextMask),
    ROW("Kafei's Mask", SLOT_MASK_KAFEIS_MASK, StepMask, TextMask),
    ROW("Couple's Mask", SLOT_MASK_COUPLE, StepMask, TextMask),
    ROW("Mask of Truth", SLOT_MASK_TRUTH, StepMask, TextMask),
    ROW("Kamaro's Mask", SLOT_MASK_KAMARO, StepMask, TextMask),
    ROW("Gibdo Mask", SLOT_MASK_GIBDO, StepMask, TextMask),
    ROW("Garo's Mask", SLOT_MASK_GARO, StepMask, TextMask),
    ROW("Captain's Hat", SLOT_MASK_CAPTAIN, StepMask, TextMask),
    ROW("Giant's Mask", SLOT_MASK_GIANT, StepMask, TextMask),
};

// ---- Quest

static void StepQuest(int item, int) {
    if (CHECK_QUEST_ITEM(item)) {
        REMOVE_QUEST_ITEM(item);
    } else {
        SET_QUEST_ITEM(item);
    }
}

static const char* TextQuest(int item, char*, size_t) {
    return CHECK_QUEST_ITEM(item) ? "Yes" : "No";
}

// the Goron Lullaby comes in two halves: none, the intro, the song
static void StepLullaby(int, int direction) {
    const int now = CHECK_QUEST_ITEM(QUEST_SONG_LULLABY) ? 2 : CHECK_QUEST_ITEM(QUEST_SONG_LULLABY_INTRO) ? 1 : 0;
    const int next = (now + direction + 3) % 3;

    REMOVE_QUEST_ITEM(QUEST_SONG_LULLABY);
    REMOVE_QUEST_ITEM(QUEST_SONG_LULLABY_INTRO);
    if (next >= 1) {
        SET_QUEST_ITEM(QUEST_SONG_LULLABY_INTRO);
    }
    if (next == 2) {
        SET_QUEST_ITEM(QUEST_SONG_LULLABY);
    }
}

static const char* TextLullaby(int, char*, size_t) {
    return CHECK_QUEST_ITEM(QUEST_SONG_LULLABY) ? "Full song" : CHECK_QUEST_ITEM(QUEST_SONG_LULLABY_INTRO) ? "Intro" : "No";
}

// the sword the save has, on the human's B button
static void SetSword(int value) {
    SET_EQUIP_VALUE(EQUIP_TYPE_SWORD, value);
    BUTTON_ITEM_EQUIP(0, EQUIP_SLOT_B) =
        value == EQUIP_VALUE_SWORD_NONE ? ITEM_NONE : ITEM_SWORD_KOKIRI + value - EQUIP_VALUE_SWORD_KOKIRI;
    if (value == EQUIP_VALUE_SWORD_RAZOR) {
        PLAYER_DATA.swordHealth = 100;
    }
    Interface_LoadItemIconImpl(gPlayState, EQUIP_SLOT_B);
}

static void StepSword(int, int direction) {
    if (GET_PLAYER_FORM != PLAYER_FORM_FIERCE_DEITY) { // its sword isn't the save's
        SetSword(Wrap(GET_CUR_EQUIP_VALUE(EQUIP_TYPE_SWORD) + direction, EQUIP_VALUE_SWORD_NONE, EQUIP_VALUE_SWORD_GILDED));
    }
}

static const char* TextSword(int, char*, size_t) {
    switch (GET_CUR_EQUIP_VALUE(EQUIP_TYPE_SWORD)) {
        case EQUIP_VALUE_SWORD_NONE: return "None";
        case EQUIP_VALUE_SWORD_KOKIRI: return "Kokiri Sword";
        case EQUIP_VALUE_SWORD_RAZOR: return "Razor Sword";
        default: return "Gilded Sword";
    }
}

static void StepShield(int, int direction) {
    SET_EQUIP_VALUE(EQUIP_TYPE_SHIELD,
                    Wrap(GET_CUR_EQUIP_VALUE(EQUIP_TYPE_SHIELD) + direction, EQUIP_VALUE_SHIELD_NONE, EQUIP_VALUE_SHIELD_MIRROR));
    Player_SetEquipmentData(gPlayState, GET_PLAYER(gPlayState));
}

static const char* TextShield(int, char*, size_t) {
    switch (GET_CUR_EQUIP_VALUE(EQUIP_TYPE_SHIELD)) {
        case EQUIP_VALUE_SHIELD_NONE: return "None";
        case EQUIP_VALUE_SHIELD_HERO: return "Hero's Shield";
        default: return "Mirror Shield";
    }
}

static void StepHeartPieces(int, int direction) {
    const u32 count = Wrap((int)GET_QUEST_HEART_PIECE_COUNT + direction, 0, 3);

    gSaveContext.save.saveInfo.inventory.questItems &= ~0xF0000000;
    gSaveContext.save.saveInfo.inventory.questItems |= count << QUEST_HEART_PIECE_COUNT;
}

static const char* TextHeartPieces(int, char* buffer, size_t size) {
    return Number(buffer, size, (int)GET_QUEST_HEART_PIECE_COUNT);
}

// the count's halves: the swamp's the high one
static void StepTokens(int swamp, int direction) {
    u32 tokens = gSaveContext.save.saveInfo.skullTokenCount;

    if (swamp) {
        const u32 count = Clamp((int)(tokens >> 16) + direction, 0, 30);
        tokens = (count << 16) | (tokens & 0xFFFF);
    } else {
        const u32 count = Clamp((int)(tokens & 0xFFFF) + direction, 0, 30);
        tokens = (tokens & 0xFFFF0000) | count;
    }
    gSaveContext.save.saveInfo.skullTokenCount = tokens;
}

static const char* TextTokens(int swamp, char* buffer, size_t size) {
    const u32 tokens = gSaveContext.save.saveInfo.skullTokenCount;

    return Number(buffer, size, swamp ? (int)(tokens >> 16) : (int)(tokens & 0xFFFF));
}

static void GiveAllQuest(int, int) {
    for (int i = QUEST_REMAINS_ODOLWA; i <= QUEST_BOMBERS_NOTEBOOK; i++) {
        if (i != QUEST_SHIELD && i != QUEST_SWORD) {
            SET_QUEST_ITEM(i);
        }
    }
    SET_QUEST_ITEM(QUEST_SONG_LULLABY_INTRO);
    SET_EQUIP_VALUE(EQUIP_TYPE_SHIELD, EQUIP_VALUE_SHIELD_MIRROR);
    if (GET_PLAYER_FORM != PLAYER_FORM_FIERCE_DEITY) {
        SetSword(EQUIP_VALUE_SWORD_GILDED);
    }
    Player_SetEquipmentData(gPlayState, GET_PLAYER(gPlayState));
}

static const Option kQuest[] = {
    ACTION("Give all quest items", GiveAllQuest),
    ROW("Sword", 0, StepSword, TextSword),
    ROW("Shield", 0, StepShield, TextShield),
    ROW("Odolwa's Remains", QUEST_REMAINS_ODOLWA, StepQuest, TextQuest),
    ROW("Goht's Remains", QUEST_REMAINS_GOHT, StepQuest, TextQuest),
    ROW("Gyorg's Remains", QUEST_REMAINS_GYORG, StepQuest, TextQuest),
    ROW("Twinmold's Remains", QUEST_REMAINS_TWINMOLD, StepQuest, TextQuest),
    ROW("Song of Time", QUEST_SONG_TIME, StepQuest, TextQuest),
    ROW("Song of Healing", QUEST_SONG_HEALING, StepQuest, TextQuest),
    ROW("Epona's Song", QUEST_SONG_EPONA, StepQuest, TextQuest),
    ROW("Song of Soaring", QUEST_SONG_SOARING, StepQuest, TextQuest),
    ROW("Song of Storms", QUEST_SONG_STORMS, StepQuest, TextQuest),
    ROW("Sonata of Awakening", QUEST_SONG_SONATA, StepQuest, TextQuest),
    ROW("Goron Lullaby", 0, StepLullaby, TextLullaby),
    ROW("New Wave Bossa Nova", QUEST_SONG_BOSSA_NOVA, StepQuest, TextQuest),
    ROW("Elegy of Emptiness", QUEST_SONG_ELEGY, StepQuest, TextQuest),
    ROW("Oath to Order", QUEST_SONG_OATH, StepQuest, TextQuest),
    ROW("Saria's Song (unused)", QUEST_SONG_SARIA, StepQuest, TextQuest),
    ROW("Sun's Song (unused)", QUEST_SONG_SUN, StepQuest, TextQuest),
    ROW("Bombers' Notebook", QUEST_BOMBERS_NOTEBOOK, StepQuest, TextQuest),
    ROW("Heart pieces", 0, StepHeartPieces, TextHeartPieces),
    ROW("Swamp Skulltula tokens", 1, StepTokens, TextTokens),
    ROW("Ocean Skulltula tokens", 0, StepTokens, TextTokens),
};

// ---- Dungeons: `arg` is the dungeon, and for the dungeon items the item too

#define DUNGEON_ARG(dungeon, item) ((dungeon) | ((item) << 4))

static void StepDungeonItem(int arg, int) {
    const int dungeon = arg & 15, item = arg >> 4;

    if (CHECK_DUNGEON_ITEM(item, dungeon)) {
        REMOVE_DUNGEON_ITEM(item, dungeon);
    } else {
        SET_DUNGEON_ITEM(item, dungeon);
    }
}

static const char* TextDungeonItem(int arg, char*, size_t) {
    return CHECK_DUNGEON_ITEM(arg >> 4, arg & 15) ? "Yes" : "No";
}

static void StepKeys(int dungeon, int direction) {
    static const int kMost[4] = { 1, 3, 1, 4 };

    // (-1: none got yet, what the game starts with)
    gSaveContext.save.saveInfo.inventory.dungeonKeys[dungeon] =
        Clamp(gSaveContext.save.saveInfo.inventory.dungeonKeys[dungeon] + direction, -1, kMost[dungeon]);
}

static const char* TextKeys(int dungeon, char* buffer, size_t size) {
    const int keys = gSaveContext.save.saveInfo.inventory.dungeonKeys[dungeon];

    return keys < 0 ? "None yet" : Number(buffer, size, keys);
}

static void StepFairies(int dungeon, int direction) {
    gSaveContext.save.saveInfo.inventory.strayFairies[dungeon] =
        Clamp(gSaveContext.save.saveInfo.inventory.strayFairies[dungeon] + direction, 0, 15);
}

static const char* TextFairies(int dungeon, char* buffer, size_t size) {
    return Number(buffer, size, gSaveContext.save.saveInfo.inventory.strayFairies[dungeon]);
}

#define DUNGEON_ROWS(name, dungeon)                                                         \
    ROW(name " map", DUNGEON_ARG(dungeon, DUNGEON_MAP), StepDungeonItem, TextDungeonItem),  \
    ROW(name " compass", DUNGEON_ARG(dungeon, DUNGEON_COMPASS), StepDungeonItem, TextDungeonItem), \
    ROW(name " boss key", DUNGEON_ARG(dungeon, DUNGEON_BOSS_KEY), StepDungeonItem, TextDungeonItem), \
    ROW(name " small keys", dungeon, StepKeys, TextKeys),                                   \
    ROW(name " stray fairies", dungeon, StepFairies, TextFairies)

static const Option kDungeons[] = {
    DUNGEON_ROWS("Woodfall", DUNGEON_SCENE_INDEX_WOODFALL_TEMPLE),
    DUNGEON_ROWS("Snowhead", DUNGEON_SCENE_INDEX_SNOWHEAD_TEMPLE),
    DUNGEON_ROWS("Great Bay", DUNGEON_SCENE_INDEX_GREAT_BAY_TEMPLE),
    DUNGEON_ROWS("Stone Tower", DUNGEON_SCENE_INDEX_STONE_TOWER_TEMPLE),
};

extern const OptionGroup kEditGroups[] = {
    { "General", kGeneral, LEN(kGeneral) },
    { "Items", kItems, LEN(kItems) },
    { "Masks", kMasks, LEN(kMasks) },
    { "Quest", kQuest, LEN(kQuest) },
    { "Dungeons", kDungeons, LEN(kDungeons) },
};
extern const int kEditGroupCount = LEN(kEditGroups);

// ---- Warp: 2Ship's Better Map Select, its list in its order, with the same choices

#define WARP_SETTINGS 3 // the rows before the scenes
#define WARP_SCENES 106 // BetterMapSelect.c's sBetterScenes
#define CVAR_WARP_SCENE "gDeveloperTools.BetterMapSelect.CurrentScene"
#define CVAR_WARP_ENTRANCE "gDeveloperTools.BetterMapSelect.Opt"

static Option sWarpRows[WARP_SETTINGS + WARP_SCENES];
static char sWarpLabels[WARP_SCENES][48];
static u8 sWarpEntrance[WARP_SCENES];
static int sWarpForm, sWarpDay, sWarpStage;
static int sWarpTo = -1; // the scene picked, for when the menu has closed

static void StepWarpForm(int, int direction) {
    sWarpForm = Wrap(sWarpForm + direction, PLAYER_FORM_FIERCE_DEITY, PLAYER_FORM_HUMAN);
}

static const char* TextWarpForm(int, char*, size_t) {
    static const char* const kNames[] = { "Deity", "Goron", "Zora", "Deku", "Child" };
    return kNames[sWarpForm];
}

static void StepWarpDay(int, int direction) {
    sWarpDay = Wrap(sWarpDay + direction, 1, 4);
}

static const char* TextWarpDay(int, char*, size_t) {
    static const char* const kNames[] = { "First Day", "Second Day", "Final Day", "Clear Day" };
    return kNames[sWarpDay - 1];
}

static void StepWarpStage(int, int direction) {
    sWarpStage = Wrap(sWarpStage + direction, 0, 14);
}

static const char* TextWarpStage(int, char* buffer, size_t size) {
    static const char* const kNames[] = { "Time as it is", "Morning", "Afternoon", "Night" };

    if (sWarpStage < 4) {
        return kNames[sWarpStage];
    }
    snprintf(buffer, size, "Cutscene %X", sWarpStage - 4);
    return buffer;
}

static void StepWarpEntrance(int scene, int direction) {
    sWarpEntrance[scene] = Wrap(sWarpEntrance[scene] + direction, 0, BetterMapSelect_Spawns(scene) - 1);
}

static const char* TextWarpEntrance(int scene, char* buffer, size_t size) {
    if (BetterMapSelect_Spawns(scene) <= 1) {
        return "";
    }
    snprintf(buffer, size, "Entrance %d", sWarpEntrance[scene]);
    return buffer;
}

static void PressWarp(int scene) {
    sWarpTo = scene;
    CVarSetInteger(CVAR_WARP_SCENE, scene);
    CVarSetInteger(CVAR_WARP_ENTRANCE, sWarpEntrance[scene]);
}

void LusxWarp_Open(const Option** list, int* count, int* cursor, int* pinned) {
    static const Option kSettings[WARP_SETTINGS] = {
        ROW("Form", 0, StepWarpForm, TextWarpForm),
        ROW("Day", 0, StepWarpDay, TextWarpDay),
        ROW("Time", 0, StepWarpStage, TextWarpStage),
    };
    const int scenes = BetterMapSelect_Count();
    const int last = Clamp(CVarGetInteger(CVAR_WARP_SCENE, 0), 0, scenes - 1);

    for (int i = 0; i < WARP_SETTINGS; i++) {
        sWarpRows[i] = kSettings[i];
    }
    for (int i = 0; i < scenes; i++) {
        Option& row = sWarpRows[WARP_SETTINGS + i];

        snprintf(sWarpLabels[i], sizeof(sWarpLabels[i]), "%d  %s", i, BetterMapSelect_Name(i));
        row.kind = OPT_CUSTOM;
        row.label = sWarpLabels[i];
        row.arg = i;
        row.step = StepWarpEntrance;
        row.text = TextWarpEntrance;
        row.press = PressWarp;
    }
    sWarpEntrance[last] = Clamp(CVarGetInteger(CVAR_WARP_ENTRANCE, 0), 0, BetterMapSelect_Spawns(last) - 1);
    sWarpForm = Clamp(GET_PLAYER_FORM, PLAYER_FORM_FIERCE_DEITY, PLAYER_FORM_HUMAN);
    sWarpDay = Clamp(CURRENT_DAY, 1, 4);
    sWarpStage = 0;
    *list = sWarpRows;
    *count = WARP_SETTINGS + scenes;
    *cursor = WARP_SETTINGS + last;
    *pinned = WARP_SETTINGS;
}

void LusxWarp_Run(void) {
    if (sWarpTo >= 0) {
        BetterMapSelect_Warp(sWarpTo, sWarpEntrance[sWarpTo], sWarpForm, sWarpDay, sWarpStage);
        sWarpTo = -1;
    }
}
