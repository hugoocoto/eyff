// eyff - explore your f*cking files
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
        char *title; // NULL until the config file sets it, "Title" otherwise
        int font_size;
        char *font_name;
        int font_spacing;
} Config;

Config config = {
        .width = 800,
        .height = 600,
        .title = NULL,
        .font_size = 33,
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
        Vector2 char_dimension;

        Rectangle filetree;
        Grid filetree_c;
        Rectangle preview;

        Entry *root;
        Entry_List entry_list;
        Sort_Type entry_list_sort_method;
        Filters entry_list_filters;

        int cursor;

        bool list_need_refresh;
        bool lazy;
} g = {
        .entry_list_sort_method = SORT_ALPHA,
        .entry_list_filters = 0 | FILTER_IGNORE_DOTS,
        .lazy = true, // lazy load directory contents
};

typedef enum Preview_State {
        PREVIEW_NONE = 0, // not looked at yet
        PREVIEW_READY,    // texture holds the image
        PREVIEW_FAILED,   // not an image, or it didn't decode
} Preview_State;

struct Entry {
        const char *path;
        Entry *parent;
        Entry_List files; // just for directories
        int nesting_level;
        Texture2D texture; // for previews
        Preview_State preview;

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
        if (g.cursor >= g.entry_list.count) g.cursor = g.entry_list.count - 1;
        if (g.cursor < 0) g.cursor = 0;
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
        FcPatternDestroy(0); // assert that this is not going to break on error
        FcPatternDestroy(pattern);
        FcPatternDestroy(match);
        FcFini();
        return sroute;
}

void
Write(const char *text, int cw, int ch, Color tint)
{
        DrawTextEx(g.font, text, (Vector2) { g.filetree.x + g.char_dimension.x * cw, g.filetree.y + g.char_dimension.y * ch },
                   config.font_size, config.font_spacing, tint);
}

void
open_directory(Entry *entry)
{
        printf("Open directory %s\n", entry->path);
        assert(IsPathDirectory(entry->path));

        FilePathList list = LoadDirectoryFiles(entry->path);
        for (unsigned int i = 0; i < list.count; i++) {
                Entry *e = entry_create(list.paths[i], entry, false, entry->nesting_level + 1);
                if (e->is_dir && !g.lazy) {
                        open_directory(e);
                }
                Da_append(&entry->files, e);
        }
}

void
entry_list_print(Entry_List list, int cursor)
{
        int tabsize = 2;
        int tabstart = list.items[0]->nesting_level;

        int items_per_page = g.filetree_c.rows;
        int page = cursor / items_per_page;
        int page_start = page * items_per_page;
        int page_end = Min((page + 1) * items_per_page, list.count);

        // Long names would paint over the preview
        BeginScissorMode((int) g.filetree.x, (int) g.filetree.y,
                         (int) g.filetree.width, (int) g.filetree.height);

        for (int i = page_start; i < page_end; i++) {
                Entry *e = list.items[i];
                int cx = (e->nesting_level - tabstart) * tabsize;
                Color color = WHITE;
                int cy = i - page_start;

                if (i == cursor) {
                        DrawRectangle(g.filetree.x + g.char_dimension.x * cx,
                                      g.filetree.y + g.char_dimension.y * cy,
                                      g.char_dimension.x, config.font_size, LIGHTGRAY);
                }

                Write(GetFileName(e->path), cx, cy, color);

                if (e->is_dir) {
                        Write("/", cx + strlen(GetFileName(e->path)), cy, color);
                }
        }

        EndScissorMode();
}

int
entry_cmp(const void *a, const void *b)
{
        return strcmp((*(Entry **) a)->path, (*(Entry **) b)->path);
}

void
filter(Entry_List *list, bool(f)(Entry *))
{
        // start at 1 because the first entry is the root path so there is no
        // reason to hide it
        for (int i = 1; i < list->count; i++) {
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
        if (g.entry_list.capacity > 0) Da_destroy(&g.entry_list);
        g.entry_list = entry_flatten(g.root);

        if (g.entry_list_sort_method == SORT_ALPHA) entry_list_sort_by_name(&g.entry_list);
        if (g.entry_list_filters & FILTER_IGNORE_DOTS) filter(&g.entry_list, entry_list_filter_dots);

        g.list_need_refresh = false;
        cursor_trim();
}

void
entry_visibility_show(Entry *e)
{
        if (e->is_dir && !e->is_extended) {
                if (g.lazy && e->files.count == 0 && GetDirectoryFileCount(e->path) > 0) {
                        open_directory(e);
                }
                e->is_extended = true;
                g.list_need_refresh = true;
        }
}

void
entry_visibility_hide(Entry *e)
{
        // maybe this should turn off all the subdirs '->extend
        if (e->is_dir && e->is_extended) {
                e->is_extended = false;
                g.list_need_refresh = true;
        }
}

void
entry_toggle_visibility(Entry *e)
{
        // just for fun
        (e->is_extended ? entry_visibility_hide : entry_visibility_show)(e);
}

// clang-format off
enum {
        NUL=0, SOH, STX, ETX, EOT, ENQ, ACK, BEL, BS   , HT     , LF , VT ,
        FF   , CR , SO , SI , DLE, DC1, DC2, DC3, DC4  , NAK    , SYN, ETB,
        CAN  , EM , SUB, ESC, FS , GS , RS , US , SPACE, DEL=127,
};
// clang-format on

// +---------------------------------------------------------------+
// | keyboard layout: This might only work for US qwerty. UNTESTED |
// +---------------------------------------------------------------+

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
        if (0 <= kp && kp < 128)
                return WITHOUT_SHIFT[kp];
        return kp;
}

int
using_shift(int kp)
{
        if (0 <= kp && kp < 128)
                return WITH_SHIFT[kp];
        return kp;
}

void
change_root(Entry *new)
{
        g.root = new;
        g.cursor = 0;
        entry_visibility_show(g.root);
        g.list_need_refresh = true;
}

bool
entry_looks_like_a_file(Entry *e)
{
        // What raylib can decode. IsFileExtension is case-insensitive and takes
        // a semicolon separated list.
        return IsFileExtension(e->path, ".png;.jpg;.jpeg;.bmp;.tga;.gif;.qoi;.psd;.hdr;.pnm");
}

// A preview never needs more pixels than this on its longest side, and a huge
// image would otherwise take a lot of GPU memory for nothing.
#define PREVIEW_MAX_DIM 4096

// Load and upload the preview of E once. E->preview tells whether it's worth
// trying, so this is safe to call every frame.
void
preview_ensure(Entry *e)
{
        if (e->is_dir || e->preview != PREVIEW_NONE) return;

        if (!entry_looks_like_a_file(e)) {
                e->preview = PREVIEW_FAILED;
                return;
        }

        Image img = LoadImage(e->path);
        if (!IsImageValid(img)) {
                e->preview = PREVIEW_FAILED;
                return;
        }

        int longest = Max(img.width, img.height);
        if (longest > PREVIEW_MAX_DIM) {
                float scale = (float) PREVIEW_MAX_DIM / (float) longest;
                // Max(1, ...) because a rounded down side would make the image
                // invalid, and ImageResize would then refuse to do anything
                ImageResize(&img, Max(1, (int) (img.width * scale + 0.5f)),
                            Max(1, (int) (img.height * scale + 0.5f)));
        }

        e->texture = LoadTextureFromImage(img);
        UnloadImage(img); // the texture has its own copy now

        if (!IsTextureValid(e->texture)) {
                e->preview = PREVIEW_FAILED;
                return;
        }

        SetTextureFilter(e->texture, TEXTURE_FILTER_BILINEAR);
        e->preview = PREVIEW_READY;
}

void
preview_print(Entry *e)
{
        if (e->preview != PREVIEW_READY) return;

        // Fit the texture in the preview rect keeping its aspect ratio, and
        // center it. Scaling at draw time (instead of resizing the image) means
        // a window resize doesn't reload anything, and small images keep their
        // real size instead of being blown up.
        float scale = Min(g.preview.width / (float) e->texture.width,
                          g.preview.height / (float) e->texture.height);
        if (scale > 1.0f) scale = 1.0f;

        float w = e->texture.width * scale;
        float h = e->texture.height * scale;

        DrawTexturePro(e->texture,
                       (Rectangle) { 0, 0, (float) e->texture.width, (float) e->texture.height },
                       (Rectangle) { g.preview.x + (g.preview.width - w) / 2,
                                     g.preview.y + (g.preview.height - h) / 2,
                                     w, h },
                       (Vector2) { 0, 0 }, 0.0f, WHITE);
}

// open cwd on null
void
explorer(const char *path)
{
        g.root = entry_create(path ?: GetWorkingDirectory(), NULL, true, 0);
        open_directory(g.root);

        refresh_global_entry_list();

        // before InitWindow, or its startup logs still show
        SetTraceLogLevel(LOG_WARNING);
        SetConfigFlags(FLAG_WINDOW_RESIZABLE |  // Set to allow resizable window
                       FLAG_WINDOW_TOPMOST |    // Set to window always on top
                       FLAG_WINDOW_HIGHDPI |    // Set to support HighDPI
                       FLAG_WINDOW_ALWAYS_RUN | // Set to allow windows running while minimized
                       FLAG_VSYNC_HINT);        // Set to try enabling V-Sync on GPU
        InitWindow(config.width, config.height, config.title ?: "Title");

        // Load config.font_name or 'monospace' or raylib default
        g.font = LoadFontEx(font_find_by_name(config.font_name) ?: font_find_by_name("monospace"),
                            config.font_size, NULL, 0);

        if (!IsFontValid(g.font)) {
                g.font = GetFontDefault();
        }
        config.font_size = g.font.baseSize;

        bool first = true;
        g.cursor = 0;

        bool has_ctrl = IsKeyPressed(KEY_LEFT_CONTROL) || IsKeyPressed(KEY_RIGHT_CONTROL);
        bool has_shift = IsKeyPressed(KEY_LEFT_SHIFT) || IsKeyPressed(KEY_RIGHT_SHIFT);
        bool is_layout_changed = false;
        enum {
                LAYOUT_FTT_FPB = 0,     // File tree at the top, file preview at the bottom
                LAYOUT_FTL_FPR = 1,     // File tree at the left, file preview at the right
                LAYOUT_INTELLIGENT = 2, // Use the wider setup
        } layout = 2;

        while (!WindowShouldClose()) {
                if (first || IsWindowResized() || is_layout_changed) {
                        g.char_dimension = MeasureTextEx(g.font, " ", config.font_size, config.font_spacing);
                        int marginx = g.char_dimension.x * 1;
                        int marginy = g.char_dimension.y;
                        int layout_cpy = layout;

                repeat_switch:
                        switch (layout_cpy) {
                        case LAYOUT_FTT_FPB:
                                g.filetree.x = marginx;
                                g.filetree.y = marginy;
                                g.filetree.width = (GetRenderWidth() - marginx) - marginx;
                                g.filetree.height = (GetRenderHeight() - 3 * g.filetree.y) / 2;

                                g.preview.x = marginx;
                                g.preview.y = g.filetree.y + g.filetree.height + marginy;
                                g.preview.width = (GetRenderWidth() - marginx) - marginx;
                                g.preview.height = (GetRenderHeight() - g.preview.y) - marginy;
                                break;

                        case LAYOUT_FTL_FPR:
                                g.filetree.y = marginy;
                                g.filetree.x = marginx;
                                g.filetree.height = (GetRenderHeight() - marginy) - marginy;
                                g.filetree.width = (GetRenderWidth() - 3 * g.filetree.x) / 2;

                                g.preview.y = marginy;
                                g.preview.x = g.filetree.x + g.filetree.width + marginx;
                                g.preview.height = (GetRenderHeight() - marginy) - marginy;
                                g.preview.width = (GetRenderWidth() - g.preview.x) - marginx;

                                break;

                        case LAYOUT_INTELLIGENT:
                                layout_cpy = (GetRenderHeight() > GetRenderWidth()) ? LAYOUT_FTT_FPB :
                                                                                      LAYOUT_FTL_FPR;
                                goto repeat_switch;
                        }

                        // auto calculated
                        g.filetree_c.cols = Max(1, (int) (g.filetree.width / g.char_dimension.x));
                        g.filetree_c.rows = Max(1, (int) (g.filetree.height / g.char_dimension.y));

                        first = false;
                }

                // clang-format off
                if (has_ctrl   && IsKeyUp(KEY_LEFT_CONTROL)    && IsKeyUp(KEY_RIGHT_CONTROL))    has_ctrl  = false;
                if (!has_ctrl  && (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))) has_ctrl  = true;
                if (has_shift  && IsKeyUp(KEY_LEFT_SHIFT)      && IsKeyUp(KEY_RIGHT_SHIFT))      has_shift = false;
                if (!has_shift && (IsKeyDown(KEY_LEFT_SHIFT)   || IsKeyDown(KEY_RIGHT_SHIFT)))   has_shift = true;
                // clang-format on

                int key;

                while ((key = GetKeyPressed())) {
                        if (0) {
                        }

                        // J, C-n or Down
                        else if (key == 'J' || (has_ctrl && key == 'N') || key == KEY_DOWN) {
                                ++g.cursor;
                                cursor_trim();
                        }

                        // k, C-p or Up
                        else if (key == 'K' || (has_ctrl && key == 'P') || key == KEY_UP) {
                                --g.cursor;
                                cursor_trim();
                        }

                        else if (key == KEY_ENTER) {
                                printf("ENTER on %s\n", g.entry_list.items[g.cursor]->path);
                                // change the root to the folder under the cursor
                                if (g.root != g.entry_list.items[g.cursor] &&
                                    g.entry_list.items[g.cursor]->is_dir) {
                                        change_root(g.entry_list.items[g.cursor]);
                                } else if (!g.entry_list.items[g.cursor]->is_dir) {
                                        // the problem with this approach is that the terminal is the same, so
                                        // nvim runs in the same terminal for example, and logs break nvim. If I
                                        // detach, nvim runs in a terminal that is not visible, so it's not a
                                        // solution.
                                        Command c = { 0 };
                                        Command_add(&c, "xdg-open", g.entry_list.items[g.cursor]->path);
                                        Command_run_async(c);
                                }
                        }

                        else if (key == KEY_BACKSPACE) {
                                printf("BACKSPACE on %s\n", g.entry_list.items[g.cursor]->path);
                                // change the root to the parent folder
                                if (g.root->parent) {
                                        change_root(g.root->parent);
                                } else {
                                        Entry *e = entry_create(GetPrevDirectoryPath(g.root->path),
                                                                NULL, true, g.root->nesting_level - 1);
                                        g.root->parent = e;
                                        open_directory(e);
                                        change_root(e);
                                }
                        }

                        else if (key == KEY_SPACE) {
                                printf("SPACE on %s\n", g.entry_list.items[g.cursor]->path);
                                entry_toggle_visibility(g.entry_list.items[g.cursor]);
                        }

                        else if (has_shift && key == '.') { // '>'
                                // opens the dir
                                entry_visibility_show(g.entry_list.items[g.cursor]);
                        }

                        else if (!has_shift && key == '.') {
                                // toggle dot filter (hide files starting with a dots)
                                g.entry_list_filters ^= FILTER_IGNORE_DOTS;
                                g.list_need_refresh = true;
                        }

                        else if (has_shift && key == ',') { // '<'
                                // closes the parent dir or current if the
                                // cursor is over a directory and it's open
                                if (g.entry_list.items[g.cursor]->is_dir &&
                                    g.entry_list.items[g.cursor]->is_extended) {
                                        entry_visibility_hide(g.entry_list.items[g.cursor]);
                                }

                                else {
                                        // search for the parent index
                                        int i = 0;
                                        for (; i < g.entry_list.count; i++) {
                                                if (g.entry_list.items[i] == g.entry_list.items[g.cursor]->parent) {
                                                        g.cursor = i;
                                                        entry_visibility_hide(g.entry_list.items[i]);
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

                if (g.list_need_refresh) refresh_global_entry_list();

                // Before BeginDrawing: decoding a big image takes a while, and
                // doing it mid-frame stalls the one being drawn.
                preview_ensure(g.entry_list.items[g.cursor]);

                BeginDrawing();
                ClearBackground(BLACK);

                // window bounds for debugging
                DrawRectangleRec(g.filetree, DARKBLUE);
                DrawRectangleRec(g.preview, DARKGRAY);
                DrawRectangleLines(g.filetree.x, g.filetree.y,
                                   g.filetree_c.cols * g.char_dimension.x,
                                   g.filetree_c.rows * g.char_dimension.y, RED);

                entry_list_print(g.entry_list, g.cursor);
                preview_print(g.entry_list.items[g.cursor]);

                EndDrawing();
        }
}

static void
config_free()
{
        free(config.title);
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
        printf("Loading config from %s\n", found);

        Conf_get_int(conf, &config.width, "Config.window.width");
        Conf_get_int(conf, &config.height, "Config.window.height");

        // The string Conf_get_str returns belongs to Lua and dies with the
        // handle at Conf_close, so keep our own copy.
        const char *title = NULL;
        if (Conf_get_str(conf, &title, "Config.window.title") == CONF_OK) {
                free(config.title);
                config.title = strdup(title);
        }
        // todo: add all the things that can be loaded

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
