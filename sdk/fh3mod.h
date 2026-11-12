/*
 * fh3mod.h -- FH3 Mod Platform API (MIT licensed, see LICENSE).
 *
 * The FH3 Mod Platform is the d3dcompiler_47.dll (and, the same file, the
 * DWrite.dll) next to azimuth.exe. It loads every DLL in the fh3-mods folder
 * and offers them the services in FH3ModApi below, so mods don't each
 * reinvent them and don't step on each other's files.
 *
 * A mod is a 64-bit Windows DLL in fh3-mods that exports:
 *
 *     FH3MOD_EXPORT int fh3mod_init(const FH3ModApi* api);
 *
 * The platform calls it once, on its own thread (never under the Windows
 * loader lock), right after loading the DLL. Do your setup there, start
 * your own threads for anything slow, and return 0. The api pointer stays
 * valid for the life of the process.
 *
 * Strings passed in are copied; nothing you get back needs freeing. All
 * functions are thread-safe.
 *
 * Compatibility: api->api_version only ever grows, and new members are only
 * ever added at the end. Check api->size (or api_version) before using a
 * member newer than the version you built against.
 */
#ifndef FH3MOD_H
#define FH3MOD_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FH3MOD_API_VERSION 16

#ifdef __cplusplus
#define FH3MOD_EXPORT extern "C" __declspec(dllexport)
#else
#define FH3MOD_EXPORT __declspec(dllexport)
#endif

/* Opaque handle for one registered mod. */
typedef struct FH3Mod FH3Mod;
/* Opaque handle for one hook (API 3). */
typedef struct FH3Hook FH3Hook;
/* One row on the Mods page (API 4), defined below FH3ModApi. */
typedef struct FH3Setting FH3Setting;
/* A page of your own, and one of its rows (API 8), defined below FH3ModApi. */
typedef struct FH3Page FH3Page;
typedef struct FH3PageRow FH3PageRow;
/* A title-screen entry (API 9), defined below FH3ModApi. */
typedef struct FH3TitleEntry FH3TitleEntry;
typedef struct FH3Hotkey FH3Hotkey;
typedef struct FH3TuningRow FH3TuningRow;

/* The CPU's registers at an instruction hook (API 7): see hook_instruction.
   rsp is what it was at the hooked instruction; changing it has no effect. */
typedef struct FH3Xmm {
    union {
        float f[4];
        double d[2];
        uint32_t u32[4];
        uint64_t u64[2];
    };
} FH3Xmm;
typedef struct FH3Context {
    uint64_t rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rflags;
    uint64_t reserved;
    FH3Xmm xmm[16];
} FH3Context;
typedef void (*FH3InstructionFn)(FH3Context* ctx, void* user);

/* A command (API 9, add_command): runs on the platform's command thread,
   not the game's, with the rest of the request's line in `args`. Write
   the answer into `reply` (UTF-8, at most reply_size bytes with its NUL;
   the platform cuts anything longer). Start it with "error: " for a
   failure, which fh3mod.exe exits 1 on. */
typedef void (*FH3CommandFn)(void* user, const char* name, const char* args, char* reply, size_t reply_size);

typedef struct FH3ModInfo {
    uint32_t size;                /* sizeof(FH3ModInfo) */
    const char* name;             /* display name, UTF-8: "WheresMyBuddy" */
    const char* version;          /* "1.3.0" */
    const char* github_repo;      /* "Owner/Repo" to check for new releases, or NULL */
    const char* platform_version; /* the platform version this mod ships with, e.g. FH3MOD_PLATFORM_VERSION */
    int check_updates;            /* 0 = never go online for this mod */
    /* API 6 (platform 2.5.0): what your mod adds, in a sentence or two, for the MODS tab's
       Installed mods page. May be NULL. Leave it out (an older FH3ModInfo) on older platforms. */
    const char* description;
    /* API 9 (platform 2.8.0): nonzero for a mod that shouldn't run in online play, because it
       changes driving or what other players see (a nitro boost, say). When the game is started
       from the FH2MP launcher, register_mod then returns NULL, so return from fh3mod_init
       straight away (call register_mod first, before you do anything else); from the next
       launch the platform doesn't load the DLL at all online. Players can still choose for
       themselves on MODS > Installed mods ("Load when playing online"). Older platforms
       ignore it. */
    int offline_only;
} FH3ModInfo;

/* ---- Custom upgrades (API 11, register_upgrade) ---- */
typedef struct FH3UpgradeStage {
    const wchar_t* name;          /* "Street Nitrous Kit" */
    const wchar_t* description;
    int32_t price;                /* credits; ignored for stage 0 (stock is free) */
    const char* icon;             /* game path of the small icon, e.g. "GAME:\\Media\\UI\\Textures\\Data_Bound\\Upgrade_Parts\\Custom\\x_Icon.swatchbin"; NULL = the game's default */
    const char* icon_large;       /* the large one, or NULL */
} FH3UpgradeStage;
/* Called (game thread) when the stage on a car is known or changes: the car being driven changed, or a stage was
   installed. `garage` is the car's garage id. Keep it short and non-blocking. */
typedef void (*FH3UpgradeStageFn)(void* user, uint32_t garage, int stage);
typedef struct FH3UpgradeDef {
    uint32_t size;                /* sizeof(FH3UpgradeDef) */
    const char* id;               /* stable key of this upgrade, e.g. "nitrosystem2.kit" (letters, digits, '.', '_') */
    const wchar_t* name;          /* the type's name: "Nitrous Kit" */
    const wchar_t* description;
    int32_t area;                 /* UpgradeAreas id: 1 Engine (0 = 1), 4 Tires and rims, 8 Conversion */
    const char* icon;             /* the type's icons (game paths) or NULL */
    const char* icon_large;
    int32_t stage_count;          /* 2 to 10, stage 0 is stock */
    const FH3UpgradeStage* stages;
    FH3UpgradeStageFn on_stage;   /* may be NULL */
    void* user;
    int32_t preferred_slot;       /* -1 = any; a mod moving its old upgrade over asks for the slot it had (0 = the first) */
    /* (platform 1.0.0, API 12; may be left out: a definition without it is accepted)
       the game part whose shop camera this upgrade's list uses, as its part enum: 10 front rims, 11 rear rims,
       8 tire compound, 38/39 front/rear tire width, ...; 0 = the area's default camera */
    int32_t camera_part;
} FH3UpgradeDef;
#define FH3_UPGRADEDEF_SIZE_V11 ((uint32_t)offsetof(FH3UpgradeDef, camera_part))

typedef struct FH3ModApi {
    uint32_t size;                /* sizeof(FH3ModApi) as the platform knows it */
    uint32_t api_version;         /* FH3MOD_API_VERSION of the platform */
    const char* platform_version; /* "2.2.0" */
    const wchar_t* azimuth_dir;   /* folder with azimuth.exe, no trailing slash */
    const wchar_t* game_dir;      /* the game's Mount folder (game:\), or "" if not found */

    /* Call once from fh3mod_init. Returns your handle for the calls below.
       The platform then shows a title-screen notice when a newer release of
       your mod is out on GitHub (checked at most every 6 hours, and again as
       soon as a different version of your mod is installed), or when
       the installed platform is older than info->platform_version. */
    FH3Mod* (*register_mod)(const FH3ModInfo* info);

    /* One line to fh3-mods\platform.log, prefixed with your mod's name. */
    void (*log)(FH3Mod* mod, const char* utf8_line);

    /* ---- String tables -----------------------------------------------------
       The game's text lives in string tables (media\Stripped\StringTables\
       <language>.zip\<Table>.str). Edits are applied to every language and
       written to Forza Horizon 2 Extra Options' mediaoverride folder, merged
       with every other mod's edits to the same table. The game reads its
       string tables once at startup: edits made in fh3mod_init are always in
       time; later ones show from the next launch. See docs/string-tables.md
       for how to find keys. */

    /* Set the string with `key` in `table` ("Main.str") to `text`. */
    void (*set_string)(FH3Mod* mod, const char* table, uint16_t key, const wchar_t* text);
    /* Replace every string in `table` that is exactly `from` with `to`. */
    void (*replace_string)(FH3Mod* mod, const char* table, const wchar_t* from, const wchar_t* to);
    /* Forget this mod's edits to `table`, or to every table if NULL. */
    void (*clear_strings)(FH3Mod* mod, const char* table);
    /* Write the changed tables now. Done for you after fh3mod_init returns;
       call it yourself after edits you make later. */
    void (*apply_strings)(FH3Mod* mod);

    /* ---- Title screen ------------------------------------------------------
       Lines shown under "PRESS (A) TO START", with every other mod's. Keep
       each under ~40 characters. "\n" separates lines; NULL or "" clears.
       Written when apply_strings runs (or right away after init). */
    void (*set_notice)(FH3Mod* mod, const char* utf8_lines);

    /* ---- The game ----------------------------------------------------------
       Azimuth maps forza_durango_release_final.exe (the 24 Nov 2015 Xbox One
       build) into its own process when you press Play. */

    /* Base address of the mapped game, or NULL if it isn't mapped yet. */
    void* (*game_base)(void);
    /* Waits until the game is mapped (or timeout_ms passes; 0xFFFFFFFF =
       forever) and returns its base, or NULL. */
    void* (*wait_for_game)(uint32_t timeout_ms);

    /* ==== API version 2 (platform 2.1.0): check api_version >= 2 ======== */

    /* ---- Game files --------------------------------------------------------
       Replace a file the game reads from game:\media, through Extra Options'
       mediaoverride folder. `media_path` is relative to media, with a zip's
       name standing in for the zip: "drivatarglobalcache/DefaultCache.bin",
       or "UI/somefile.xml" for a file inside media\UI.zip. One mod per
       file: returns 1 if written (or already identical), 0 if another mod
       already replaces it or the write failed (the log says which). String
       tables and the game database have their own functions above and
       below, and can't be replaced whole. */
    int (*set_file)(FH3Mod* mod, const char* media_path, const void* data, size_t size);
    /* Stop replacing a file (or all of yours, if NULL): the override is
       deleted and the game reads its own copy again. Files a mod no longer
       replaces at all (because it was removed) are cleaned up shortly after
       startup. */
    void (*clear_file)(FH3Mod* mod, const char* media_path);

    /* ---- Game database -----------------------------------------------------
       media\Stripped\gamedbRC.slt is a SQLite database with most of the
       game's data (cars, events, AI drivers). add_sql queues statements
       (UTF-8, several per call is fine) to run against a copy of it; every
       mod's statements run in load order, each mod's in a transaction of its
       own, and the copy goes to mediaoverride. If a mod's SQL fails, only
       that mod's changes are rolled back, and the error is logged. The copy
       is rebuilt only when something changed. Statements added in
       fh3mod_init are applied for you; later ones need apply_database. */
    void (*add_sql)(FH3Mod* mod, const char* utf8_sql);
    /* Drop all of your statements (call apply_database after). */
    void (*clear_sql)(FH3Mod* mod);
    /* Rebuild the database copy now if anything changed. Slow (seconds):
       call it from your own thread. The game reads the database once, at
       startup. */
    void (*apply_database)(FH3Mod* mod);

    /* ---- Other mods --------------------------------------------------------
       Mods can offer each other functions. Publish a struct of function
       pointers under a name, like "WheresMyBuddy.Friends", and treat it like
       this API: never change a published member, only add new ones at the
       end and bump the version. */

    /* Offer `iface` (which must stay valid) as `name`, version `version`.
       Another mod can't take a name you published. */
    void (*publish_interface)(FH3Mod* mod, const char* name, uint32_t version, const void* iface);
    /* The interface published as `name`, if its version is at least
       `min_version`; otherwise NULL. Mods start in alphabetical order, so
       look up interfaces after wait_for_mods. */
    const void* (*get_interface)(const char* name, uint32_t min_version);
    /* Waits until every mod's fh3mod_init has returned (or timeout_ms
       passes). Returns 1 when they have. Call it from your own thread:
       from inside fh3mod_init it returns 0 straight away. */
    int (*wait_for_mods)(uint32_t timeout_ms);
    /* The version string of the registered mod called `name`, or NULL if
       it isn't installed. */
    const char* (*mod_version)(const char* name);

    /* ==== API version 3 (platform 2.2.0): check api_version >= 3 ======== */

    /* ---- Hooks -------------------------------------------------------------
       Change what game code does, through the platform, so every mod's
       changes to the same code fit together: several mods can hook one
       function, each calls on to the next through `original`, and they can
       be switched on and off in any order. Hooks on exactly the same function,
       call site or pointer share it; the platform refuses a hook whose bytes
       overlap any other hook's, or game code that Extra Options (or a mod
       patching the game by itself) already changed, and logs who hooked
       what. Crash reports list every hook in place.

       Hooks in game code have to wait for wait_for_code (below). Hooks
       elsewhere (Azimuth, a Windows DLL) can be made at any time.

       Each call makes a hook, switched off, and returns it (or NULL, with
       the reason in the log). `name` is for the log and crash reports, like
       "RadioStreamFmod::createSound"; it may be NULL. `detour` is your
       function, with the same signature and calling convention as the code
       it replaces. `*original` is set before the call returns: call it from
       your detour to run the code you replaced (and any other mod's detour
       behind yours). It's safe to call at any time; while your hook is off
       it goes straight to the game's own code.

       When several mods hook the same place, the most recently enabled
       detour runs first. */

    /* An inline hook at the start of a function (`target`), with MinHook. */
    FH3Hook* (*hook_function)(FH3Mod* mod, const char* name, void* target, void* detour, void** original);
    /* One CALL rel32 or JMP rel32 instruction at `call_site`: only that
       call goes to your detour; `*original` is the function it called. */
    FH3Hook* (*hook_call)(FH3Mod* mod, const char* name, void* call_site, void* detour, void** original);
    /* A function pointer in memory, like a vtable slot: `*slot` is replaced,
       and `*original` is what it held. */
    FH3Hook* (*hook_pointer)(FH3Mod* mod, const char* name, void** slot, void* detour, void** original);
    /* Switch `hook` on or off, or all of your hooks if it's NULL (in one go:
       the game's threads are paused once, for all of them). Returns 1 if
       they all are now; 0 if any couldn't be, with the reason in the log. */
    int (*enable_hooks)(FH3Mod* mod, FH3Hook* hook);
    int (*disable_hooks)(FH3Mod* mod, FH3Hook* hook);
    /* Like wait_for_game, but also waits until Extra Options has finished
       patching the game (it checks the code it patches first, and patches
       nothing if any of it was changed). Returns the game's base when it's
       safe to hook game code, or NULL on timeout. Call it from your own
       thread. */
    void* (*wait_for_code)(uint32_t timeout_ms);

    /* ==== API version 4 (platform 2.3.0): check api_version >= 4 ======== */

    /* ---- In-game settings --------------------------------------------------
       On/Off rows on the game's own Mods page (Pause > Settings > MODS),
       next to every other mod's. The tile and the page only appear when at
       least one mod adds a row. Add your rows from fh3mod_init: the page is
       written when every mod's fh3mod_init has returned, and rows added
       after that only show from the next launch.

       Your mod keeps owning the value: pass the current one in, and when the
       player changes it, on_change tells you the new one, so you can save it
       wherever you keep your settings (your ini) and use it. The platform
       doesn't store values. Rows marked needs_restart get a note in their
       description, and changing one brings up the game's own message box
       when the player leaves the Mods page, offering to restart the game.
       Returns 1 if the row was added, 0 if not (the log says why). */
    int (*add_setting)(FH3Mod* mod, const FH3Setting* setting);
    /* The row's current value, or -1 if you have no row called `key`. */
    int (*get_setting)(FH3Mod* mod, const char* key);
    /* You changed the value yourself (a hotkey, say): the page shows it the
       next time it opens. on_change isn't called. */
    void (*set_setting)(FH3Mod* mod, const char* key, int value);

    /* ==== API version 5 (platform 2.4.0): check api_version >= 5 ======== */

    /* Sliders on the Mods page: an FH3Setting with kind = FH3_SETTING_SLIDER
       (see FH3Setting below). On an older platform a slider would come out as
       an On/Off row, so only add one when api_version >= 5. */
    /* A slider's current value, or 0 if you have no slider called `key`. */
    float (*get_slider)(FH3Mod* mod, const char* key);
    /* You changed a slider's value yourself: the page shows it the next time
       it opens. on_slide isn't called. */
    void (*set_slider)(FH3Mod* mod, const char* key, float value);

    /* ==== API version 7 (platform 2.6.0): check api_version >= 7 ======== */

    /* ---- Precise changes to game code ---------------------------------------
       For when hooking a whole function, one call or one pointer is too
       blunt: a constant in the middle of a function, a branch to flip, a
       register to read at one exact instruction. Both kinds below come back
       as an FH3Hook, made switched off, and are switched with enable_hooks
       and disable_hooks like any other hook. The same rules hold: game code
       has to wait for wait_for_code, the bytes are checked against the exe on
       disk and against every other hook, and it's all in the log and in
       crash reports. Addresses are exact: use them on the game build you
       found them in (the one Azimuth runs). */

    /* Bytes written in place: `size` bytes (at most 32) of `bytes` at `at`,
       which has to hold `expect` (the same size) first; pass NULL to take
       what's there as long as it matches the exe on disk. Disabling puts
       the original bytes back. Only one patch can cover a byte, and a patch
       can't overlap any other hook. */
    FH3Hook* (*patch_bytes)(FH3Mod* mod, const char* name, void* at, const void* expect, const void* bytes, uint32_t size);

    /* An instruction hook: when the game reaches the instruction at `at`
       (anywhere in a function, not only its start), `fn(ctx, user)` is
       called with the CPU's registers in `ctx`, before that instruction
       runs. Change them in `ctx` (not rsp) and the game carries on with your
       values. `at` has to be the start of an instruction, after the
       function's prolog, with nothing in the function jumping into the
       bytes the hook replaces (the platform checks; the log says why when it
       refuses). Several mods may hook the same instruction: the most
       recently enabled `fn` runs first. `fn` runs on the game's thread,
       often every frame: keep it short. If it crashes, that hook is
       switched off and the crash goes in the log. */
    FH3Hook* (*hook_instruction)(FH3Mod* mod, const char* name, void* at, FH3InstructionFn fn, void* user);

    /* ==== API version 8 (platform 2.7.0): check api_version >= 8 ======== */

    /* ---- Pages -------------------------------------------------------------
       A page of your own in the game's menus, laid out like the Mods page: a
       list of rows, each with a label on the left and a value on the right,
       and the selected row's description beside the list. Your mod's page
       under MODS > Installed mods gets a row that opens it. A page can also
       take over a pause-menu tile that does nothing on PC: FH3Page.tile.

       Add pages from fh3mod_init (one added later only shows from the next
       launch). Their rows can change at any time: the page is written each
       time it's opened, from the rows you last set. Returns 1 if the page was
       added, 0 if not (the log says why). */
    int (*add_page)(FH3Mod* mod, const FH3Page* page);
    /* Your page `page_key` now has these `count` rows (copied), in this
       order; the next time it opens, it shows them. Call it whenever they
       change, or from the page's on_open. Returns 1, or 0 if you have no page
       called `page_key` (or a row is unusable; the log says which). */
    int (*set_page_rows)(FH3Mod* mod, const char* page_key, const FH3PageRow* rows, uint32_t count);

    /* ---- Pages of your own design --------------------------------------
       Instead of rows, a page on a tile can be XAML you write: the game's
       own controls, styles and textures, laid out however you like, inside
       the frame of the game's options pages (background, player bar, help
       bar). `content` replaces what's between the title and the help bar
       (the HUD options page's BasicOptions panel), and must name the
       element to focus first x:Name="FirstItem". `resources` (may be NULL)
       goes into the page's ResourceDictionary. `help` (may be NULL) replaces
       the help bar's DefaultHelpPanel; NULL leaves only Back. All three are
       UTF-8 XAML fragments, using the page's xmlns (local: is ForzaUI).
       Like rows, the XAML is copied, and the page is written from the last
       you set each time it opens. A page with XAML shows it on its tile;
       under MODS > Installed mods it shows its rows if it has any, and
       otherwise its row says which tile opens it. Returns 1, or 0 if you
       have no page called `page_key`, `content` has no FirstItem, or a
       fragment's tags don't balance (the page keeps the XAML it had).

       Bindings: slot n (0..15) is an int of the page's, "FH3Mod_Slot_<n>",
       which XAML can bind two-way (a list's SelectedIndex, say), and
       "FH3Mod_Slot_<n>_Is_<v>" is true while it's v, 0..9999 (for
       Visibility, with the converters FH3Mod_ShowIf, FH3Mod_ShowIfHidden
       and FH3Mod_HideIf the platform puts in the page's resources). Slots
       are 0 when the page opens; set the ones you want otherwise in
       on_open. */
    int (*set_page_xaml)(FH3Mod* mod, const char* page_key, const char* resources, const char* content, const char* help);
    /* A slot's value (0 if there's no such page or slot). */
    int (*get_page_slot)(FH3Mod* mod, const char* page_key, int slot);
    /* Sets a slot. From on_button or on_slot, the page's bindings see it
       straight away (from on_slot, except the slot being set). From
       on_open, it's what the page opens with. Anywhere else, it's only
       stored: the page doesn't show it, and opening resets it. Returns 1,
       or 0 for no such page or slot. */
    int (*set_page_slot)(FH3Mod* mod, const char* page_key, int slot, int value);
    /* Moves the focus to the Button, Slider or SpinnerControl named `name`
       in the page's XAML, from on_button. Returns 1 if it did. */
    int (*focus_page_element)(FH3Mod* mod, const char* page_key, const char* name);
    /* A line of text on the tile your page has taken over, at its bottom
       left in the game's own style for a tile's count (a club tile's
       member count): a score, say. Plain ASCII, up to 40 characters; NULL
       or "" shows nothing. Call it from any thread, whenever it changes:
       the pause menu reads it each time it's shown. Returns 1, or 0 if you
       have no page called `page_key` or it isn't on a tile. */
    int (*set_page_tile_text)(FH3Mod* mod, const char* page_key, const char* text);
    /* That tile's colours, as 0xAARRGGBB: `background`, and `text` for its
       title and your line over it. 0 leaves the game's own (its light grey
       tile, dark title and orange line). The icon takes the background's
       colour, so it stays a shade of it. Like the text, the pause menu
       reads them each time it's shown. Returns 1, or 0 if you have no page
       called `page_key` or it isn't on a tile. */
    int (*set_page_tile_colors)(FH3Mod* mod, const char* page_key, uint32_t background, uint32_t text);

    /* ==== API version 9 (platform 2.8.0): check api_version >= 9 ======== */

    /* ---- Title-screen entries ----------------------------------------------
       A line of your own on the title screen, under "PRESS (A) TO START" and
       its notices, that runs your code when the player presses its button:
       "(Y) START ON STORM ISLAND". Each entry has a button of its own, Y, X, RB
       then LB in the order mods add them (or the one you ask for, if it's
       free): four at most, from every mod together. The lines show while the
       title screen waits for A, and go with "PRESS (A) TO START" once the
       game starts signing in. If the platform can't hook the title screen
       (the game's code isn't as expected), they don't show at all and the
       log says why. An entry can be a list instead (FH3TitleEntry.choices):
       its press shows the game's message box over the title screen with the
       choices and a Back, and the pick calls on_choose. A list of more than
       10 is shown 8 choices at a time, with Previous and Next.

       Add entries from fh3mod_init: the title screen is written when every
       mod's fh3mod_init has returned, so one added later only shows from the
       next launch. Returns 1 if the entry was added, 0 if not (a bad or
       repeated key, no label, no on_press, no button left, a list without
       on_choose or with more than FH3_TITLE_MAX_CHOICES; the log says
       why). */
    int (*add_title_entry)(FH3Mod* mod, const FH3TitleEntry* entry);
    /* The key of your entry that started the game (its on_press or
       on_choose returned FH3_TITLE_START), or NULL if the player started it with A, another
       mod's entry did, or it hasn't started yet. A later press of A on the
       title screen sets it back to NULL. Call it from any thread: from your
       hook on the world loading, say, to know whether this start is yours. */
    const char* (*title_started_by)(FH3Mod* mod);

    /* ---- Online play -------------------------------------------------------
       1 when the game was started from the FH2MP launcher (online play on the
       FH2MP servers), 0 otherwise. It's decided once, before any mod loads,
       and doesn't change during the session. Use it to turn off just the part
       of your mod that shouldn't run online; for the whole mod, set
       FH3ModInfo.offline_only instead. Call it from anywhere, fh3mod_init
       included. */
    int (*playing_online)(void);

    /* ---- Sessions ----------------------------------------------------------
       This run of the game's id, "20261021-203224-1234": when the platform
       started, and the process id (the game can start twice in one launch,
       and Restart now starts a new process). The same id is in
       platform.log's third line, in the copy of it the platform keeps
       (fh3-mods\platform-logs\platform-<id>.log, the newest 20 sessions'),
       and in fh3-mods\platform-session.json, which says for tools what's
       running (docs/api.md). Name your own logs with it, or let
       session_log_path do that. Call it from anywhere. */
    const char* (*session_id)(void);
    /* <azimuth_dir>\<folder>\logs\<session id>.log, with the folder made,
       where `folder` is your mod's own folder next to azimuth.exe (L"MyMod").
       The newest `keep` logs there are kept, counting this one (at least 1),
       and older *.log files in it are deleted. Open the file yourself; the
       path stays valid for the session. NULL if `folder` isn't a plain folder
       name or can't be made (the log says why). */
    const wchar_t* (*session_log_path)(FH3Mod* mod, const wchar_t* folder, uint32_t keep);

    /* ---- Save snapshots ----------------------------------------------------
       A copy of the saves folder (<azimuth_dir>\data\titles\1A96545B\saves)
       this session, before the game reads it: for a mod being tested, or one
       that changes what gets saved. Call it from fh3mod_init. Once every mod's
       fh3mod_init has returned, the platform copies the folder (once, however
       many mods ask) to fh3-mods\save-snapshots\<session id>\, keeps the
       newest `keep` copies (the most any mod asked for, at least 1), and says
       in the log which mods asked and why. The game reads the saves when A is
       pressed on the title screen, long after. The platform only reads the
       saves, never writes to them; to go back, copy a snapshot's contents
       over the saves folder with the game closed (all of it: Azimuth checks
       the saves' hashes). Returns 1 if a copy will be made, 0 if there's no
       saves folder or the call came after fh3mod_init (the log says why). */
    int (*request_save_snapshot)(FH3Mod* mod, const char* reason, uint32_t keep);

    /* ---- Crash dumps -------------------------------------------------------
       The minidump the platform writes with every crash report has the
       writable data (globals) of the DLLs it loaded, yours included. Memory
       behind your globals, on the heap (your tables, the state your hooks
       keep), only goes in if you add it here: `n` bytes at `p`, 1 byte to
       64 MB, in every dump from now on. Keep it to what you'd want to see
       after a crash; 64 ranges at most, from every mod together. The memory
       must stay allocated (or the range is skipped at the crash). Any
       thread. */
    void (*add_dump_memory)(FH3Mod* mod, const void* p, size_t n);

    /* ---- Hotkeys -----------------------------------------------------------
       A keyboard key (with FH3_KEY_ modifiers) and a controller chord that
       call your on_press, for a "mark this spot" key or a live switch while
       testing. Keys are only watched while the game's window is in front, and
       the game still gets every key and button: pick ones that do nothing
       harmful in game (F-keys; LB+RB with a face button). Each hotkey gets two
       rows on the Mods page, under your mod's: its key (Off, F1 to F12,
       Ctrl+F1 to F12, or yours if it's none of those) and its controller chord
       (Off, or LB+RB with Y, X, B, A or a d-pad direction). A change there
       takes effect straight away and calls on_rebind, so you can save it:
       the platform doesn't keep bindings, so pass the saved ones in next time.
       Add hotkeys from fh3mod_init, so their rows show. Returns 1 if added, 0
       if not (the log says why). See FH3Hotkey below. */
    int (*add_hotkey)(FH3Mod* mod, const FH3Hotkey* hotkey);

    /* ---- Commands ----------------------------------------------------------
       A command other programs can run while the game does, to flip a
       setting, ask for numbers or mark a spot without touching the game:
       `fh3mod MyMod shadows off` with fh3mod.exe (in the platform's zip), or
       one line of UTF-8, "<mod> <command> [args]", on the named pipe
       \\.\pipe\fh3mod-<process id> (platform-session.json has its name).
       "list" lists every mod's commands with their help. The pipe only
       starts once a mod adds a command, and only programs this Windows user
       runs on this PC can connect. `name` is letters, digits and '_', unique
       within your mod; `help` is a line for "list" (may be NULL). Commands
       run one at a time, on the platform's command thread (see
       FH3CommandFn). Returns 1 if added, 0 if not (the log says why). */
    int (*add_command)(FH3Mod* mod, const char* name, const char* help, FH3CommandFn fn, void* user);

    /* ==== API version 10: check api_version >= 10 ======================== */

    /* Add a string to one of the game's database-backed string tables (Upgrades.str, UpgradeTypes.str, ... -
       the ones whose keys are 32-bit hashes, not the small numbers set_string takes), under a key of your
       choosing (pick something unlikely to clash, e.g. a hash of your mod and text; a key the table already
       has is left alone). Written like set_string (every language, applied after fh3mod_init, shows from
       the next launch). The database refers to the text as "_&" followed by the decimal of
       ((uint64_t)table_hash << 32) | key; FH3_TABLE_UPGRADES and FH3_TABLE_UPGRADETYPES are the table hashes
       of the two tables upgrade names and descriptions live in. */
    void (*add_string_key)(FH3Mod* mod, const char* table, uint32_t key, const wchar_t* text);
    /* ==== API version 11: check api_version >= 11 ======================== */

    /* Custom upgrades (the upgrade bridge). A mod describes an upgrade type (name, description, icons, area, stages
       with names, descriptions and prices) and the platform does the rest: it hands out the ids (part enum, type,
       upgrade and part ids; remembered by `id` in fh3-mods\platform-upgrades.ini), writes the database rows and
       the texts, shows the type in Garage > Upgrades, runs the basket, purchase and "install this setup?" prompt,
       and remembers each car's stage and owned stages. Stage 0 is the stock stage: always owned and fitted on
       every car until another stage is bought. Call from fh3mod_init. Returns a handle (>= 0) or -1 (the log
       says why). Up to 8 upgrades in all, up to 10 stages each. */
    int (*register_upgrade)(FH3Mod* mod, const FH3UpgradeDef* def);
    /* The stage fitted on the car with that garage id (0 = stock), or on the car being driven if garage <= 0 (0 when
       the car being driven isn't one of the player's, a loaner; from API 14 the platform follows the player's car
       itself, so a car switch at an event's car select or a loaner needs nothing from the mod). */
    int (*upgrade_stage)(FH3Mod* mod, int handle, int32_t garage);
    /* The garage id of the car being driven (0 = not known yet, or, from API 14, the driven car isn't one of the
       player's). The stage callbacks run with garage 0, stage 0 when the player gets into such a car. */
    uint32_t (*upgrade_active_garage)(void);
    /* One-time move of a mod's own earlier per-car data to the platform: car `garage` (model `ordinal`, 0 if not
       known) had `stage` fitted and owns the stages whose bits are set in `owned_mask` (bit n = stage n). */
    void (*upgrade_import)(FH3Mod* mod, int handle, int32_t garage, int32_t ordinal, int stage, uint32_t owned_mask);
    /* ==== API version 12: check api_version >= 12 ======================== */

    /* The model (car ordinal, the number in the game's car spec) the platform has recorded for the car with that
       garage id, or 0 if it doesn't know yet (it learns it when the car is seen in the upgrade shop). A mod
       can compare it with the model of the car it finds being driven: when they differ, the game has put the
       player in another car (a loaner in a mission), and the garage car's upgrades don't apply. */
    int32_t (*upgrade_car_model)(int32_t garage);
    /* ==== API version 13: check api_version >= 13 ======================== */

    /* A slider row of your own on a page of the garage's tuning menu (Springs: after Ride Height), laid out like the
       game's own tuning sliders. The row is written into the page when the tuning menu is built, so add rows from
       fh3mod_init. See FH3TuningRow. Returns 1 if added, 0 if not (the log says why). Up to 64 rows in all (the page scrolls). */
    int (*add_tuning_row)(FH3Mod* mod, const struct FH3TuningRow* row);
    /* ==== API version 14: check api_version >= 14 ======================== */

    /* Which of the player's cars is a car with this Data_Car id (the number in the car's spec, model * 1000 + variant,
       or the model itself)? Returns its garage id, or 0 when it isn't one of the player's cars (a mission loaner, an
       AI car). The driven car's garage id wins when it is that model; otherwise the player's car of that model the
       game read last (within a minute: an event's car select reads the chosen car), or, with FH3MOD_CAR_IS_PLAYERS,
       the player's car of that model even without such a read. FH3MOD_CAR_MAKE_DRIVEN makes the answer the driven
       car (upgrade_active_garage, the stage callbacks run). Use it when the game builds or drives a car: the driven
       garage id the platform learns from the game's statements can lag behind a car switch by several seconds.
       Called while the game builds a car (from a hook in the car loader), it also answers whether that car is the
       player's: an AI car of the player's model (a one-make race) gets 0, so it gets none of the player's upgrades. */
    int32_t (*upgrade_garage_for_car)(int32_t car_id, uint32_t flags);
    /* The stage of your upgrade on a car the game builds, by the car's Data_Car id: the stage of the player's car of
       that model (see upgrade_garage_for_car; it becomes the driven car), 0 when it isn't one of the player's cars.
       The call for a mod that changes a car while the game builds it (wheel spacers: the car's track). */
    int (*upgrade_stage_for_car)(FH3Mod* mod, int handle, int32_t car_id);
    /* ==== API version 15: check api_version >= 15 ======================== */

    /* The stage of your upgrade the garage's upgrade list is showing on the car right now (the cursor is on it), or -1
       when none is: the cursor is on something else or has left the list, the stage was put in the basket or the setup
       was installed or abandoned. Unlike upgrade_stage's preview, which answers for 300 ms after each cursor move for the
       rebuild the game starts, this one stays while the cursor stays. For a mod that shows something on the car for as
       long as the player looks at the stage (the Airride lowers the car 3 cm). Any thread. */
    int (*upgrade_preview_stage)(FH3Mod* mod, int handle);
    /* A number that goes up with every step the garage's upgrade list takes on the car (each cursor move shows a stage again). The game
       gives no sign when the player backs out of the list (upgrade_preview_stage stays), so a mod that ends its preview itself (on the
       back button) keeps it ended until this changes: the next hover. */
    uint32_t (*upgrade_shop_serial)(void);

    /* ==== API version 16 (platform 1.2.0): check api_version >= 16 ===== */

    /* ---- Hot reload (a developer feature) ----------------------------------
       With fh3-mods\platform.ini [dev] HotReload=1, the platform loads every mod from a copy, so a rebuilt DLL can replace the
       file while the game runs, and F9 swaps in the DLLs that changed. A mod takes part by exporting

           FH3MOD_EXPORT void fh3mod_shutdown(void);

       which the platform calls (on a thread of its own, never a game thread) before it loads the new build. It has to stop what the
       mod started: its threads (and wait for them to end), timers, anything it patched by itself. The platform switches the mod's
       hooks off after it returns, loads the new DLL and calls its fh3mod_init again. The old DLL stays in memory for good (code
       that is still running in it, or a pointer someone kept, never lands on unmapped memory). A mod without fh3mod_shutdown is
       left running as it is and needs a restart to change.
       During that second fh3mod_init, register_mod returns the same handle, and calls that add rows, pages, hotkeys, commands,
       upgrades or tuning rows with a key that exists already swap the callbacks and `user` pointers of that entry (nothing is
       added twice); the data the game reads at startup (text, files, SQL, the rows' labels, upgrade stages and prices) is
       not applied again: a change to it shows after a restart. is_reloading() tells the second run from the first. */
    int (*is_reloading)(void);   /* 1 while your fh3mod_init runs as a reload, else 0 */
} FH3ModApi;

#define FH3MOD_CAR_MAKE_DRIVEN 1u   /* upgrade_garage_for_car: make the car found the driven car */
#define FH3MOD_CAR_IS_PLAYERS  2u   /* upgrade_garage_for_car: the caller knows this is the car the player drives */

/* One slider row on the tuning menu (API 13). All callbacks run on the game's UI thread: keep them short, don't call
   into the game from them. */
struct FH3TuningRow {
    uint32_t size;                 /* sizeof(FH3TuningRow) */
    const char* key;               /* unique within your mod: letters, digits and '_' */
    const char* label;             /* UTF-8, at the left of the row: "Airride drop (cm)"; "Airride|Drop (cm)" = header band "AIRRIDE", row "Drop", unit cm */
    const char* help;              /* UTF-8, the description shown while the row is selected; may be NULL */
    float min, max, step;          /* the slider's range and step */
    int decimals;                  /* digits after the point in the number at the right */
    float (*get)(void* user);      /* the value to show for the car being tuned */
    void (*set)(void* user, float value);   /* the player moved the slider (already on a step, inside the range) */
    int (*visible)(void* user);    /* 1 = show the row for the car being tuned, 0 = hide it; may be NULL (always shown) */
    void* user;                    /* passed to every callback */
};

#define FH3_TABLE_UPGRADES 0x968719E9u      /* Upgrades.str: Upgrades.Name / Description */
#define FH3_TABLE_UPGRADETYPES 0x07710821u  /* UpgradeTypes.str: UpgradeTypes.Name / Description */

/* Called on the game's UI thread when the player changes a row: keep it
   short (save the value, flip a flag), and don't call back into the game. */
typedef void (*FH3SettingChangedFn)(void* user, const char* key, int value);
/* The same for a slider (API 5). */
typedef void (*FH3SliderChangedFn)(void* user, const char* key, float value);

#define FH3_SETTING_ON_OFF 0
#define FH3_SETTING_SLIDER 1
#define FH3_SETTING_LINK   2   /* API 6 */
#define FH3_SETTING_CHOICE 3   /* API 9 */
#define FH3_SETTING_MAX_CHOICES 32

/* One row on the Mods page (API 4; sliders API 5). */
struct FH3Setting {
    uint32_t size;                 /* sizeof(FH3Setting) */
    const char* key;               /* unique within your mod: letters, digits and '_', like "Trees" */
    const char* label;             /* the row's label, UTF-8: "Breakable trees" */
    const char* description;       /* shown beside the list while the row is selected; may be NULL */
    int value;                     /* current value: 0 = Off, 1 = On */
    int needs_restart;             /* nonzero: a change only takes effect the next time the game starts */
    FH3SettingChangedFn on_change; /* may be NULL */
    void* user;                    /* passed to on_change and on_slide */

    /* ---- API 5 (platform 2.4.0): sliders. Zeroed (as by FH3Setting s{}),
       the row is On/Off and these are ignored. ---- */
    int kind;                      /* FH3_SETTING_ON_OFF or FH3_SETTING_SLIDER */
    float min, max;                /* the slider's range */
    float step;                    /* how far one press of left or right moves it; 0 = a hundredth of the range */
    float slider_value;            /* its current value (`value` is ignored) */
    float display_scale;           /* the number shown is the value times this, so 100 shows 0.12 as 12; 0 = 1 */
    int decimals;                  /* digits shown after the decimal point, 0 to 3 */
    FH3SliderChangedFn on_slide;   /* may be NULL */

    /* ---- API 6 (platform 2.5.0): links. With kind = FH3_SETTING_LINK the row
       opens a web page in the player's browser when they press A on it: your
       dashboard, your docs. It shows on your mod's own page under MODS >
       Installed mods (not on Settings > Mods), with the address under its
       description. value, needs_restart, on_change and the slider fields are
       ignored. On an older platform it would come out as an On/Off row, so
       only add one when api_version >= 6. ---- */
    const char* url;               /* "http://..." or "https://..." */

    /* ---- API 9 (platform 2.8.0): choices. With kind = FH3_SETTING_CHOICE the
       row shows one of `choices` by name, as the game's own option rows do
       ("Speedometer: Analogue / Digital"), and left and right step through
       them. `value` is the index of the one chosen (0 = the first), and
       on_change, get_setting and set_setting use that index. The names are
       copied. The slider fields are ignored. On an older platform it would
       come out as an On/Off row, so only add one when api_version >= 9. ---- */
    const char* const* choices;    /* UTF-8, shown as given: "LB", "RB", "A" ... */
    uint32_t choice_count;         /* 2 to FH3_SETTING_MAX_CHOICES */
};

/* Pages (API 8). Every page callback runs on the game's UI thread: keep them
   short, and don't call into the game from them. */
/* The page is about to be written to be shown: set its rows now if you
   haven't kept them up to date. Opening MODS > Installed mods writes every
   mod's pages, so it's called then too. */
typedef void (*FH3PageOpenFn)(void* user, const char* page_key);
/* The player pressed A on a row that has a key. */
typedef void (*FH3PageSelectFn)(void* user, const char* page_key, const char* row_key);
/* A page of your own design: a button was pressed (FH3_BUTTON_...). Return
   FH3_PAGE_IGNORED for what it usually does (B leaves the page, A does
   nothing, the others whatever the game does with them there), or one of
   the FH3_PAGE_HANDLED values. */
typedef int (*FH3PageButtonFn)(void* user, const char* page_key, int button);
/* A page of your own design: a binding changed slot `slot` to `value`. */
typedef void (*FH3PageSlotFn)(void* user, const char* page_key, int slot, int value);

#define FH3_BUTTON_A  1
#define FH3_BUTTON_B  2
#define FH3_BUTTON_X  3
#define FH3_BUTTON_Y  4
#define FH3_BUTTON_LB 5
#define FH3_BUTTON_RB 6
#define FH3_PAGE_IGNORED        0
#define FH3_PAGE_HANDLED        1   /* nothing else happens */
#define FH3_PAGE_HANDLED_SELECT 2   /* and the menus' select sound plays */
#define FH3_PAGE_HANDLED_BACK   3   /* and the menus' back sound plays */
#define FH3_PAGE_SLOTS 16

struct FH3Page {
    uint32_t size;                 /* sizeof(FH3Page) */
    const char* key;               /* unique within your mod: letters, digits and '_', like "List" */
    const char* title;             /* the page's title, UTF-8: "Achievements" (shown in capitals) */
    const char* label;             /* the row on your mod's page that opens it; NULL = the title */
    const char* description;       /* that row's description; may be NULL */
    /* A pause-menu tile that opens this page instead of what it did on Xbox:
       "Achievements" (the tile on the pause menu and on its Storm Island
       tab). One mod per tile. NULL for none. */
    const char* tile;
    FH3PageOpenFn on_open;         /* may be NULL */
    FH3PageSelectFn on_select;     /* may be NULL: A then does nothing on the page */
    void* user;                    /* passed to every callback */
    /* Pages of your own design (set_page_xaml). Both may be NULL. */
    FH3PageButtonFn on_button;
    FH3PageSlotFn on_slot;
};

/* Title-screen entries (API 9). Called on the game's UI thread when the
   player presses the entry's button on the title screen. Keep it short (set
   a flag, start a thread of your own) and don't call into the game from it.
   Return FH3_TITLE_HANDLED to stay on the title screen, or FH3_TITLE_START
   for the platform to press A for the player straight after: the game then
   signs in, loads the save and loads the world as it always does, and
   title_started_by names this entry. */
typedef int (*FH3TitlePressFn)(void* user, const char* key);
/* The same for an entry with a list (FH3TitleEntry.choices): called when the
   player picks one, with its position in the list from 0. */
typedef int (*FH3TitleChooseFn)(void* user, const char* key, uint32_t choice);

#define FH3_TITLE_HANDLED 0
#define FH3_TITLE_START   1
#define FH3_TITLE_MAX_CHOICES 100

/* One title-screen entry (API 9). */
struct FH3TitleEntry {
    uint32_t size;                 /* sizeof(FH3TitleEntry) */
    const char* key;               /* unique within your mod: letters, digits and '_', like "Storm" */
    const char* label;             /* UTF-8, shown in capitals after its button: "Start on Storm Island"; about 24 characters fit */
    FH3TitlePressFn on_press;      /* may be NULL for an entry with choices (it isn't called then) */
    void* user;                    /* passed to on_press and on_choose */
    int button;                    /* FH3_BUTTON_Y, _X, _RB or _LB to ask for that one; 0 = the next free one */
    /* A list instead of an action: the press shows the game's message box
       over the title screen with these choices and a Back, and picking one
       calls on_choose. The choices are copied. */
    const char* const* choices;    /* UTF-8, shown as given; NULL = no list */
    uint32_t choice_count;         /* 1 to FH3_TITLE_MAX_CHOICES; over 10, shown in pages */
    FH3TitleChooseFn on_choose;
    const char* list_title;        /* the box's title (shown in capitals); NULL = the label */
    const char* list_message;      /* text over the choices; may be NULL */
};

/* Hotkeys (API 9). Modifiers for FH3Hotkey.modifiers: */
#define FH3_KEY_CTRL  1
#define FH3_KEY_SHIFT 2
#define FH3_KEY_ALT   4
/* Controller buttons for FH3Hotkey.pad_chord (XInput's bits): all of them held. */
#define FH3_PAD_DPAD_UP    0x0001
#define FH3_PAD_DPAD_DOWN  0x0002
#define FH3_PAD_DPAD_LEFT  0x0004
#define FH3_PAD_DPAD_RIGHT 0x0008
#define FH3_PAD_START      0x0010
#define FH3_PAD_BACK       0x0020
#define FH3_PAD_LTHUMB     0x0040
#define FH3_PAD_RTHUMB     0x0080
#define FH3_PAD_LB         0x0100
#define FH3_PAD_RB         0x0200
#define FH3_PAD_A          0x1000
#define FH3_PAD_B          0x2000
#define FH3_PAD_X          0x4000
#define FH3_PAD_Y          0x8000

/* Called on a platform thread (not the game's) when the key or chord is
   pressed, once per press. Keep it short, and hand game work to a hook. */
typedef void (*FH3HotkeyFn)(void* user, const char* key);
/* The player picked another binding on the Mods page: save it, and pass it
   in next time (`vk` 0 = no key, `pad_chord` 0 = no chord). On the UI
   thread. */
typedef void (*FH3HotkeyRebindFn)(void* user, const char* key, uint32_t vk, uint32_t modifiers, uint32_t pad_chord);

/* One hotkey (API 9). */
struct FH3Hotkey {
    uint32_t size;                 /* sizeof(FH3Hotkey) */
    const char* key;               /* unique within your mod: letters, digits and '_', like "Mark" */
    const char* label;             /* UTF-8, for its rows on the Mods page: "Mark this spot" */
    uint32_t vk;                   /* the key, a Windows virtual-key code (VK_F9, 'M'); 0 = none */
    uint32_t modifiers;            /* FH3_KEY_CTRL | FH3_KEY_SHIFT | FH3_KEY_ALT, held with it */
    uint32_t pad_chord;            /* FH3_PAD_ bits, held together; 0 = none */
    FH3HotkeyFn on_press;
    FH3HotkeyRebindFn on_rebind;   /* may be NULL */
    void* user;                    /* passed to both */
};

#define FH3_ROW_NORMAL  0
#define FH3_ROW_DIM     1          /* label and value in grey: locked, not available yet */
#define FH3_ROW_HEADING 2          /* label in the game's orange: a heading over the rows after it */

/* One row of a page (API 8). */
struct FH3PageRow {
    uint32_t size;                 /* sizeof(FH3PageRow) */
    const char* key;               /* passed to on_select when A is pressed on it; NULL: A does nothing */
    const char* label;             /* left, UTF-8: "Island Explorer" */
    const char* value;             /* right: "12 / 50"; may be NULL */
    const char* description;       /* beside the list while the row is selected; may be NULL */
    const char* footer;            /* a line at the bottom of the description panel; may be NULL */
    int style;                     /* FH3_ROW_NORMAL, FH3_ROW_DIM or FH3_ROW_HEADING */
    int value_alert;               /* nonzero: the value in red */
};

/* What a mod exports. */
typedef int (*FH3ModInitFn)(const FH3ModApi* api);
typedef void (*FH3ModShutdownFn)(void);   /* optional, API 16: lets the mod be hot-reloaded (see is_reloading) */

#ifdef __cplusplus
}
#endif

#endif /* FH3MOD_H */
