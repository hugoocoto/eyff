// eyff - explore Your F*cking Files
// Copyright (C) 2026 Hugo Coto
// SPDX-License-Identifier: GPL-3.0-or-later

// SetLoadFileDataCallback
// SetSaveFileDataCallback
// SetLoadFileTextCallback
// SetSaveFileTextCallback
// FileRename
// FileRemove
// FileCopy
// FileMove
// FileTextReplace
// FileTextFindIndex
// FileExists
// DirectoryExists
// IsFileExtension
// IsFileHidden
// GetFileLength
// GetFileModTime
// GetFileExtension
// GetFileName
// GetFileNameWithoutExt
// GetDirectoryPath
// GetPrevDirectoryPath
// GetWorkingDirectory
// GetApplicationDirectory
// MakeDirectory
// ChangeDirectory
// IsPathFile
// IsPathDirectory
// IsPathAbsolute
// IsFileNameValid
// LoadDirectoryFiles
// LoadDirectoryFilesEx
// UnloadDirectoryFiles
// IsFileDropped
// LoadDroppedFiles
// UnloadDroppedFiles
// GetDirectoryFileCount
// GetDirectoryFileCountEx

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <fontconfig/fontconfig.h>
#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lua_compat.h" // before conf.h
#include "raylib.h"

#define INCLUDE_CONF_IMPLEMENTATION
#include "conf.h"
#include "cum.h"
#include "flag.h"

// Both come from the Makefile: NAME from the project, VERSION from git
#ifndef NAME
#define NAME "eyff"
#endif
#ifndef VERSION
#define VERSION "unknown"
#endif

// What the config file sets. Add a field for each setting, with its default
// and its Conf_get_* in load_config, and free what it owns in config_free.
typedef struct {
        int width;
        int height;
        const char *title;
        int font_size;
        char *font_name;
        int font_spacing;
} Config;

Config config = {
        .width = 800,
        .height = 600,
        .title = "Title",
        .font_size = 30,
        .font_name = "Iosevka NFM",
        .font_spacing = 0,
};

typedef struct Grid {
        int rows, cols;
} Grid;

typedef struct Entry Entry;
typedef Da(struct Entry *) Entry_List;

typedef enum Sort_Type {
        SORT_NONE,
        SORT_ALPHA,
} Sort_Type;

typedef enum Filters {
        FILTER_NONE = 0,
        FILTER_IGNORE_DOTS = 1 << 0,
} Filters;

// globals
static struct {
        Font font;
        Rectangle usable;
        Vector2 csize;
        Grid grid;
        Entry *root;
        Entry_List entry_list;
        Sort_Type entry_list_sort_method;
        Filters entry_list_filters;
        int cursor;
        bool list_need_refresh;
} G = {
        .entry_list_sort_method = SORT_ALPHA,
        .entry_list_filters = 0 | FILTER_IGNORE_DOTS,
};

struct Entry {
        const char *path;
        Entry *parent;
        Entry_List files; // just for directories
        int nesting_level;

        unsigned char is_dir      : 1;
        unsigned char is_extended : 1;
};

Entry *
entry_create(const char *path, Entry *parent, bool extend, int nesting_level)
{
        Entry *e = calloc(1, sizeof(Entry));
        assert(e != NULL);
        e->path = strdup(path);
        e->is_extended = extend;
        e->is_dir = IsPathDirectory(path);
        e->parent = parent;
        e->nesting_level = nesting_level;
        return e;
}

Entry_List
entry_flatten(Entry *e)
{
        Entry_List l = { 0 };
        Da_append(&l, e);
        if (e->is_dir && e->is_extended) {
                Da_foreach(f, e->files)
                {
                        Entry_List li = entry_flatten(*f);
                        Da_extend(&l, li);
                        Da_destroy(&li);
                }
        }
        return l;
}

void
cursor_trim()
{
        if (G.cursor >= G.entry_list.count) G.cursor = G.entry_list.count - 1;
        if (G.cursor < 0) G.cursor = 0;
}

char *
font_find_by_name(const char *name)
{
        FcChar8 *route = 0;
        FcPattern *match = 0;
        FcPattern *pattern = 0;
        FcResult result;
        char *sroute = 0;

        if (!name) return NULL;

        if (!FcInit()) {
                printf("Error: Fontconfig FcInit fails\n");
                return NULL;
        }

        if ((pattern = FcNameParse((const FcChar8 *) name)) == NULL) {
                goto free_resources;
        }

        FcConfigSubstitute(NULL, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);

        if ((match = FcFontMatch(NULL, pattern, &result)) == NULL) {
                goto free_resources;
        }

        if (FcPatternGetString(match, FC_FILE, 0, &route) == FcResultMatch) {
                sroute = strdup((char *) route);
        }

free_resources:
        FcPatternDestroy(0); // assert that it is not going to explode on error
        FcPatternDestroy(pattern);
        FcPatternDestroy(match);
        FcFini();
        return sroute;
}

void
Write(const char *text, int cw, int ch, Color tint)
{
        DrawTextEx(G.font, text, (Vector2) { G.csize.x * cw, G.csize.y * ch },
                   config.font_size, config.font_spacing, tint);
}

void
open_directory(Entry *entry)
{
        static int nesting_level = 0;
        assert(IsPathDirectory(entry->path));

        FilePathList list = LoadDirectoryFiles(entry->path);
        for (unsigned int i = 0; i < list.count; i++) {
                Entry *e = entry_create(list.paths[i], entry, false, nesting_level);
                if (e->is_dir) {
                        ++nesting_level;
                        open_directory(e);
                        --nesting_level;
                }
                Da_append(&entry->files, e);
        }
}

void
entry_list_print(Entry_List list, int cursor)
{
        int tabsize = 2;
        for (int i = 0; i < list.count; i++) {
                Entry *e = list.items[i];
                Color color = WHITE;

                if (i == cursor) {
                        DrawRectangle(G.csize.x * e->nesting_level * tabsize, G.csize.y * i, G.csize.x, config.font_size, LIGHTGRAY);
                }

                Write(GetFileName(e->path), e->nesting_level * tabsize, i, color);

                if (e->is_dir) {
                        Write("/", e->nesting_level * tabsize + strlen(GetFileName(e->path)), i, color);
                }
        }
}

int
entry_cmp(const void *a, const void *b)
{
        return strcmp((*(Entry **) a)->path, (*(Entry **) b)->path);
}

void
filter(Entry_List *list, bool(f)(Entry *))
{
        for (int i = 0; i < list->count; i++) {
                if (f(list->items[i]) == false) {
                        Da_remove(list, i);
                        i--;
                }
        }
}

// true if pass the filter
bool
entry_list_filter_dots(Entry *e)
{
        return GetFileName(e->path)[0] != '.';
}

void
entry_list_sort_by_name(Entry_List *list)
{
        qsort(list->items, list->count, sizeof(*list->items), entry_cmp);
}

void
refresh_global_entry_list()
{
        if (G.entry_list.capacity > 0) Da_destroy(&G.entry_list);
        G.entry_list = entry_flatten(G.root);

        if (G.entry_list_sort_method == SORT_ALPHA) entry_list_sort_by_name(&G.entry_list);
        if (G.entry_list_filters & FILTER_IGNORE_DOTS) filter(&G.entry_list, entry_list_filter_dots);

        G.list_need_refresh = false;
        cursor_trim();
}

void
entry_visibility_show(Entry *e)
{
        if (e->is_dir && !e->is_extended) {
                e->is_extended = true;
                G.list_need_refresh = true;
        }
}

void
entry_visibility_hide(Entry *e)
{
        // maybe this should turn off all the subdirs '->extend
        if (e->is_dir && e->is_extended) {
                e->is_extended = false;
                G.list_need_refresh = true;
        }
}

void
entry_toggle_visibility(Entry *e)
{
        // just for fun
        (e->is_extended ? entry_visibility_hide : entry_visibility_show)(e);
}

// clang-format off
typedef enum {
NUL=0, SOH, STX, ETX, EOT, ENQ, ACK, BEL, BS   , HT     , LF , VT ,
FF   , CR , SO , SI , DLE, DC1, DC2, DC3, DC4  , NAK    , SYN, ETB,
CAN  , EM , SUB, ESC, FS , GS , RS , US , SPACE, DEL=127,
} ControlChar;
// clang-format on

const char WITHOUT_SHIFT[128] = {
        [0x00] = NUL, /* '\0' (null character)   */
        [0x01] = SOH, /* (start of heading)      */
        [0x02] = STX, /* (start of text)         */
        [0x03] = ETX, /* (end of text)           */
        [0x04] = EOT, /* (end of transmission)   */
        [0x05] = ENQ, /* (enquiry)               */
        [0x06] = ACK, /* (acknowledge)           */
        [0x07] = BEL, /* '\a' (bell)             */
        [0x08] = BS,  /* '\b' (backspace)        */
        [0x09] = HT,  /* '\t' (horizontal tab)   */
        [0x0A] = LF,  /* '\n' (new line)         */
        [0x0B] = VT,  /* '\v' (vertical tab)     */
        [0x0C] = FF,  /* '\f' (form feed)        */
        [0x0D] = CR,  /* '\r' (carriage ret)     */
        [0x0E] = SO,  /* (shift out)             */
        [0x0F] = SI,  /* (shift in)              */
        [0x10] = DLE, /* (data link escape)      */
        [0x11] = DC1, /* (device control 1)      */
        [0x12] = DC2, /* (device control 2)      */
        [0x13] = DC3, /* (device control 3)      */
        [0x14] = DC4, /* (device control 4)      */
        [0x15] = NAK, /* (negative ack.)         */
        [0x16] = SYN, /* (synchronous idle)      */
        [0x17] = ETB, /* (end of trans. blk)     */
        [0x18] = CAN, /* (cancel)                */
        [0x19] = EM,  /* (end of medium)         */
        [0x1A] = SUB, /* (substitute)            */
        [0x1B] = ESC, /* (escape)                */
        [0x1C] = FS,  /* (file separator)        */
        [0x1D] = GS,  /* (group separator)       */
        [0x1E] = RS,  /* (record separator)      */
        [0x1F] = US,  /* (unit separator)        */
        [0x20] = SPACE,
        [0x21] = '!',
        [0x22] = '"',
        [0x23] = '#',
        [0x24] = '$',
        [0x25] = '%',
        [0x26] = '&',
        [0x27] = '\'',
        [0x28] = '(',
        [0x29] = ')',
        [0x2A] = '*',
        [0x2B] = '+',
        [0x2C] = ',',
        [0x2D] = '-',
        [0x2E] = '.',
        [0x2F] = '/',
        [0x30] = '0',
        [0x31] = '1',
        [0x32] = '2',
        [0x33] = '3',
        [0x34] = '4',
        [0x35] = '5',
        [0x36] = '6',
        [0x37] = '7',
        [0x38] = '8',
        [0x39] = '9',
        [0x3A] = ':',
        [0x3B] = ';',
        [0x3C] = '<',
        [0x3D] = '=',
        [0x3E] = '>',
        [0x3F] = '?',
        [0x40] = '@',
        [0x41] = 'a',
        [0x42] = 'b',
        [0x43] = 'c',
        [0x44] = 'd',
        [0x45] = 'e',
        [0x46] = 'f',
        [0x47] = 'g',
        [0x48] = 'h',
        [0x49] = 'i',
        [0x4A] = 'j',
        [0x4B] = 'k',
        [0x4C] = 'l',
        [0x4D] = 'm',
        [0x4E] = 'n',
        [0x4F] = 'o',
        [0x50] = 'p',
        [0x51] = 'q',
        [0x52] = 'r',
        [0x53] = 's',
        [0x54] = 't',
        [0x55] = 'u',
        [0x56] = 'v',
        [0x57] = 'w',
        [0x58] = 'x',
        [0x59] = 'y',
        [0x5A] = 'z',
        [0x5B] = '[',
        [0x5C] = '\\',
        [0x5D] = ']',
        [0x5E] = '^',
        [0x5F] = '_',
        [0x60] = '`',
        [0x61] = 'a',
        [0x62] = 'b',
        [0x63] = 'c',
        [0x64] = 'd',
        [0x65] = 'e',
        [0x66] = 'f',
        [0x67] = 'g',
        [0x68] = 'h',
        [0x69] = 'i',
        [0x6A] = 'j',
        [0x6B] = 'k',
        [0x6C] = 'l',
        [0x6D] = 'm',
        [0x6E] = 'n',
        [0x6F] = 'o',
        [0x70] = 'p',
        [0x71] = 'q',
        [0x72] = 'r',
        [0x73] = 's',
        [0x74] = 't',
        [0x75] = 'u',
        [0x76] = 'v',
        [0x77] = 'w',
        [0x78] = 'x',
        [0x79] = 'y',
        [0x7A] = 'z',
        [0x7B] = '{',
        [0x7C] = '|',
        [0x7D] = '}',
        [0x7E] = '~',
        [0x7F] = DEL,
};

const char WITH_SHIFT[128] = {
        [0x00] = NUL, /* '\0' (null character)   */
        [0x01] = SOH, /* (start of heading)      */
        [0x02] = STX, /* (start of text)         */
        [0x03] = ETX, /* (end of text)           */
        [0x04] = EOT, /* (end of transmission)   */
        [0x05] = ENQ, /* (enquiry)               */
        [0x06] = ACK, /* (acknowledge)           */
        [0x07] = BEL, /* '\a' (bell)             */
        [0x08] = BS,  /* '\b' (backspace)        */
        [0x09] = HT,  /* '\t' (horizontal tab)   */
        [0x0A] = LF,  /* '\n' (new line)         */
        [0x0B] = VT,  /* '\v' (vertical tab)     */
        [0x0C] = FF,  /* '\f' (form feed)        */
        [0x0D] = CR,  /* '\r' (carriage ret)     */
        [0x0E] = SO,  /* (shift out)             */
        [0x0F] = SI,  /* (shift in)              */
        [0x10] = DLE, /* (data link escape)      */
        [0x11] = DC1, /* (device control 1)      */
        [0x12] = DC2, /* (device control 2)      */
        [0x13] = DC3, /* (device control 3)      */
        [0x14] = DC4, /* (device control 4)      */
        [0x15] = NAK, /* (negative ack.)         */
        [0x16] = SYN, /* (synchronous idle)      */
        [0x17] = ETB, /* (end of trans. blk)     */
        [0x18] = CAN, /* (cancel)                */
        [0x19] = EM,  /* (end of medium)         */
        [0x1A] = SUB, /* (substitute)            */
        [0x1B] = ESC, /* (escape)                */
        [0x1C] = FS,  /* (file separator)        */
        [0x1D] = GS,  /* (group separator)       */
        [0x1E] = RS,  /* (record separator)      */
        [0x1F] = US,  /* (unit separator)        */
        [0x20] = SPACE,
        [0x21] = '!',
        [0x22] = '"',
        [0x23] = '#',
        [0x24] = '$',
        [0x25] = '%',
        [0x26] = '&',
        [0x27] = '"',
        [0x28] = '(',
        [0x29] = ')',
        [0x2A] = '*',
        [0x2B] = '+',
        [0x2C] = '<',
        [0x2D] = '_',
        [0x2E] = '>',
        [0x2F] = '?',
        [0x30] = ')',
        [0x31] = '!',
        [0x32] = '@',
        [0x33] = '#',
        [0x34] = '$',
        [0x35] = '%',
        [0x36] = '^',
        [0x37] = '&',
        [0x38] = '*',
        [0x39] = '(',
        [0x3A] = ':',
        [0x3B] = ':',
        [0x3C] = '<',
        [0x3D] = '+',
        [0x3E] = '>',
        [0x3F] = '?',
        [0x40] = '@',
        [0x41] = 'A',
        [0x42] = 'B',
        [0x43] = 'C',
        [0x44] = 'D',
        [0x45] = 'E',
        [0x46] = 'F',
        [0x47] = 'G',
        [0x48] = 'H',
        [0x49] = 'I',
        [0x4A] = 'J',
        [0x4B] = 'K',
        [0x4C] = 'L',
        [0x4D] = 'M',
        [0x4E] = 'N',
        [0x4F] = 'O',
        [0x50] = 'P',
        [0x51] = 'Q',
        [0x52] = 'R',
        [0x53] = 'S',
        [0x54] = 'T',
        [0x55] = 'U',
        [0x56] = 'V',
        [0x57] = 'W',
        [0x58] = 'X',
        [0x59] = 'Y',
        [0x5A] = 'Z',
        [0x5B] = '{',
        [0x5C] = '|',
        [0x5D] = '}',
        [0x5E] = '^',
        [0x5F] = '_',
        [0x60] = '`',
        [0x61] = 'a',
        [0x62] = 'b',
        [0x63] = 'c',
        [0x64] = 'd',
        [0x65] = 'e',
        [0x66] = 'f',
        [0x67] = 'g',
        [0x68] = 'h',
        [0x69] = 'i',
        [0x6A] = 'j',
        [0x6B] = 'k',
        [0x6C] = 'l',
        [0x6D] = 'm',
        [0x6E] = 'n',
        [0x6F] = 'o',
        [0x70] = 'p',
        [0x71] = 'q',
        [0x72] = 'r',
        [0x73] = 's',
        [0x74] = 't',
        [0x75] = 'u',
        [0x76] = 'v',
        [0x77] = 'w',
        [0x78] = 'x',
        [0x79] = 'y',
        [0x7A] = 'z',
        [0x7B] = '{',
        [0x7C] = '|',
        [0x7D] = '}',
        [0x7E] = '~',
        [0x7F] = DEL,
};

int
using_no_shift(int kp)
{
        if (0 <= kp && kp <= 128)
                return WITHOUT_SHIFT[kp];
        return kp;
}

int
using_shift(int kp)
{
        if (0 <= kp && kp <= 128)
                return WITH_SHIFT[kp];
        return kp;
}

// open cwd on null
void
explorer(const char *path)
{
        G.root = entry_create(path ?: GetWorkingDirectory(), NULL, true, 0);
        open_directory(G.root);

        refresh_global_entry_list();

        SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
        InitWindow(config.width, config.height, config.title);

        // Load config.font_name or 'monospace' or raylib default
        G.font = LoadFontEx(font_find_by_name(config.font_name) ?: font_find_by_name("monospace"),
                            config.font_size, NULL, 0);

        if (!IsFontValid(G.font)) {
                G.font = GetFontDefault();
        }
        config.font_size = G.font.baseSize;

        bool first = true;
        G.cursor = 0;

        bool has_ctrl = IsKeyPressed(KEY_LEFT_CONTROL) || IsKeyPressed(KEY_RIGHT_CONTROL);
        bool has_shift = IsKeyPressed(KEY_LEFT_SHIFT) || IsKeyPressed(KEY_RIGHT_SHIFT);

        while (!WindowShouldClose()) {
                if (first || IsWindowResized()) {
                        G.csize = MeasureTextEx(G.font, " ", config.font_size, config.font_spacing);

                        G.usable.x = 0;
                        G.usable.y = 0;
                        G.usable.width = GetRenderWidth();
                        G.usable.height = GetRenderHeight();

                        G.grid.cols = G.csize.x > 0 ? (int) ((G.usable.width - G.usable.x) / G.csize.x) : 0;
                        G.grid.rows = G.csize.y > 0 ? (int) ((G.usable.height - G.usable.y) / G.csize.y) : 0;
                        first = false;
                }

                // clang-format off
                if (has_ctrl   && IsKeyUp(KEY_LEFT_CONTROL)    && IsKeyUp(KEY_RIGHT_CONTROL))    has_ctrl  = false;
                if (!has_ctrl  && (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))) has_ctrl  = true;
                if (has_shift  && IsKeyUp(KEY_LEFT_SHIFT)      && IsKeyUp(KEY_RIGHT_CONTROL))    has_shift = false;
                if (!has_shift && (IsKeyDown(KEY_LEFT_SHIFT)   || IsKeyDown(KEY_RIGHT_CONTROL))) has_shift = true;
                // clang-format on

                int key;

                while ((key = GetKeyPressed())) {
                        if (0) {
                        }

                        // J, C-n or Down
                        else if (key == 'J' || (has_ctrl && key == 'N') || key == KEY_DOWN) {
                                ++G.cursor;
                                cursor_trim();
                        }

                        // k, C-p or Up
                        else if (key == 'K' || (has_ctrl && key == 'P') || key == KEY_UP) {
                                --G.cursor;
                                cursor_trim();
                        }

                        else if (key == KEY_ENTER) {
                                printf("ENTER on %s\n", G.entry_list.items[G.cursor]->path);
                        }

                        else if (key == KEY_SPACE) {
                                printf("SPACE on %s\n", G.entry_list.items[G.cursor]->path);
                                entry_toggle_visibility(G.entry_list.items[G.cursor]);
                        }

                        else if (has_shift && key == '.') { // '>'
                                // opens the dir
                                entry_visibility_show(G.entry_list.items[G.cursor]);
                        }

                        else if (!has_shift && key == '.') {
                                G.entry_list_filters ^= FILTER_IGNORE_DOTS;
                                G.list_need_refresh = true;
                        }

                        else if (has_shift && key == ',') { // '<'
                                // closes the parent dir or current if the
                                // cursor is over a directory and it's open
                                if (G.entry_list.items[G.cursor]->is_dir &&
                                    G.entry_list.items[G.cursor]->is_extended) {
                                        entry_visibility_hide(G.entry_list.items[G.cursor]);
                                }

                                else {
                                        // search for the parent index
                                        int i = 0;
                                        for (; i < G.entry_list.count; i++) {
                                                if (G.entry_list.items[i] == G.entry_list.items[G.cursor]->parent) {
                                                        G.cursor = i;
                                                        entry_visibility_hide(G.entry_list.items[i]);
                                                        break;
                                                }
                                        }
                                }

                        }

                        else {
                                key = has_shift ? using_shift(key) : using_no_shift(key);

                                if (isprint(key)) {
                                        printf("Unknown key press %d (%c)\n", key, key);
                                } else {
                                        printf("Unknown key press %d\n", key);
                                }
                        }
                }

                BeginDrawing();
                ClearBackground(BLACK);

                if (G.list_need_refresh) refresh_global_entry_list();
                entry_list_print(G.entry_list, G.cursor);

                // window bounds for debugging
                DrawRectangleLines(0, 0, G.grid.cols * G.csize.x, G.grid.rows * G.csize.y, RED);

                EndDrawing();
        }
}

static void
config_free()
{
}

// Returns the user's config file, $XDG_CONFIG_HOME/NAME/config.lua (default
// ~/.config/NAME/config.lua), or NULL if it doesn't exist. Never one in the
// current dir: the config is Lua with its whole standard library, so running
// the program inside a downloaded dir would run that dir's code. The result
// must be freed.
static char *
find_config(void)
{
        char path[4096];
        const char *xdg = getenv("XDG_CONFIG_HOME");
        const char *home = getenv("HOME");

        if (xdg && *xdg) {
                snprintf(path, sizeof path, "%s/" NAME "/config.lua", xdg);
        } else if (home && *home) {
                snprintf(path, sizeof path, "%s/.config/" NAME "/config.lua", home);
        } else {
                path[0] = '\0';
        }

        if (*path && access(path, R_OK) == 0) return strdup(path);
        return NULL;
}

// Fill CONFIG from PATH, or from find_config() if PATH is NULL. Keys missing
// from the file keep their defaults, and no file at all is not an error.
// Returns 0 on success.
static int
load_config(const char *path)
{
        Conf conf = NULL;
        char *found = NULL;

        if (path == NULL && (path = found = find_config()) == NULL) return 0;

        if (Conf_open(&conf, path) != CONF_OK) {
                fprintf(stderr, NAME ": can't load %s\n", path);
                free(found);
                return 1;
        }

        Conf_get_int(conf, &config.width, "Config.window.width");
        Conf_get_int(conf, &config.height, "Config.window.height");
        Conf_get_str(conf, &config.title, "Config.window.title");

        Conf_close(conf);
        free(found);
        return 0;
}

int
main(int argc, char **argv)
{
        const char *show_version;
        const char *config_path;

        // .name: the usage line says NAME however it's run, so it's the same
        // as in the README (test/docs.sh checks it)
        flag_program(.name = NAME, .help = "Explore Your F*cking Files");
        flag_add(&show_version, "--version", "-v", .help = "Show version and exit");
        flag_add(&config_path, "--config", "-c", .nargs = 1, .help = "Use this config file");

        if (flag_parse(&argc, &argv) || argc > 1) {
                flag_show_help(STDERR_FILENO);
                flag_free();
                return 1;
        }

        if (show_version) {
                printf(NAME " %s\n", VERSION);
                flag_free();
                return 0;
        }

        if (load_config(config_path)) {
                config_free();
                flag_free();
                return 1;
        }

        explorer(0);

        config_free();
        flag_free();
        return 0;
}
