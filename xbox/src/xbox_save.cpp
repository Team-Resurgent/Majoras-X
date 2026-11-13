// xbox_save.cpp -- where the game's saves live, as an original Xbox game keeps them: each of the three file slots is its
// own save game on the hard disk (U:), named after the slot and its player ("File 1 - Link"). The dashboard's Memory
// page lists them under "Majora's-X" with the save image the XBE carries (tools/build.py: /DEFAULTSAVEIMAGE), and
// copies or deletes each one on its own. A slot's save game holds that slot's 2Ship files: fileN.json (its new cycle
// save and its owl save) and fileNbackup.json (their backups). The game's settings aren't a save: they stay in T: with
// the port's (2ship2harkinian.json), as a game's options would, and so do the copies 2Ship keeps of a save it couldn't
// read (fileN_invalid_....json, in T:\saves).
//
// SaveManager.cpp asks here for a file's path (LusXbox_SavePath), for the path to write it to (LusXbox_SaveOpen: the
// slot's save game made on its first save and named after the player), and says when a file is gone
// (LusXbox_SaveGone: the save game goes with the slot's last file, when the file select erases the slot).
//
// A slot's save game is found by the files in it, not by its name: the name changes with the player's (rewritten in
// its SaveMeta.xbx), and the system finds a save game by name through a hash of the name it was made with (libxapi
// xsaveapi.c), so after a rename only the files still say which slot it is. So a save game is removed here file by
// file, not with XDeleteSaveGame.
//
// Before this, the saves were in T:\saves (2Ship's own place, and the Lus-X builds', which xbox_main.cpp copies over):
// they're moved into the slots' own save games at start.
#include <xtl.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "lusx_log.h"

#define OLD_DIR "T:\\saves" // the slots' files before; 2Ship's copies of unreadable saves still
#define SLOTS 3

static bool sSetUp;
static char sDir[SLOTS][MAX_PATH];    // each slot's save game's folder, once found ("" not known)
static wchar_t sShown[SLOTS][48];     // the name each slot's save game is known to have ("" not known)

static void Log(const char* fmt, ...) {
    char line[MAX_PATH + 160];
    va_list ap;

    if (!gLusxLogOn) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    strcat(line, "\n");
    LusxLog_Send(line);
}

static bool Exists(const char* path) {
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static void Strip(char* dir) {
    size_t n = strlen(dir);
    while (n > 0 && dir[n - 1] == '\\') {
        dir[--n] = '\0';
    }
}

// the slot (0-2) of a 2Ship save file's name, "file1.json" or "file1backup.json"; -1 for any other file
static int SlotOf(const char* name) {
    const char* base = strrchr(name, '\\');
    const char* slash = strrchr(name, '/');
    base = slash > base ? slash : base;
    base = base != NULL ? base + 1 : name;
    if (_strnicmp(base, "file", 4) != 0 || base[4] < '1' || base[4] >= '1' + SLOTS) {
        return -1;
    }
    if (_stricmp(base + 5, ".json") != 0 && _stricmp(base + 5, "backup.json") != 0) {
        return -1;
    }
    return base[4] - '1';
}

// the file name alone, from a path
static const char* BaseOf(const char* name) {
    const char* base = strrchr(name, '\\');
    const char* slash = strrchr(name, '/');
    base = slash > base ? slash : base;
    return base != NULL ? base + 1 : name;
}

// whether `dir` holds any of slot `slot`'s files
static bool HasSlotFiles(const char* dir, int slot) {
    char file[MAX_PATH];

    snprintf(file, sizeof(file), "%s\\file%d.json", dir, slot + 1);
    if (Exists(file)) {
        return true;
    }
    snprintf(file, sizeof(file), "%s\\file%dbackup.json", dir, slot + 1);
    return Exists(file);
}

// The save game holding slot `slot`'s files: its folder, without the trailing backslash
static bool Find(int slot, char* dir, size_t size) {
    XGAME_FIND_DATA fd;
    bool found = false;

    if (sDir[slot][0] != '\0' && HasSlotFiles(sDir[slot], slot)) {
        snprintf(dir, size, "%s", sDir[slot]);
        return true;
    }
    sDir[slot][0] = '\0';
    sShown[slot][0] = L'\0';
    HANDLE h = XFindFirstSaveGame("U:\\", &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    do {
        Strip(fd.szSaveGameDirectory);
        found = HasSlotFiles(fd.szSaveGameDirectory, slot);
        if (found) {
            snprintf(dir, size, "%s", fd.szSaveGameDirectory);
            snprintf(sDir[slot], sizeof(sDir[slot]), "%s", fd.szSaveGameDirectory);
            wcsncpy(sShown[slot], fd.szSaveGameName, sizeof(sShown[slot]) / sizeof(sShown[slot][0]) - 1);
            sShown[slot][sizeof(sShown[slot]) / sizeof(sShown[slot][0]) - 1] = L'\0';
        }
    } while (!found && XFindNextSaveGame(h, &fd));
    XFindClose(h);
    return found;
}

// a save game's folder and everything in it
static void RemoveSaveGame(const char* dir) {
    WIN32_FIND_DATAA fd;
    char path[MAX_PATH];

    snprintf(path, sizeof(path), "%s\\*", dir);
    HANDLE h = FindFirstFileA(path, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
                SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL); // (SaveMeta.xbx is a system file)
                DeleteFileA(path);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

// the shown name of a save game, in its SaveMeta.xbx: the format XCreateSaveGame writes (a BOM, then "Name=...")
static void Rename(const char* dir, const wchar_t* name) {
    char path[MAX_PATH];
    wchar_t text[160];
    DWORD put = 0;

    snprintf(path, sizeof(path), "%s\\SaveMeta.xbx", dir);
    int n = _snwprintf(text, sizeof(text) / sizeof(text[0]) - 1, L"%lcName=%ls\r\n", (wint_t)0xFEFF, name);
    if (n <= 0) {
        return;
    }
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_SYSTEM, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    WriteFile(h, text, (DWORD)n * sizeof(wchar_t), &put, NULL);
    FlushFileBuffers(h);
    CloseHandle(h);
}

static void ShownName(int slot, const char* player, wchar_t* out, size_t size) {
    int n = _snwprintf(out, size - 1, L"File %d", slot + 1);
    if (n > 0 && player != NULL && player[0] != '\0') {
        n += _snwprintf(out + n, size - 1 - n, L" - ");
        for (const char* p = player; *p != '\0' && (size_t)n < size - 1; p++) {
            out[n++] = (wchar_t)(unsigned char)*p;
        }
    }
    out[n > 0 ? ((size_t)n < size ? n : size - 1) : 0] = L'\0';
}

// the slot's save game (in `dir`) named after `player`, unless it is already
static void NameSlot(int slot, const char* dir, const char* player) {
    wchar_t name[48];

    ShownName(slot, player, name, sizeof(name) / sizeof(name[0]));
    if (wcscmp(sShown[slot], name) == 0) {
        return;
    }
    Rename(dir, name);
    wcscpy(sShown[slot], name);
    Log("lus-x: save: file %d's save game named \"%ls\"", slot + 1, name);
}

// The name the dashboard shows for the game: U:\TitleMeta.xbx, which the system writes only the first time (libxapi
// xapiinit.c: "only write if this is an empty file"), so it keeps the name of the build that first ran. Rewritten
// when it isn't this XBE's.
static void FixTitleName(void) {
    // the running XBE's certificate (its header is at 0x10000, the certificate's address at +0x118), and the title
    // name in it (+0xC, 40 wide characters)
    const BYTE* cert = *(const BYTE* const*)(0x10000 + 0x118);
    wchar_t title[41];
    memcpy(title, cert + 0xC, 40 * sizeof(wchar_t));
    title[40] = L'\0';
    wchar_t want[64], have[64];
    DWORD got = 0, put = 0;

    int n = _snwprintf(want, sizeof(want) / sizeof(want[0]) - 1, L"%lcTitleName=%ls\r\n", (wint_t)0xFEFF, title);
    if (n <= 0) {
        return;
    }
    HANDLE h = CreateFileA("U:\\TitleMeta.xbx", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        ReadFile(h, have, sizeof(have), &got, NULL);
        CloseHandle(h);
        if (got == (DWORD)n * sizeof(wchar_t) && memcmp(have, want, got) == 0) {
            return;
        }
    }
    SetFileAttributesA("U:\\TitleMeta.xbx", FILE_ATTRIBUTE_NORMAL);
    h = CreateFileA("U:\\TitleMeta.xbx", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_SYSTEM, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    WriteFile(h, want, (DWORD)n * sizeof(wchar_t), &put, NULL);
    FlushFileBuffers(h);
    CloseHandle(h);
    Log("lus-x: save: the dashboard's name for the game set to \"%ls\"", title);
}

// a file moved across drives (copied, then the old one deleted); an existing target is left alone
static bool Move(const char* from, const char* to) {
    if (Exists(to)) {
        return false;
    }
    if (!CopyFileA(from, to, TRUE)) {
        Log("lus-x: save: %s not moved to %s (%lu)", from, to, (unsigned long)GetLastError());
        return false;
    }
    DeleteFileA(from);
    return true;
}

// Saves where the builds before kept them (T:\saves), into the slots' own save games ("File N" until the file select
// reads them and they're named after their players: SaveManager.cpp)
static void MoveOldSaves(void) {
    static const char* const kFiles[] = { "file%d.json", "file%dbackup.json" };
    char from[MAX_PATH], to[MAX_PATH], dir[MAX_PATH], file[32];
    int moved = 0;

    for (int slot = 0; slot < SLOTS; slot++) {
        if (!HasSlotFiles(OLD_DIR, slot)) {
            continue;
        }
        if (Find(slot, dir, sizeof(dir))) {
            Log("lus-x: save: file %d has its save game already: the one in %s is left there", slot + 1, OLD_DIR);
            continue;
        }
        wchar_t name[48];
        ShownName(slot, NULL, name, sizeof(name) / sizeof(name[0]));
        DWORD rv = XCreateSaveGame("U:\\", name, OPEN_ALWAYS, 0, dir, sizeof(dir));
        if (rv != ERROR_SUCCESS) {
            Log("lus-x: save: no save game for file %d (%lu): it stays in %s", slot + 1, (unsigned long)rv, OLD_DIR);
            continue;
        }
        Strip(dir);
        for (const char* pattern : kFiles) {
            snprintf(file, sizeof(file), pattern, slot + 1);
            snprintf(from, sizeof(from), "%s\\%s", OLD_DIR, file);
            snprintf(to, sizeof(to), "%s\\%s", dir, file);
            if (Exists(from) && Move(from, to)) {
                moved++;
            }
        }
        if (!HasSlotFiles(dir, slot)) {
            RemoveSaveGame(dir); // nothing went in: no empty save game on the dashboard
        }
    }
    if (moved > 0) {
        Log("lus-x: save: %d files moved from %s into their slots' own save games", moved, OLD_DIR);
    }
}

static void SetUp(void) {
    if (sSetUp) {
        return;
    }
    sSetUp = true;
    FixTitleName();
    MoveOldSaves();
}

extern "C" {

// The path of 2Ship's save file `name` ("file1.json"): a slot's file in the slot's save game if it has one (1); any
// other file, or a slot's with no save game yet, at `fallback`, where 2Ship would keep it (0)
int LusXbox_SavePath(const char* name, const char* fallback, char* out, unsigned size) {
    char dir[MAX_PATH];
    const int slot = SlotOf(name);

    SetUp();
    if (slot >= 0 && Find(slot, dir, sizeof(dir))) {
        snprintf(out, size, "%s\\%s", dir, BaseOf(name));
        return 1;
    }
    snprintf(out, size, "%s", fallback);
    return 0;
}

// The path to write `name` to: for a slot's file, in its save game, made now if it has none, and named after `player`
// (ASCII; NULL or "" leaves it "File N"). `fallback` for any other file, or if the save game can't be made (the disk
// full, say): then the save still goes somewhere.
int LusXbox_SaveOpen(const char* name, const char* player, const char* fallback, char* out, unsigned size) {
    char dir[MAX_PATH];
    const int slot = SlotOf(name);

    SetUp();
    if (slot < 0) {
        snprintf(out, size, "%s", fallback);
        return 0;
    }
    if (!Find(slot, dir, sizeof(dir))) {
        wchar_t shown[48];
        ShownName(slot, player, shown, sizeof(shown) / sizeof(shown[0]));
        DWORD rv = XCreateSaveGame("U:\\", shown, OPEN_ALWAYS, 0, dir, sizeof(dir));
        if (rv != ERROR_SUCCESS) {
            Log("lus-x: save: no save game for file %d (%lu): saved in %s", slot + 1, (unsigned long)rv, OLD_DIR);
            CreateDirectoryA(OLD_DIR, NULL);
            snprintf(out, size, "%s", fallback);
            return 0;
        }
        Strip(dir);
        snprintf(sDir[slot], sizeof(sDir[slot]), "%s", dir);
        wcscpy(sShown[slot], shown);
        Log("lus-x: save: file %d's save game made: %s", slot + 1, dir);
    }
    if (player != NULL && player[0] != '\0') {
        NameSlot(slot, dir, player);
    }
    snprintf(out, size, "%s\\%s", dir, BaseOf(name));
    return 1;
}

// a slot's save game named after its player, if it has one (the file select reading a save moved in as "File N")
void LusXbox_SaveName(const char* name, const char* player) {
    char dir[MAX_PATH];
    const int slot = SlotOf(name);

    if (slot >= 0 && player != NULL && player[0] != '\0' && Find(slot, dir, sizeof(dir))) {
        NameSlot(slot, dir, player);
    }
}

// `name` deleted or moved away: the slot's save game goes once none of the slot's files is left in it, so the
// dashboard isn't left with an empty one
void LusXbox_SaveGone(const char* name) {
    const int slot = SlotOf(name);

    if (slot < 0 || sDir[slot][0] == '\0') {
        return;
    }
    if (!HasSlotFiles(sDir[slot], slot)) {
        RemoveSaveGame(sDir[slot]);
        Log("lus-x: save: file %d's save game removed", slot + 1);
        sDir[slot][0] = '\0';
        sShown[slot][0] = L'\0';
    }
}

// after each save: the folder's own entries on the disk too, not only the file's bytes, before anyone turns the
// console off (the power button skips the cache's flush)
void LusXbox_FlushDirOf(const char* file) {
    char dir[MAX_PATH];

    snprintf(dir, sizeof(dir), "%s", file);
    for (char* p = dir; *p; p++) {
        if (*p == '/') {
            *p = '\\';
        }
    }
    char* cut = strrchr(dir, '\\');
    if (cut == NULL) {
        return;
    }
    *cut = '\0';
    HANDLE h = CreateFileA(dir, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    FlushFileBuffers(h);
    CloseHandle(h);
}

}
