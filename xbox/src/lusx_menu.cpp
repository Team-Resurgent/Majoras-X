// lusx_menu.cpp -- the port's own menu, drawn over the game. Back, pressed and let go on its own, opens and closes it.
// After Diddy-Kong-Racing-X's dkr_menu.c.
//
// While it is open the game does not run at all: BenPort.cpp's XboxRunMenu sits inside the tick, replaying the tick's
// recording with the menu over it, and goes back to the game once the menu has closed. Nothing in the game can tell.
// The audio thread makes nothing meanwhile, so the music stops and takes up where it was.
//
// The controls are 2Ship's own mappings (port 1's, what its input editor changes on a PC): a row shows the Xbox inputs
// on an N64 button, and they're kept in the config file with the rest. The Cheats and Options pages are 2Ship's settings
// (kCheatGroups, kOptionGroups), saved once as the menu closes.
#include <memory>
#include <stdio.h>
#include <string>

#include <SDL2/SDL.h>
#include <spdlog/spdlog.h>

#include "ship/Context.h"
#include "ship/config/ConsoleVariable.h"
#include "ship/controller/controldeck/ControlDeck.h"
#include "ship/controller/controldevice/controller/Controller.h"
#include "ship/controller/controldevice/controller/ControllerButton.h"
#include "ship/controller/controldevice/controller/mapping/sdl/SDLAxisDirectionToButtonMapping.h"
#include "ship/controller/controldevice/controller/mapping/sdl/SDLButtonToButtonMapping.h"
#include "libultraship/libultra/controller.h"
#include "libultraship/bridge/consolevariablebridge.h"
#include "2s2h/ShipInit.hpp"

// after the C++ headers: the Xbox's xtl.h defines min and max (and ignores NOMINMAX)
#include <xtl.h>

extern "C" void AudioSeq_SetPortVolumeScale(uint8_t seqPlayerIndex, float volume);
extern "C" void HudEditor_SetWidescreen(bool widescreen); // 2Ship's HUD editor's Widescreen preset

#include "lusx_debug.h"
#include "lusx_log.h"
#include "lusx_menu.h"
#include "lusx_options.h"
#include "lusx_overlay.h"

// the layout, in lusx_overlay.h's 960x720
#define PANEL_X0 150.0f
#define PANEL_X1 810.0f
#define PANEL_Y0 60.0f
#define PANEL_Y1 676.0f
#define BORDER 4.0f
#define RADIUS 30.0f
#define HEADER_Y1 154.0f
#define TITLE_Y 107.0f
#define ROWS_Y 208.0f
#define ROW_STEP 58.0f
#define ROW_HALF 24.0f
#define LIST_Y 192.0f
#define LIST_STEP 46.0f
#define LIST_HALF 20.0f
#define LIST_ROWS 9 // of the controls page, on screen at once
#define ROW_X0 186.0f
#define ROW_X1 774.0f
#define LABEL_X 214.0f
#define VALUE_X 744.0f
#define FOOTER_Y 634.0f
#define BADGE_RADIUS 15.0f
#define ARROW_GAP 12.0f
#define ARROW_SIZE 9.0f
#define CENTRE_X ((PANEL_X0 + PANEL_X1) * 0.5f)

// colours, 0xAARRGGBB
#define C_DIM 0xA0000010u
#define C_SHADOW 0x70000000u
#define C_GOLD_TOP 0xFFFFDA55u
#define C_GOLD_BOTTOM 0xFFE09A18u
#define C_PANEL_TOP 0xF5382A6Eu
#define C_PANEL_BOTTOM 0xF5140E2Cu
#define C_HEADER_TOP 0xFF6A4CC0u
#define C_HEADER_BOTTOM 0xFF3A2A80u
#define C_HEADER_MIDDLE 0xFF4A3896u
#define C_ROW 0x16FFFFFFu
#define C_CHOSEN_TOP 0xF07A5CE0u
#define C_CHOSEN_BOTTOM 0xF04A32A8u
#define C_TEXT_SHADOW 0x90000000u
#define C_TITLE 0xFFFFFFFFu
#define C_LABEL 0xFFE8E4FFu
#define C_VALUE 0xFFC8B8FFu
#define C_CHOSEN 0xFFFFFFFFu
#define C_CHOSEN_VALUE 0xFFFFE060u
#define C_HINT 0xFFC4BCEEu
#define C_BADGE_A 0xFF3DB24Au
#define C_BADGE_B 0xFFD8443Cu
#define C_BADGE_X 0xFF3C78D8u

#define REPEAT_AFTER 24 // frames a direction is held before it repeats
#define REPEAT_EVERY 6
#define SCROLL_HOLD 60    // frames a long label waits at each end
#define SCROLL_SPEED 1.5f // and the layout's pixels it moves a frame

static const unsigned kMenuLeft = LUSX_PAD_LEFT | LUSX_PAD_L_LEFT, kMenuRight = LUSX_PAD_RIGHT | LUSX_PAD_L_RIGHT;

enum Page { PAGE_MAIN, PAGE_CONTROLS, PAGE_CAPTURE, PAGE_GROUPS, PAGE_LIST, PAGE_DEBUG, PAGE_CONFIRM };

enum MainRow { ROW_RESUME, ROW_CONTROLS, ROW_CHEATS, ROW_OPTIONS, ROW_DEBUG, ROW_RESTART, ROW_EXIT, ROW_COUNT };

// the Debug page: the switches always shown first, then the tools Debug mode shows (Warp and Edit save), then Back
enum DebugRow {
    DEBUG_MODE,
    DEBUG_LOGGING, // lusx_log.h
#if LUSX_DEBUG_TOOLS
    DEBUG_OVERLAY,
#endif
    DEBUG_WARP, // the Warp page: an area and its entrance (port_nogui.cpp)
    DEBUG_EDIT,
    DEBUG_BACK,
    DEBUG_COUNT
};

// the N64's buttons, as the controls page lists them
struct N64Button {
    const char* name;
    CONTROLLERBUTTONS_T mask;
};
static const N64Button kButtons[] = {
    { "A", BTN_A },           { "B", BTN_B },           { "Z", BTN_Z },
    { "R", BTN_R },           { "L", BTN_L },           { "Start", BTN_START },
    { "C-Up", BTN_CUP },      { "C-Down", BTN_CDOWN },  { "C-Left", BTN_CLEFT },
    { "C-Right", BTN_CRIGHT }, { "D-Pad Up", BTN_DUP },  { "D-Pad Down", BTN_DDOWN },
    { "D-Pad Left", BTN_DLEFT }, { "D-Pad Right", BTN_DRIGHT },
};
enum {
    BUTTON_COUNT = sizeof(kButtons) / sizeof(kButtons[0]),
    LIST_RUMBLE = BUTTON_COUNT,
    LIST_RESET,
    LIST_BACK,
    LIST_COUNT
};

// the Xbox's inputs that can go on one. Not Back, which is the menu's, nor the left stick, which is the N64's stick.
struct XboxInput {
    unsigned bit;
    const char* name;
    int button;         // an SDL button, or
    int axis, direction; // an SDL axis' half
};
static const XboxInput kInputs[] = {
    { LUSX_PAD_A, "A", SDL_CONTROLLER_BUTTON_A, 0, 0 },
    { LUSX_PAD_B, "B", SDL_CONTROLLER_BUTTON_B, 0, 0 },
    { LUSX_PAD_X, "X", SDL_CONTROLLER_BUTTON_X, 0, 0 },
    { LUSX_PAD_Y, "Y", SDL_CONTROLLER_BUTTON_Y, 0, 0 },
    { LUSX_PAD_BLACK, "Black", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0, 0 },
    { LUSX_PAD_WHITE, "White", SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0, 0 },
    { LUSX_PAD_LT, "Left trigger", -1, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1 },
    { LUSX_PAD_RT, "Right trigger", -1, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1 },
    { LUSX_PAD_START, "Start", SDL_CONTROLLER_BUTTON_START, 0, 0 },
    { LUSX_PAD_LS, "Left click", SDL_CONTROLLER_BUTTON_LEFTSTICK, 0, 0 },
    { LUSX_PAD_RS, "Right click", SDL_CONTROLLER_BUTTON_RIGHTSTICK, 0, 0 },
    { LUSX_PAD_UP, "D-Up", SDL_CONTROLLER_BUTTON_DPAD_UP, 0, 0 },
    { LUSX_PAD_DOWN, "D-Down", SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0, 0 },
    { LUSX_PAD_LEFT, "D-Left", SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0, 0 },
    { LUSX_PAD_RIGHT, "D-Right", SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0, 0 },
    { LUSX_PAD_R_UP, "Stick Up", -1, SDL_CONTROLLER_AXIS_RIGHTY, -1 },
    { LUSX_PAD_R_DOWN, "Stick Down", -1, SDL_CONTROLLER_AXIS_RIGHTY, 1 },
    { LUSX_PAD_R_LEFT, "Stick Left", -1, SDL_CONTROLLER_AXIS_RIGHTX, -1 },
    { LUSX_PAD_R_RIGHT, "Stick Right", -1, SDL_CONTROLLER_AXIS_RIGHTX, 1 },
};
enum { INPUT_COUNT = sizeof(kInputs) / sizeof(kInputs[0]) };

// The Cheats and Options pages: rows over 2Ship's own settings, the ones its PC menu changes. A setting's hooks are
// made again when it changes (ShipInit::Init), as on a PC.
#define OPT_SWITCH(label, cvar) { OPT_ON_OFF, label, cvar, 0, NULL, NULL, 0, -1, NULL }
#define OPT_SWITCH_ON(label, cvar) { OPT_ON_OFF, label, cvar, 1, NULL, NULL, 0, -1, NULL }
#define OPT_NAMES(label, cvar, names) { OPT_LIST, label, cvar, 0, names, NULL, ARRAY_LEN(names), -1, NULL }
#define OPT_VALUES(label, cvar, fallback, names, values) \
    { OPT_LIST, label, cvar, fallback, names, values, ARRAY_LEN(names), -1, NULL }
#define OPT_VOLUME(label, cvar, player) { OPT_PERCENT, label, cvar, 10, NULL, NULL, 11, player, NULL }
#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

#define CVAR_SPEED "gLusx.LinkSpeed" // the row's own: 2Ship has a mode and a multiplier
#define CVAR_SPEED_MODE "gCheats.SpeedModifier.Mode"
#define CVAR_SPEED_VALUE "gCheats.SpeedModifier.Value"

static const char* const kSpeedNames[] = { "Normal", "1.5x", "2x", "3x" };
static const float kSpeeds[] = { 1.0f, 1.5f, 2.0f, 3.0f };
static const char* const kTimeStopNames[] = { "Off", "Temples", "Temples and mini dungeons" };
static const char* const kClockNames[] = { "Original", "MM3D style", "Text only" };
static const char* const kBlurNames[] = { "As the game has it", "Always off", "Always on" };
static const char* const kAutosaveNames[] = { "1 minute", "5 minutes", "10 minutes", "15 minutes", "30 minutes" };
static const int kAutosaveValues[] = { 1, 5, 10, 15, 30 };
static const char* const kFrameCapNames[] = { "Auto", "40", "30", "20" };
static const int kFrameCapValues[] = { 60, 40, 30, 20 };
static const char* const kRumbleNames[] = { "Full", "Medium", "Low", "Off" };
static const char* const kTransformNames[] = { "Off", "On", "After the first time" };
static const char* const kDoggyNames[] = { "Off", "With the Mask of Truth", "Always" };

// the speed modifier, always on at the chosen multiplier
static void AfterSpeed(void) {
    const int speed = CVarGetInteger(CVAR_SPEED, 0);

    CVarSetFloat(CVAR_SPEED_VALUE, kSpeeds[speed]);
    CVarSetInteger(CVAR_SPEED_MODE, speed != 0);
    ShipInit::Init(CVAR_SPEED_MODE);
}

#define CVAR_WIDE_HUD "gLusx.WidescreenHud" // the row's own: 2Ship keeps a mode for each piece of the HUD

// the HUD to the screen's edges (2Ship's Widescreen preset), or back to the game's 4:3 box
static void AfterWideHud(void) {
    HudEditor_SetWidescreen(CVarGetInteger(CVAR_WIDE_HUD, 0) != 0);
}

static const Option kCheatItems[] = {
    OPT_SWITCH("Infinite health", "gCheats.InfiniteHealth"),
    OPT_SWITCH("Infinite magic", "gCheats.InfiniteMagic"),
    OPT_SWITCH("Infinite rupees", "gCheats.InfiniteRupees"),
    OPT_SWITCH("Infinite consumables", "gCheats.InfiniteConsumables"),
    OPT_SWITCH("Infinite Epona carrots", "gCheats.InfiniteEponaCarrots"),
    OPT_SWITCH("Unrestricted items", "gCheats.UnrestrictedItems"),
    OPT_SWITCH("Unbreakable Razor Sword", "gCheats.UnbreakableRazorSword"),
    OPT_SWITCH("Bomb arrows", "gEnhancements.Equipment.BombArrows"),
    OPT_SWITCH("No explosive limit", "gEnhancements.Items.RemoveExplosiveLimit"),
    OPT_SWITCH("Hot water never cools", "gEnhancements.Items.HotWaterNeverCools"),
};

static const Option kCheatAbilities[] = {
    OPT_SWITCH("No clip", "gCheats.NoClip"),
    OPT_SWITCH("Moon jump (hold L)", "gCheats.MoonJumpOnL"),
    { OPT_LIST, "Link's speed", CVAR_SPEED, 0, kSpeedNames, NULL, ARRAY_LEN(kSpeedNames), -1, AfterSpeed },
    OPT_SWITCH("Hookshot anywhere", "gCheats.HookshotAnywhere"),
    OPT_SWITCH("Climb anywhere", "gCheats.ClimbAnywhere"),
    OPT_SWITCH("Elegy anywhere", "gCheats.ElegyAnywhere"),
    OPT_SWITCH("Fierce Deity anywhere", "gEnhancements.Masks.FierceDeitysAnywhere"),
    OPT_SWITCH("Longer flower glide", "gCheats.LongerFlowerGlide"),
    OPT_SWITCH("Infinite Deku hopping", "gEnhancements.Player.InfiniteDekuHopping"),
};

static const Option kCheatEvents[] = {
    OPT_NAMES("Stop time", "gCheats.TempleTimeStop", kTimeStopNames),
    OPT_NAMES("Win the Doggy Race", "gEnhancements.Minigames.AlwaysWinDoggyRace", kDoggyNames),
    OPT_SWITCH("Skip the horse race", "gEnhancements.Minigames.SkipHorseRace"),
    OPT_SWITCH("Skip the ranch invasion", "gEnhancements.Minigames.SkipRanchInvasion"),
};

// on the Debug page; logged as the cheats are
static const Option kDebugMode = OPT_SWITCH("Debug mode", "gDeveloperTools.DebugEnabled");

// on the Debug page, whatever Debug mode is: lusx_log.h. Off on a fresh install.
static const Option kLogging = { OPT_ON_OFF, "Logging", "gLusx.Logging", 0, NULL, NULL, 0, -1, LusxLog_Apply };

// the spdlog level that Logging's Off replaces, given back by On
static spdlog::level::level_enum sLogLevel = spdlog::level::info;

extern "C" void LusxLog_Apply(void) {
    static bool sSettled;
    const bool on = CVarGetInteger(kLogging.cvar, kLogging.fallback) != 0;
    const std::shared_ptr<spdlog::logger> logger = spdlog::default_logger();

    if (!sSettled) { // the first, as the config is read: the start-up's held lines go out, or are dropped
        sSettled = true;
        if (!on) {
            sLogLevel = logger->level();
            logger->set_level(spdlog::level::off);
        }
        LusxLog_Settle(on);
        return;
    }
    if (!on && gLusxLogOn) {
        SPDLOG_INFO("lus-x: logging off (port menu, Debug page)");
        sLogLevel = logger->level();
        logger->set_level(spdlog::level::off);
        LusxLog_Settle(0);
    } else if (on && !gLusxLogOn) {
        LusxLog_Settle(1);
        logger->set_level(sLogLevel);
        SPDLOG_INFO("lus-x: logging on (port menu, Debug page)");
    }
}

#if LUSX_DEBUG_TOOLS
// on the Debug page, whatever Debug mode is: the tester build's overlay, and the last run's report on it (lusx_debug.h).
// Saved, off on a fresh install.
static void ApplyOverlay(void) {
    LusxDebug_SetOverlay(CVarGetInteger("gLusx.TestOverlay", 0));
}
static const Option kTestOverlay = { OPT_ON_OFF, "Test overlay", "gLusx.TestOverlay", 0, NULL, NULL, 0, -1, ApplyOverlay };
#endif

static const Option kSaving[] = {
    OPT_SWITCH("Autosave", "gEnhancements.Saving.Autosave"),
    OPT_VALUES("Autosave every", "gEnhancements.Saving.AutosaveInterval", 5, kAutosaveNames, kAutosaveValues),
    OPT_SWITCH("Save from pause menu", "gEnhancements.Saving.PauseSave"),
    OPT_SWITCH("Persistent owl saves", "gEnhancements.Saving.PersistentOwlSaves"),
    OPT_SWITCH("Save on moon crash", "gEnhancements.Cycle.SaveOnMoonCrash"),
    OPT_SWITCH("New cycle keeps rupees", "gEnhancements.Cycle.DoNotResetRupees"),
    OPT_SWITCH("New cycle keeps consumables", "gEnhancements.Cycle.DoNotResetConsumables"),
    OPT_SWITCH("New cycle keeps bottles", "gEnhancements.Cycle.DoNotResetBottleContent"),
    OPT_SWITCH("New cycle keeps Razor Sword", "gEnhancements.Cycle.DoNotResetRazorSword"),
};

static const Option kSkips[] = {
    OPT_SWITCH("Skip to file select", "gEnhancements.Cutscenes.SkipToFileSelect"),
    OPT_SWITCH("Skip intro", "gEnhancements.Cutscenes.SkipIntroSequence"),
    OPT_SWITCH("Skip entrance cutscenes", "gEnhancements.Cutscenes.SkipEntranceCutscenes"),
    OPT_SWITCH("Hide title cards", "gEnhancements.Cutscenes.HideTitleCards"),
    OPT_SWITCH("Fast text", "gEnhancements.Dialogue.FastText"),
    OPT_SWITCH("Faster song playback", "gEnhancements.Songs.FasterSongPlayback"),
    OPT_SWITCH("Fast chests", "gEnhancements.Timesavers.FastChests"),
};

static const Option kGameplay[] = {
    OPT_NAMES("Fast mask transformation", "gEnhancements.Masks.FastTransformation", kTransformNames),
    OPT_SWITCH("Instant putaway", "gEnhancements.Player.InstantPutaway"),
    OPT_SWITCH("Fast flower launch", "gEnhancements.Player.FastFlowerLaunch"),
    OPT_SWITCH("D-pad item equips", "gEnhancements.Dpad.DpadEquips"),
    OPT_SWITCH("Arrow type cycling", "gEnhancements.PlayerActions.ArrowCycle"),
    OPT_SWITCH_ON("Invert aiming up and down", "gEnhancements.Camera.FirstPerson.InvertY"),
    OPT_SWITCH("Invert aiming left and right", "gEnhancements.Camera.FirstPerson.InvertX"),
};

static const Option kDisplay[] = {
    OPT_VALUES("Frame rate limit", "gLusx.FrameCap", 60, kFrameCapNames, kFrameCapValues),
    { OPT_ON_OFF, "Widescreen HUD", CVAR_WIDE_HUD, 0, NULL, NULL, 0, -1, AfterWideHud },
    // (libultraship's gfx_xbox.h: replays draw from vertex buffers; off for xemu, where they're slower)
    OPT_SWITCH_ON("GPU vertex buffers", "gLusx.VertexBuffers"),
    OPT_NAMES("Clock", "gEnhancements.Graphics.ClockType", kClockNames),
    OPT_SWITCH("24 hour clock", "gEnhancements.Graphics.24HoursClock"),
    OPT_SWITCH("Enemy health bars", "gEnhancements.Graphics.EnemyHealthBars"),
    OPT_SWITCH("Bow reticle", "gEnhancements.Graphics.BowReticle"),
    OPT_NAMES("Motion blur", "gEnhancements.Graphics.MotionBlur.Mode", kBlurNames),
    OPT_SWITCH("No flash on enemy kills", "gEnhancements.A11y.NoScreenFlashForEnemyKill"),
    OPT_SWITCH("No final day quakes", "gEnhancements.A11y.NoFinalDayQuakes"),
    OPT_SWITCH("No screen distortion", "gEnhancements.A11y.NoScreenDistortion"),
};

static const Option kSound[] = {
    OPT_VOLUME("Master volume", "gSettings.Audio.MasterVolume", -1),
    OPT_VOLUME("Music", "gSettings.Audio.MainMusicVolume", 0),
    OPT_VOLUME("Second music track", "gSettings.Audio.SubMusicVolume", 3),
    OPT_VOLUME("Sound effects", "gSettings.Audio.SoundEffectsVolume", 2),
    OPT_VOLUME("Fanfares", "gSettings.Audio.FanfareVolume", 1),
    OPT_VOLUME("Ambience", "gSettings.Audio.AmbienceVolume", 4),
};

// on the controls page
static const Option kRumble = OPT_NAMES("Rumble", "gLusx.Rumble", kRumbleNames);

static const OptionGroup kOptionGroups[] = {
    { "Saving", kSaving, ARRAY_LEN(kSaving) },       { "Skips", kSkips, ARRAY_LEN(kSkips) },
    { "Gameplay", kGameplay, ARRAY_LEN(kGameplay) }, { "Display", kDisplay, ARRAY_LEN(kDisplay) },
    { "Sound", kSound, ARRAY_LEN(kSound) },
};
static const OptionGroup kCheatGroups[] = {
    { "Items", kCheatItems, ARRAY_LEN(kCheatItems) },
    { "Abilities", kCheatAbilities, ARRAY_LEN(kCheatAbilities) },
    { "Events", kCheatEvents, ARRAY_LEN(kCheatEvents) },
};

static volatile int sOpen;       // drawn and holding the game
static volatile unsigned sPad;   // the pads, as last polled
static volatile int sWanted;     // Back was pressed and let go
static int sBackDown, sBackAlone;
static int sClosing;             // closed, but the buttons that closed it are not yet let go
static Page sPage;
static int sCursor, sTop;        // the chosen row; the controls page's first on screen
static int sMainCursor;          // what the main page goes back to
static bool sAdd;                // the input being asked for goes on beside the ones there
static int sConfirmRow;          // the main page's row being confirmed
static int sHeldFor;             // frames a direction has been held
static unsigned sHeld;           // last frame's buttons
static unsigned sPressed;        // every button pressed since the menu opened
static std::string sValues[BUTTON_COUNT]; // what each N64 button is on, for the controls page
static const Option* sList;      // the list page's rows, its title and how many, with a Back row after them
static const char* sListTitle;
static int sListCount;
static const OptionGroup* sGroups; // the Options or Cheats page: its rows, how many, with a Back row after, its title
static int sGroupCount;
static const char* sGroupsTitle;
static int sGroupCursor;         // what the list page goes back to there
static bool sListFromDebug;      // the list page was opened from the Debug page (Warp)
static int sDebugCursor;         // what the pages opened from the Debug page go back to
static int sPinned;              // the list page's first rows that stay put while the rest scroll (Warp's settings)
static int sListRow;             // the row of the rest the cursor was last on
static int sScrollFrames;        // frames the cursor has been on this row, for a long label
static bool sChanged;            // settings to save when the menu closes

static std::shared_ptr<Ship::Controller> Pad1(void) {
    return Ship::Context::GetRawInstance()->GetControlDeck()->GetControllerByPort(0);
}

static std::shared_ptr<Ship::ControllerButtonMapping> MappingFor(const XboxInput& in, CONTROLLERBUTTONS_T mask) {
    if (in.button >= 0) {
        return std::make_shared<Ship::SDLButtonToButtonMapping>(0, mask, in.button);
    }
    return std::make_shared<Ship::SDLAxisDirectionToButtonMapping>(0, mask, in.axis, in.direction);
}

// the rows' text, in kInputs' order
static void ReadMappings(void) {
    auto pad = Pad1();

    for (int b = 0; b < BUTTON_COUNT; b++) {
        auto button = pad->GetButton(kButtons[b].mask);
        std::string& value = sValues[b];

        value.clear();
        for (int i = 0; i < INPUT_COUNT; i++) {
            if (button->GetButtonMappingById(MappingFor(kInputs[i], kButtons[b].mask)->GetButtonMappingId()) != nullptr) {
                value += value.empty() ? "" : ", ";
                value += kInputs[i].name;
            }
        }
        if (value.empty()) {
            value = "None";
        }
    }
}

static void HaveConfig(void) {
    Ship::Context::GetRawInstance()->GetConsoleVariables()->SetInteger(CVAR_PREFIX_CONTROLLERS ".Port1.HasConfig", 1);
}

static void Remap(int row, const XboxInput& in) {
    auto cvars = Ship::Context::GetRawInstance()->GetConsoleVariables();
    auto button = Pad1()->GetButton(kButtons[row].mask);
    auto mapping = MappingFor(in, kButtons[row].mask);

    cvars->HoldSaves(true); // each step of this saves the whole file
    if (!sAdd) {
        button->ClearAllButtonMappingsForDeviceType(Ship::PhysicalDeviceType::SDLGamepad);
    }
    button->AddButtonMapping(mapping);
    mapping->SaveToConfig();
    button->SaveButtonMappingIdsToConfig();
    HaveConfig();
    cvars->HoldSaves(false);
    ReadMappings();
}

static void ResetMappings(void) {
    auto cvars = Ship::Context::GetRawInstance()->GetConsoleVariables();
    auto pad = Pad1();

    cvars->HoldSaves(true);
    for (int b = 0; b < BUTTON_COUNT; b++) {
        auto button = pad->GetButton(kButtons[b].mask);
        button->ClearAllButtonMappingsForDeviceType(Ship::PhysicalDeviceType::SDLGamepad);
        button->AddDefaultMappings(Ship::PhysicalDeviceType::SDLGamepad);
    }
    HaveConfig();
    cvars->HoldSaves(false);
    ReadMappings();
}

// a row's setting as an index into its values
static int OptionIndex(const Option& o) {
    if (o.kind == OPT_PERCENT) {
        const int tenths = (int)(CVarGetFloat(o.cvar, 1.0f) * 10.0f + 0.5f);
        return tenths < 0 ? 0 : tenths > 10 ? 10 : tenths;
    }
    const int value = CVarGetInteger(o.cvar, o.fallback);
    if (o.values != NULL) {
        for (int i = 0; i < o.count; i++) {
            if (o.values[i] == value) {
                return i;
            }
        }
        return 0;
    }
    return value;
}

// its value's text; `text` is for the ones that have to be made
static const char* OptionText(const Option& o, char* text, size_t size) {
    if (o.kind == OPT_CUSTOM || o.kind == OPT_ACTION) {
        return o.text(o.arg, text, size);
    }
    const int index = OptionIndex(o);

    if (o.kind == OPT_ON_OFF) {
        return index ? "On" : "Off";
    }
    if (o.kind == OPT_PERCENT) {
        snprintf(text, size, "%d%%", index * 10);
        return text;
    }
    return index >= 0 && index < o.count ? o.names[index] : "?";
}

static void ChangeOption(const Option& o, int step) {
    if (o.kind == OPT_CUSTOM || o.kind == OPT_ACTION) { // an edit of the save: a report from a run with them says so
        char mark[80], text[32];
        o.step(o.arg, step);
        snprintf(mark, sizeof(mark), "save edit: %s: %s", o.label, OptionText(o, text, sizeof(text)));
        LusxDebug_Mark(mark);
        return;
    }
    const int count = o.kind == OPT_ON_OFF ? 2 : o.count;
    const int index = (OptionIndex(o) + step + count) % count;

    if (o.kind == OPT_PERCENT) {
        CVarSetFloat(o.cvar, index / 10.0f);
        if (o.seqPlayer >= 0) {
            AudioSeq_SetPortVolumeScale((uint8_t)o.seqPlayer, index / 10.0f);
        }
    } else {
        CVarSetInteger(o.cvar, o.values != NULL ? o.values[index] : index);
    }
    if (o.after != NULL) {
        o.after();
    }
    ShipInit::Init(o.cvar);
    sChanged = true;
    if (&o == &kDebugMode || (sList != NULL && sGroups == kCheatGroups)) { // a report from a run with cheats on says so
        char mark[80], text[32];
        snprintf(mark, sizeof(mark), "cheat: %s: %s", o.label, OptionText(o, text, sizeof(text)));
        LusxDebug_Mark(mark);
    }
}

static void SaveSettings(void) {
    if (sChanged) {
        sChanged = false;
        CVarSave();
    }
}

// the log says what's on at the start of a run too: the settings outlive it
void LusxMenu_LogCheats(void) {
    char text[32];

#if LUSX_DEBUG_TOOLS
    ApplyOverlay(); // the saved Test overlay (and with it, whether the last run's report is shown)
#endif
    // an earlier build's row, gone now: not left on with no way to turn it off
    if (CVarGetInteger("gCheats.EasyFrameAdvance", 0) != 0) {
        CVarClear("gCheats.EasyFrameAdvance");
        ShipInit::Init("gCheats.EasyFrameAdvance");
    }
    if (OptionIndex(kDebugMode) != 0) {
        SPDLOG_INFO("lus-x: cheat on: {}", kDebugMode.label);
    }
    for (const OptionGroup& group : kCheatGroups) {
        for (int i = 0; i < group.count; i++) {
            const Option& o = group.list[i];
            if (OptionIndex(o) != 0) {
                SPDLOG_INFO("lus-x: cheat on: {}: {}", o.label, OptionText(o, text, sizeof(text)));
            }
        }
    }
}

// what the game is left for: an empty image is the dashboard
static void Leave(const char* image, const char* why) {
    SaveSettings();
    LusxDebug_Mark(why);
    LusxDebug_Stop();
    XLaunchNewImage(image, NULL);
    XLaunchNewImage(NULL, NULL);
}

extern "C" void LusxMenu_Pad(unsigned buttons) {
    sPad = buttons;
    if (sOpen) {
        return;
    }
    if (buttons & LUSX_PAD_BACK) {
        if (!sBackDown) {
            sBackDown = 1;
            sBackAlone = 1;
        }
        // the tester tools' lock test starts with Back too
        if (buttons & (LUSX_PAD_LS | LUSX_PAD_RS)) {
            sBackAlone = 0;
        }
    } else if (sBackDown) {
        sBackDown = 0;
        sWanted = sBackAlone;
    }
}

extern "C" int LusxMenu_Wanted(void) {
    const int wanted = sWanted;
    sWanted = 0;
    return wanted;
}

extern "C" int LusxMenu_IsOpen(void) {
    return sOpen;
}

bool LusxMenu_Open(Fast::GfxRenderingAPI* rapi) {
    if (!Ovl_Load(rapi)) {
        return false;
    }
    sOpen = 1;
    sClosing = 0;
    sPage = PAGE_MAIN;
    sCursor = sMainCursor = 0;
    sTop = 0;
    sHeld = sPad; // what's held already isn't a press
    sPressed = 0;
    sHeldFor = 0;
    LusXbox_PadsQuiet();
    ReadMappings();
    LusxDebug_Mark("menu: opened");
    return true;
}

extern "C" void LusxMenu_Close(void) {
    SaveSettings();
    for (std::string& value : sValues) {
        std::string().swap(value);
    }
    Ovl_Unload();
    sOpen = 0;
    LusxDebug_Mark("menu: closed");
    LusxWarp_Run();
}

static void GoMain(void) {
    sPage = PAGE_MAIN;
    sCursor = sMainCursor;
}

static void GoList(const Option* list, int count, const char* title) {
    sPage = PAGE_LIST;
    sList = list;
    sListCount = count;
    sListTitle = title;
    sListFromDebug = false;
    sPinned = 0;
    sCursor = sTop = sListRow = 0;
}

// back to the page a list was opened from
static void GoGroups(void) {
    sPage = PAGE_GROUPS;
    sCursor = sGroupCursor;
}

static void GoDebug(void) {
    sPage = PAGE_DEBUG;
    sCursor = sDebugCursor;
    sList = NULL; // (Debug mode's row isn't in a list)
}

static void LeaveList(void) {
    if (sListFromDebug) {
        GoDebug();
    } else {
        GoGroups();
    }
}

// the Options and Cheats pages go back to the main page, Edit save's to the Debug page
static void LeaveGroups(void) {
    if (sGroups == kEditGroups) {
        GoDebug();
    } else {
        GoMain();
    }
}

static void OpenGroups(const OptionGroup* groups, int count, const char* title) {
    sGroups = groups;
    sGroupCount = count;
    sGroupsTitle = title;
    sGroupCursor = 0;
    GoGroups();
}

// Warp and Edit save are debug tools: Debug mode shows them
static bool ShowDebugRows(void) {
    return CVarGetInteger(kDebugMode.cvar, 0) != 0;
}

static bool IsDebugTool(int row) {
    return row == DEBUG_WARP || row == DEBUG_EDIT;
}

static void UpdateDebug(unsigned pressed, int step) {
    if (step != 0) {
        do {
            sCursor = (sCursor + step + DEBUG_COUNT) % DEBUG_COUNT;
        } while (IsDebugTool(sCursor) && !ShowDebugRows());
    }
    if (pressed & LUSX_PAD_B) {
        GoMain();
        return;
    }
    if (sCursor == DEBUG_MODE) {
        if (pressed & (kMenuLeft | kMenuRight)) {
            ChangeOption(kDebugMode, 1);
        }
        return;
    }
    if (sCursor == DEBUG_LOGGING) {
        if (pressed & (kMenuLeft | kMenuRight)) {
            ChangeOption(kLogging, 1);
        }
        return;
    }
#if LUSX_DEBUG_TOOLS
    if (sCursor == DEBUG_OVERLAY) {
        if (pressed & (kMenuLeft | kMenuRight)) {
            ChangeOption(kTestOverlay, 1);
        }
        return;
    }
#endif
    if (!(pressed & LUSX_PAD_A)) {
        return;
    }
    sDebugCursor = sCursor;
    if (sCursor == DEBUG_WARP) {
        const Option* list;
        int count, cursor, pinned;

        LusxWarp_Open(&list, &count, &cursor, &pinned);
        GoList(list, count, "Warp");
        // on the settings, what's wanted first; the list ready on the last warp for X
        sPinned = pinned;
        sCursor = 0;
        sListRow = cursor;
        sTop = cursor - 2 > pinned ? cursor - 2 : pinned;
        sListFromDebug = true;
    } else if (sCursor == DEBUG_EDIT) {
        if (LusxEdit_Available()) {
            OpenGroups(kEditGroups, kEditGroupCount, "Edit save");
        }
    } else {
        GoMain();
    }
}

static void UpdateMain(unsigned pressed, int step) {
    sCursor = (sCursor + step + ROW_COUNT) % ROW_COUNT;
    if (pressed & LUSX_PAD_B) {
        sClosing = 1;
    }
    if (!(pressed & LUSX_PAD_A)) {
        return;
    }
    sMainCursor = sCursor;
    switch (sCursor) {
        case ROW_RESUME:
            sClosing = 1;
            break;
        case ROW_CONTROLS:
            sPage = PAGE_CONTROLS;
            sCursor = sTop = 0;
            sList = NULL; // (Rumble's row isn't a cheat, whatever list was last open)
            break;
        case ROW_CHEATS:
            OpenGroups(kCheatGroups, ARRAY_LEN(kCheatGroups), "Cheats");
            break;
        case ROW_OPTIONS:
            OpenGroups(kOptionGroups, ARRAY_LEN(kOptionGroups), "Options");
            break;
        case ROW_DEBUG:
            sDebugCursor = !ShowDebugRows() ? DEBUG_MODE : DEBUG_WARP;
            GoDebug();
            break;
        default:
            sPage = PAGE_CONFIRM;
            sConfirmRow = sCursor;
            sCursor = 0; // No: the safe answer first
            break;
    }
}

static void UpdateControls(unsigned pressed, int step) {
    sCursor = (sCursor + step + LIST_COUNT) % LIST_COUNT;
    sTop = sCursor < sTop ? sCursor : sCursor >= sTop + LIST_ROWS ? sCursor - LIST_ROWS + 1 : sTop;
    if (pressed & LUSX_PAD_B) {
        GoMain();
    } else if (sCursor == LIST_RUMBLE) {
        if (pressed & kMenuRight) {
            ChangeOption(kRumble, 1);
        } else if (pressed & kMenuLeft) {
            ChangeOption(kRumble, -1);
        }
    } else if (sCursor >= BUTTON_COUNT) {
        if (pressed & LUSX_PAD_A) {
            if (sCursor == LIST_RESET) {
                ResetMappings();
            } else {
                GoMain();
            }
        }
    } else if (pressed & (LUSX_PAD_A | LUSX_PAD_X)) {
        sAdd = (pressed & LUSX_PAD_A) == 0;
        sPage = PAGE_CAPTURE;
    }
}

static void UpdateGroups(unsigned pressed, int step) {
    sCursor = (sCursor + step + sGroupCount + 1) % (sGroupCount + 1);
    if (pressed & LUSX_PAD_B) {
        LeaveGroups();
    } else if (pressed & LUSX_PAD_A) {
        if (sCursor == sGroupCount) {
            LeaveGroups();
        } else {
            sGroupCursor = sCursor;
            GoList(sGroups[sCursor].list, sGroups[sCursor].count, sGroups[sCursor].title);
        }
    }
}

static void UpdateList(unsigned pressed, int step) {
    const int rows = sListCount + 1; // and Back
    const int shown = LIST_ROWS - sPinned; // the rows that scroll, at once

    if (sCursor < sPinned) {
        // the pinned rows and the rest are apart: up and down go round in each, and only X goes between them, back to
        // the row the list was left on
        if (pressed & LUSX_PAD_X) {
            sCursor = sListRow;
        } else {
            sCursor = (sCursor + step + sPinned) % sPinned;
        }
    } else {
        if (sPinned > 0 && (pressed & LUSX_PAD_X)) {
            sCursor = 0;
        } else {
            const int count = rows - sPinned;
            sCursor = sPinned + (sCursor - sPinned + step + count) % count;
        }
        // the triggers, a page at a time
        if (sCursor >= sPinned && (pressed & LUSX_PAD_LT)) {
            sCursor = sCursor - shown > sPinned ? sCursor - shown : sPinned;
        } else if (sCursor >= sPinned && (pressed & LUSX_PAD_RT)) {
            sCursor = sCursor + shown < rows ? sCursor + shown : rows - 1;
        }
    }
    if (sCursor >= sPinned) {
        sListRow = sCursor;
        sTop = sCursor < sTop ? sCursor : sCursor >= sTop + shown ? sCursor - shown + 1 : sTop;
    }
    if (pressed & LUSX_PAD_B) {
        LeaveList();
    } else if (sCursor == sListCount) {
        if (pressed & LUSX_PAD_A) {
            LeaveList();
        }
    } else if (sList[sCursor].press != NULL && (pressed & LUSX_PAD_A)) {
        char mark[80];

        snprintf(mark, sizeof(mark), "warp: %s", sList[sCursor].label);
        LusxDebug_Mark(mark);
        sList[sCursor].press(sList[sCursor].arg);
        sChanged = true;
        sClosing = 1;
    } else if (sList[sCursor].kind == OPT_ACTION) {
        if (pressed & LUSX_PAD_A) { // done once, on purpose
            ChangeOption(sList[sCursor], 1);
        }
    } else if (pressed & kMenuRight) {
        ChangeOption(sList[sCursor], 1);
    } else if (pressed & kMenuLeft) {
        ChangeOption(sList[sCursor], -1);
    }
}

static void UpdateCapture(unsigned pressed) {
    for (int i = 0; i < INPUT_COUNT; i++) {
        if (pressed & kInputs[i].bit) {
            Remap(sCursor, kInputs[i]);
            sPage = PAGE_CONTROLS;
            return;
        }
    }
}

static void UpdateConfirm(unsigned pressed, int step) {
    sCursor = (sCursor + step + 2) % 2;
    if (pressed & LUSX_PAD_B) {
        GoMain();
    } else if (pressed & LUSX_PAD_A) {
        if (sCursor == 0) {
            GoMain();
        } else if (sConfirmRow == ROW_RESTART) {
            Leave("D:\\default.xbe", "menu: restart");
        } else {
            Leave(NULL, "menu: to the dashboard");
        }
    }
}

extern "C" int LusxMenu_Update(void) {
    static const unsigned kUp = LUSX_PAD_UP | LUSX_PAD_L_UP, kDown = LUSX_PAD_DOWN | LUSX_PAD_L_DOWN;

    SDL_PumpEvents();
    const unsigned now = sPad, pressed = now & ~sHeld;
    sHeld = now;
    sPressed |= pressed;
    if (sClosing) {
        // back to the game only once the buttons pressed here are up: it would take one still held for a press of its
        // own. A button held from before the menu opened doesn't hold it up.
        return (now & sPressed) != 0;
    }
    // Back is the way out from anywhere, and all a capture can't take
    if (pressed & LUSX_PAD_BACK) {
        if (sPage == PAGE_CAPTURE) {
            sPage = PAGE_CONTROLS;
        } else {
            sClosing = 1;
        }
        return 1;
    }
    int step = (pressed & kDown) ? 1 : (pressed & kUp) ? -1 : 0;
    sHeldFor = (now & (kUp | kDown)) ? sHeldFor + 1 : 0;
    if (step == 0 && sHeldFor >= REPEAT_AFTER && (sHeldFor - REPEAT_AFTER) % REPEAT_EVERY == 0) {
        step = (now & kDown) ? 1 : -1;
    }
    switch (sPage) {
        case PAGE_MAIN:
            UpdateMain(pressed, step);
            break;
        case PAGE_CONTROLS:
            UpdateControls(pressed, step);
            break;
        case PAGE_CAPTURE:
            UpdateCapture(pressed);
            break;
        case PAGE_GROUPS:
            UpdateGroups(pressed, step);
            break;
        case PAGE_LIST:
            UpdateList(pressed, step);
            break;
        case PAGE_DEBUG:
            UpdateDebug(pressed, step);
            break;
        case PAGE_CONFIRM:
            UpdateConfirm(pressed, step);
            break;
    }
    return 1;
}

// text with a soft drop shadow under it
static void Text(int font, float x, float y, const char* s, uint32_t colour, int align) {
    Ovl_Text(font, x + 2.0f, y + 3.0f, s, C_TEXT_SHADOW, align);
    Ovl_Text(font, x, y, s, colour, align);
}

static void RowBack(float y, float half, bool chosen) {
    if (chosen) {
        Ovl_RoundRect(ROW_X0 - 3.0f, y - half - 3.0f, ROW_X1 + 3.0f, y + half + 3.0f, 17.0f, C_GOLD_TOP, C_GOLD_BOTTOM);
        Ovl_RoundRect(ROW_X0, y - half, ROW_X1, y + half, 14.0f, C_CHOSEN_TOP, C_CHOSEN_BOTTOM);
    } else {
        Ovl_RoundRect(ROW_X0, y - half, ROW_X1, y + half, 14.0f, C_ROW, C_ROW);
    }
}

// a row of one centred label
static void Row(float y, float half, const char* label, bool chosen) {
    RowBack(y, half, chosen);
    Text(OVL_FONT_BODY, CENTRE_X, y, label, chosen ? C_CHOSEN_VALUE : C_LABEL, OVL_ALIGN_CENTRE);
}

// how far a label `over` too long has scrolled: a second at the start, along it, a second at the end, and again
static float ScrollBy(float over) {
    const int hold = SCROLL_HOLD, along = (int)(over / SCROLL_SPEED) + 1;
    const int at = sScrollFrames % (hold + along + hold);

    return at < hold ? 0.0f : at < hold + along ? (at - hold) * SCROLL_SPEED : over;
}

// a row of a label and its value; `arrows`, chosen: left and right change it, and it says so
static void ValueRow(float y, float half, const char* label, const char* value, bool chosen, bool arrows = false) {
    // what the label has, clear of the value and its arrows
    const float room = VALUE_X - LABEL_X - Ovl_TextWidth(OVL_FONT_SMALL, value) - ARROW_GAP * 3.0f;
    const float over = Ovl_TextWidth(OVL_FONT_BODY, label) - room;

    RowBack(y, half, chosen);
    if (over > 0.0f) {
        // too long: cut off there, and on the chosen row, scrolled along to its end and back
        Ovl_Clip(LABEL_X, LABEL_X + room);
        Text(OVL_FONT_BODY, LABEL_X - (chosen ? ScrollBy(over) : 0.0f), y, label, chosen ? C_CHOSEN : C_LABEL,
             OVL_ALIGN_LEFT);
        Ovl_ClipOff();
    } else {
        Text(OVL_FONT_BODY, LABEL_X, y, label, chosen ? C_CHOSEN : C_LABEL, OVL_ALIGN_LEFT);
    }
    Text(OVL_FONT_SMALL, VALUE_X, y, value, chosen ? C_CHOSEN_VALUE : C_VALUE, OVL_ALIGN_RIGHT);
    if (chosen && arrows) {
        const float l = VALUE_X - Ovl_TextWidth(OVL_FONT_SMALL, value) - ARROW_GAP, r = VALUE_X + ARROW_GAP;

        Ovl_Tri(l, y - ARROW_SIZE, l, y + ARROW_SIZE, l - ARROW_SIZE * 1.2f, y, C_GOLD_TOP);
        Ovl_Tri(r, y - ARROW_SIZE, r + ARROW_SIZE * 1.2f, y, r, y + ARROW_SIZE, C_GOLD_TOP);
    }
}

// a pad button as the pad has it, and what it does: returns the width used
static float Hint(float x, const char* badge, uint32_t badgeColour, const char* what) {
    const float w = BADGE_RADIUS * 2.0f + 8.0f;

    Ovl_Circle(x + BADGE_RADIUS, FOOTER_Y, BADGE_RADIUS, badgeColour);
    Ovl_Text(OVL_FONT_SMALL, x + BADGE_RADIUS, FOOTER_Y, badge, 0xFFFFFFFFu, OVL_ALIGN_CENTRE);
    Ovl_Text(OVL_FONT_SMALL, x + w, FOOTER_Y, what, C_HINT, OVL_ALIGN_LEFT);
    return w + Ovl_TextWidth(OVL_FONT_SMALL, what);
}

static float HintWidth(const char* what) {
    return BADGE_RADIUS * 2.0f + 8.0f + Ovl_TextWidth(OVL_FONT_SMALL, what);
}

// for a row left and right change: two arrows, A and X if they do something too, and B
static void FooterChange(const char* what = "Change", const char* a = NULL, const char* xButton = NULL) {
    static const float kGap = 28.0f, kArrows = 34.0f;
    float x = CENTRE_X - (kArrows + Ovl_TextWidth(OVL_FONT_SMALL, what) + kGap + (a ? HintWidth(a) + kGap : 0.0f) +
                          (xButton ? HintWidth(xButton) + kGap : 0.0f) + HintWidth("Back")) * 0.5f;

    if (a != NULL) {
        x += Hint(x, "A", C_BADGE_A, a) + kGap;
    }
    Ovl_Tri(x + 10.0f, FOOTER_Y - 8.0f, x + 10.0f, FOOTER_Y + 8.0f, x, FOOTER_Y, C_HINT);
    Ovl_Tri(x + 16.0f, FOOTER_Y - 8.0f, x + 26.0f, FOOTER_Y, x + 16.0f, FOOTER_Y + 8.0f, C_HINT);
    Ovl_Text(OVL_FONT_SMALL, x + kArrows, FOOTER_Y, what, C_HINT, OVL_ALIGN_LEFT);
    x += kArrows + Ovl_TextWidth(OVL_FONT_SMALL, what) + kGap;
    if (xButton != NULL) {
        x += Hint(x, "X", C_BADGE_X, xButton) + kGap;
    }
    Hint(x, "B", C_BADGE_B, "Back");
}

static void Footer(const char* a, const char* x, const char* b) {
    static const float kGap = 34.0f;
    // measured first, to centre them
    float at = CENTRE_X - (HintWidth(a) + kGap + (x ? HintWidth(x) + kGap : 0.0f) + HintWidth(b)) * 0.5f;

    at += Hint(at, "A", C_BADGE_A, a) + kGap;
    if (x != NULL) {
        at += Hint(at, "X", C_BADGE_X, x) + kGap;
    }
    Hint(at, "B", C_BADGE_B, b);
}

static void Panel(const char* title) {
    // the game dimmed behind; the panel's shadow, gold rim and body
    Ovl_Rect(0.0f, 0.0f, OVL_W, OVL_H, C_DIM, C_DIM);
    Ovl_RoundRect(PANEL_X0 + 8.0f, PANEL_Y0 + 12.0f, PANEL_X1 + 8.0f, PANEL_Y1 + 12.0f, RADIUS, C_SHADOW, C_SHADOW);
    Ovl_RoundRect(PANEL_X0, PANEL_Y0, PANEL_X1, PANEL_Y1, RADIUS, C_GOLD_TOP, C_GOLD_BOTTOM);
    Ovl_RoundRect(PANEL_X0 + BORDER, PANEL_Y0 + BORDER, PANEL_X1 - BORDER, PANEL_Y1 - BORDER, RADIUS - BORDER,
                  C_PANEL_TOP, C_PANEL_BOTTOM);
    // the title's band: rounded at the top, square where it meets the body
    Ovl_RoundRect(PANEL_X0 + BORDER, PANEL_Y0 + BORDER, PANEL_X1 - BORDER, HEADER_Y1, RADIUS - BORDER, C_HEADER_TOP,
                  C_HEADER_BOTTOM);
    Ovl_Rect(PANEL_X0 + BORDER, HEADER_Y1 - RADIUS, PANEL_X1 - BORDER, HEADER_Y1, C_HEADER_MIDDLE, C_HEADER_BOTTOM);
    Ovl_Rect(PANEL_X0 + BORDER, HEADER_Y1, PANEL_X1 - BORDER, HEADER_Y1 + 3.0f, C_GOLD_TOP, C_GOLD_BOTTOM);
    Text(OVL_FONT_TITLE, CENTRE_X, TITLE_Y, title, C_TITLE, OVL_ALIGN_CENTRE);
}

static void DrawMain(void) {
    static const char* const kRows[ROW_COUNT] = { "Back to the game", "Controls",     "Cheats",           "Options",
                                                  "Debug",            "Restart game", "Exit to dashboard" };

    Panel("Majora's-X");
    for (int i = 0; i < ROW_COUNT; i++) {
        Row(ROWS_Y + i * ROW_STEP, ROW_HALF, kRows[i], sCursor == i);
    }
    Footer("Select", NULL, "Back");
}

// more of a list above or below; `first`, the first row that scrolls
static void MoreMarks(int rows, int first = 0) {
    if (sTop > first) {
        const float y = first > 0 ? LIST_Y + first * LIST_STEP - 20.0f : 168.0f;
        Ovl_Tri(CENTRE_X - 12.0f, y, CENTRE_X, y - 10.0f, CENTRE_X + 12.0f, y, C_GOLD_TOP);
    }
    if (sTop + LIST_ROWS - first < rows) {
        const float y = LIST_Y + LIST_ROWS * LIST_STEP - 20.0f;
        Ovl_Tri(CENTRE_X - 12.0f, y, CENTRE_X + 12.0f, y, CENTRE_X, y + 10.0f, C_GOLD_TOP);
    }
}

static void DrawDebug(void) {
    char text[32];
    float y = ROWS_Y;

    Panel("Debug");
    ValueRow(y, ROW_HALF, kDebugMode.label, OptionText(kDebugMode, text, sizeof(text)), sCursor == DEBUG_MODE, true);
    y += ROW_STEP;
    ValueRow(y, ROW_HALF, kLogging.label, OptionText(kLogging, text, sizeof(text)), sCursor == DEBUG_LOGGING, true);
    y += ROW_STEP;
#if LUSX_DEBUG_TOOLS
    ValueRow(y, ROW_HALF, kTestOverlay.label, OptionText(kTestOverlay, text, sizeof(text)), sCursor == DEBUG_OVERLAY,
             true);
    y += ROW_STEP;
#endif
    if (ShowDebugRows()) {
        Row(y, ROW_HALF, "Warp", sCursor == DEBUG_WARP);
        y += ROW_STEP;
        if (LusxEdit_Available()) {
            Row(y, ROW_HALF, "Edit save", sCursor == DEBUG_EDIT);
        } else {
            ValueRow(y, ROW_HALF, "Edit save", "No save loaded", sCursor == DEBUG_EDIT);
        }
        y += ROW_STEP;
    }
    Row(y, ROW_HALF, "Back", sCursor == DEBUG_BACK);
    if (sCursor == DEBUG_MODE || sCursor == DEBUG_LOGGING) {
        FooterChange();
#if LUSX_DEBUG_TOOLS
    } else if (sCursor == DEBUG_OVERLAY) {
        FooterChange();
#endif
    } else {
        Footer("Select", NULL, "Back");
    }
}

static void DrawGroups(void) {
    Panel(sGroupsTitle);
    for (int i = 0; i < sGroupCount; i++) {
        Row(ROWS_Y + i * ROW_STEP, ROW_HALF, sGroups[i].title, sCursor == i);
    }
    Row(ROWS_Y + sGroupCount * ROW_STEP, ROW_HALF, "Back", sCursor == sGroupCount);
    Footer("Select", NULL, "Back");
}

static void DrawList(void) {
    const int rows = sListCount + 1;
    char text[16];

    Panel(sListTitle);
    for (int i = 0; i < LIST_ROWS; i++) {
        // the pinned rows where they are, then the rest from the top of the scroll
        const int row = i < sPinned ? i : sTop + i - sPinned;
        const float y = LIST_Y + i * LIST_STEP;
        const bool chosen = row == sCursor;

        if (row >= rows) {
            break;
        }
        if (row == sListCount) {
            Row(y, LIST_HALF, "Back", chosen);
        } else {
            ValueRow(y, LIST_HALF, sList[row].label, OptionText(sList[row], text, sizeof(text)), chosen,
                     sList[row].kind != OPT_ACTION);
        }
    }
    if (sPinned > 0) {
        const float line = LIST_Y + (sPinned - 0.5f) * LIST_STEP;
        Ovl_Rect(ROW_X0, line - 1.0f, ROW_X1, line + 1.0f, C_GOLD_TOP, C_GOLD_BOTTOM);
    }
    MoreMarks(rows, sPinned);
    if (sCursor == sListCount || sList[sCursor].kind == OPT_ACTION) {
        Footer("Select", NULL, "Back");
    } else if (sList[sCursor].press != NULL) {
        FooterChange("Entrance", "Warp", "Settings");
    } else if (sCursor < sPinned) {
        FooterChange("Change", NULL, "Areas");
    } else {
        FooterChange();
    }
}

static void DrawControls(void) {
    char text[16];

    Panel("Controls");
    for (int i = 0; i < LIST_ROWS && sTop + i < LIST_COUNT; i++) {
        const int row = sTop + i;
        const float y = LIST_Y + i * LIST_STEP;
        const bool chosen = row == sCursor;

        if (row == LIST_RUMBLE) {
            ValueRow(y, LIST_HALF, kRumble.label, OptionText(kRumble, text, sizeof(text)), chosen, true);
        } else if (row == LIST_RESET) {
            Row(y, LIST_HALF, "Reset to defaults", chosen);
        } else if (row == LIST_BACK) {
            Row(y, LIST_HALF, "Back", chosen);
        } else if (chosen && sPage == PAGE_CAPTURE) {
            ValueRow(y, LIST_HALF, kButtons[row].name, "Press a button", true);
        } else {
            ValueRow(y, LIST_HALF, kButtons[row].name, sValues[row].c_str(), chosen);
        }
    }
    MoreMarks(LIST_COUNT);
    if (sPage == PAGE_CAPTURE) {
        Ovl_Text(OVL_FONT_SMALL, CENTRE_X, FOOTER_Y, sAdd ? "Press the button to add. Back cancels."
                                                          : "Press the new button. Back cancels.",
                 C_HINT, OVL_ALIGN_CENTRE);
    } else if (sCursor < BUTTON_COUNT) {
        Footer("Change", "Add another", "Back");
    } else if (sCursor == LIST_RUMBLE) {
        FooterChange();
    } else {
        Footer("Select", NULL, "Back");
    }
}

static void DrawConfirm(void) {
    const bool restart = sConfirmRow == ROW_RESTART;

    Panel(restart ? "Restart game?" : "Exit to dashboard?");
    Text(OVL_FONT_BODY, CENTRE_X, ROWS_Y + 20.0f, "Anything not saved will be lost.", C_LABEL, OVL_ALIGN_CENTRE);
    Row(ROWS_Y + ROW_STEP * 2.0f, ROW_HALF, "No, go back", sCursor == 0);
    Row(ROWS_Y + ROW_STEP * 3.0f, ROW_HALF, restart ? "Yes, restart" : "Yes, exit", sCursor == 1);
    Footer("Select", NULL, "Back");
}

void LusxMenu_Draw(Fast::GfxRenderingAPI* rapi) {
    static int sLastCursor;
    static Page sLastPage;

    if (sClosing) {
        return;
    }
    sScrollFrames = sCursor == sLastCursor && sPage == sLastPage ? sScrollFrames + 1 : 0;
    sLastCursor = sCursor;
    sLastPage = sPage;
    Ovl_Begin();
    switch (sPage) {
        case PAGE_MAIN:
            DrawMain();
            break;
        case PAGE_CONTROLS:
        case PAGE_CAPTURE:
            DrawControls();
            break;
        case PAGE_GROUPS:
            DrawGroups();
            break;
        case PAGE_LIST:
            DrawList();
            break;
        case PAGE_DEBUG:
            DrawDebug();
            break;
        case PAGE_CONFIRM:
            DrawConfirm();
            break;
    }
}
