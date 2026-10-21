// eyff - explore Your F*cking Files
// Copyright (C) 2026 Hugo Coto
// SPDX-License-Identifier: GPL-3.0-or-later

#include <assert.h>
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

typedef struct Directory_or_Entry Directory_or_Entry;
typedef Da(Directory_or_Entry) Directory_List;

typedef struct File_Info {
        /* dirent.h */
        ino_t d_ino;
        char d_name[NAME_MAX];
        /* sys/stat.h */
        dev_t st_dev;     // Device ID of device containing file.
        ino_t st_ino;     // File serial number.
        mode_t st_mode;   // Mode of file (see below).
        nlink_t st_nlink; // Number of hard links to the file.
        uid_t st_uid;     // User ID of file.
        gid_t st_gid;     // Group ID of file.
        dev_t st_rdev;    // Device ID (if file is character or block special).
        off_t st_size;    // For regular files, the file size in bytes. For
                          // symbolic links, the length in bytes of the pathname
                          // contained in the symbolic link. For a shared memory
                          // object, the length in bytes. For a typed memory
                          // object, the length in bytes. For other file types,
                          // the use of this field is unspecified.

        struct timespec st_atim; // Last data access timestamp.
        struct timespec st_mtim; // Last data modification timestamp.
        struct timespec st_ctim; // Last file status change timestamp.
        blksize_t st_blksize;    // A file system-specific preferred I/O block size
                                 // for this object. In some file system types, this
                                 // may vary from file to file.
        blkcnt_t st_blocks;      // Number of blocks allocated for this object.
} File_Info;

typedef struct Entry {
        File_Info info;
} Entry;

typedef struct Directory {
        File_Info info;
        struct Directory *parent;
        char path[PATH_MAX]; // full path to this dir, unlike info.d_name
        int enties_with_subdir_count;
        Directory_List entries;
        bool show_files : 1; // show files inside this directory
        bool valid      : 1;
} Directory;

struct Directory_or_Entry {
        Directory *_d;
        Entry *_e;
        enum {
                TYPE_DIR,
                TYPE_ENTRY,
        } type;
};

typedef struct Grid {
        int rows, cols;
} Grid;

// globals
static struct {
        Directory *root;
        Font font;
        Grid grid;
        Rectangle usable;
        Vector2 csize;
} G;

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


File_Info
build_entry_get_info(struct dirent *e, struct stat stats)
{
        File_Info info;
        strcpy(info.d_name, e->d_name);
        info.d_ino = e->d_ino;

        info.st_dev = stats.st_dev;
        info.st_ino = stats.st_ino;
        info.st_mode = stats.st_mode;
        info.st_nlink = stats.st_nlink;
        info.st_uid = stats.st_uid;
        info.st_gid = stats.st_gid;
        info.st_rdev = stats.st_rdev;
        info.st_size = stats.st_size;
        info.st_atim = stats.st_atim;
        info.st_mtim = stats.st_mtim;
        info.st_ctim = stats.st_ctim;
        info.st_blksize = stats.st_blksize;
        info.st_blocks = stats.st_blocks;
        return info;
}

Entry *
build_entry_normal(struct dirent *e, struct stat stats)
{
        Entry *entry = calloc(1, sizeof(Entry));
        entry->info = build_entry_get_info(e, stats);
        return entry;
}

Directory *directory_open(File_Info info, const char *path, bool hide_dots, Directory *parent,
                          bool show_files);

Directory *
build_entry_dir(struct dirent *e, struct stat stats, bool hide_dots, Directory *parent)
{
        Directory *dir =
                directory_open(build_entry_get_info(e, stats), NULL, hide_dots, parent, false);
        return dir;
}

Directory_or_Entry
build_entry(struct dirent *e, bool hide_dots, Directory *parent)
{
        struct stat stats = { 0 };

        if (lstat(e->d_name, &stats)) {
                perror("lstat");
        }

        Directory_or_Entry ret;
        if (S_ISDIR(stats.st_mode)) {
                ret._d = build_entry_dir(e, stats, hide_dots, parent);
                ret.type = TYPE_DIR;
        } else {
                ret._e = build_entry_normal(e, stats);
                ret.type = TYPE_ENTRY;
        }
        return ret;
}

void
count_sub_recursive_up(Directory *d, int count)
{
        if (d == NULL) return;
        d->enties_with_subdir_count -= count;
        count_sub_recursive_up(d->parent, count);
}

void
count_add_recursive_up(Directory *d, int count)
{
        if (d == NULL) return;
        d->enties_with_subdir_count += count;
        count_add_recursive_up(d->parent, count);
}

// Free everything D lists and empty the list, leaving D itself alone: it
// belongs to its parent's list, not to its own.
void
directory_entries_free(Directory *d)
{
        Da_foreach (e, d->entries) {
                if (e->type == TYPE_DIR) {
                        directory_entries_free(e->_d);
                        free(e->_d);
                } else {
                        free(e->_e);
                }
        }
        Da_destroy(&d->entries);
}

// Fill D->entries from D->path. Returns false if it can't be opened, in
// which case D is left empty.
bool
directory_read_entries(Directory *d, bool hide_dots)
{
        DIR *dir = opendir(d->path);
        if (dir == NULL) return false;

        char cwd[PATH_MAX];
        bool have_cwd = getcwd(cwd, sizeof cwd) != NULL;

        if (chdir(d->path) == 0) { // so relative paths work without more effort
                struct dirent *entry;
                while ((entry = readdir(dir))) {
                        if (hide_dots && entry->d_name[0] == '.') continue;
                        Da_append(&d->entries, build_entry(entry, hide_dots, d));
                }
                if (have_cwd) {
                        int restored = chdir(cwd); // best effort: paths are full anyway
                        (void) restored;
                }
        }
        closedir(dir);

        return true;
}

// Copy SRC into DST, truncating it if it doesn't fit.
static void
path_copy(char *dst, size_t size, const char *src)
{
        size_t n = strlen(src);
        if (n >= size) n = size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
}

// Build PARENT/NAME into DST, or "" if it doesn't fit (which can't happen for
// a path the OS accepted).
static void
path_join(char *dst, size_t size, const char *parent, const char *name)
{
        size_t p = strlen(parent);
        size_t n = strlen(name);

        if (p + 1 + n >= size) {
                dst[0] = '\0';
                return;
        }
        memcpy(dst, parent, p);
        dst[p] = '/';
        memcpy(dst + p + 1, name, n + 1);
}

// Open PATH (the root) or a child of PARENT. The result must be freed with
// directory_entries_free + free, or kept in a parent's list like this one.
Directory *
directory_open(File_Info info, const char *path, bool hide_dots, Directory *parent, bool show_files)
{
        Directory *d = calloc(1, sizeof(Directory));
        d->info = info;
        d->parent = parent;
        d->show_files = show_files;
        // Count this dir's own line too, except for the root: it's never
        // drawn, only its contents are.
        d->enties_with_subdir_count = parent ? 1 : 0;

        if (path) {
                path_copy(d->path, sizeof d->path, path);
        } else if (parent) {
                path_join(d->path, sizeof d->path, parent->path, info.d_name);
        } else {
                path_copy(d->path, sizeof d->path, info.d_name);
        }

        if (show_files && directory_read_entries(d, hide_dots)) {
                d->valid = true;
                count_add_recursive_up(d, d->entries.count);
        }

        return d;
}

File_Info
get_info(Directory_or_Entry x)
{
        switch (x.type) {
        case TYPE_DIR:
                assert(x._d);
                return x._d->info;
        case TYPE_ENTRY:
                assert(x._e);
                return x._e->info;
        default:
                abort();
        }
}

void
Write(const char *text, int cw, int ch, Color tint)
{
        DrawTextEx(G.font, text, (Vector2) { G.csize.x * cw, G.csize.y * ch }, config.font_size, config.font_spacing, tint);
}

int
run_on_directory_or_entry_at_cursor(int cursor, int *i, Directory_List list, void (*f)(Directory_or_Entry *))
{
        Da_foreach(e, list)
        {
                if (*i == cursor) {
                        f(e);
                        return 1;
                }
                ++*i;
                if (e->type == TYPE_DIR) {
                        if (run_on_directory_or_entry_at_cursor(cursor, i, e->_d->entries, f)) return 1;
                }
        }
        return 0;
}

// print entries [from, to) highlight at cursor.
void
write_folder_entries_inner_recursive(int from, int to, int cursor, int *i, Directory_List list)
{
        static int indent = 0;
        Da_foreach(e, list)
        {
                if (*i >= from && *i < to) {
                        // should print

                        assert(G.usable.x == 0); // if not, the next
                                                 // line is not correct
                        int cw = 0;
                        Color tint;
                        if (*i == cursor) {
                                // should highlight
                                tint = BLUE;
                        } else {
                                tint = WHITE;
                        }
                        Write(get_info(*e).d_name, cw + indent * 2, *i - from, tint);
                }
                ++*i;
                if (e->type == TYPE_DIR) {
                        ++indent;
                        write_folder_entries_inner_recursive(from, to, cursor, i, e->_d->entries);
                        --indent;
                }
        }
}

void
write_folder_entries(int cursor)
{
        if (G.root->enties_with_subdir_count == 0) return;

        int line_height = G.csize.y > 0 ? (int) G.csize.y : 1;
        int page_items = Max((int) (G.usable.height / line_height), 1);
        int page_num = cursor / page_items;
        int from = Max(page_num * page_items, 0);
        int to = Min(G.root->enties_with_subdir_count, (page_num + 1) * page_items);
        assert(from < to && from <= cursor && cursor < to && to <= G.root->enties_with_subdir_count);

        int i = 0;
        write_folder_entries_inner_recursive(from, to, cursor, &i, G.root->entries);
}

void
action_on_space_entry(Entry *e)
{
        printf("YOU CLICK SPACE OVER %s\n", e->info.d_name);
}

void
action_on_space_dir(Directory *d)
{
        if (d->show_files) {
                // Collapsing: everything below this dir was counted up to its
                // ancestors, so remove all of it, not just the direct children.
                int lines = d->enties_with_subdir_count - 1;
                d->show_files = false;
                directory_entries_free(d);
                count_sub_recursive_up(d, lines);
        } else {
                // Expanding: the new children count as (count) extra lines.
                d->show_files = true;
                if (directory_read_entries(d, true)) {
                        count_add_recursive_up(d, d->entries.count);
                } else {
                        d->show_files = false;
                }
        }
}

void
action_on_space(Directory_or_Entry *de)
{
        switch (de->type) {
        case TYPE_DIR:
                return action_on_space_dir(de->_d);
        case TYPE_ENTRY:
                return action_on_space_entry(de->_e);
        default:
                abort();
        }
}

// open cwd on null
void
explorer(char *path)
{
        File_Info info = { 0 };

        char buf[4096];
        if (path == NULL) {
                getcwd(buf, sizeof buf);
                path = buf;
        }

        // kinda hard-coded
        info.st_mode = S_IFDIR;
        path_copy(info.d_name, sizeof info.d_name, path);
        G.root = directory_open(info, path, true, NULL, true);

        if (!G.root->valid) {
                return;
        }

        SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
        InitWindow(config.width, config.height, config.title);

        // Load config.font_name or 'monospace' or raylib default
        G.font = LoadFontEx(font_find_by_name(config.font_name) ?: "monospace", config.font_size, NULL, 0);
        if (!IsFontValid(G.font)) {
                G.font = GetFontDefault();
        }
        config.font_size = G.font.baseSize;

        bool first = true;
        int cursor = 0;

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

                // todo: improve input: homerow mods break keys + control not working + implement hold
                bool has_ctrl = IsKeyPressed(KEY_LEFT_CONTROL) || IsKeyPressed(KEY_RIGHT_CONTROL);

                if ((has_ctrl && IsKeyPressed(KEY_N)) || IsKeyPressed(KEY_DOWN)) {
                        if (G.root->enties_with_subdir_count == 0) {
                                cursor = 0;
                        } else if (++cursor >= G.root->enties_with_subdir_count) {
                                cursor = G.root->enties_with_subdir_count - 1;
                        }
                }

                if ((has_ctrl && IsKeyPressed(KEY_P)) || IsKeyPressed(KEY_UP)) {
                        if (--cursor < 0) cursor = 0;
                }

                if (IsKeyPressed(KEY_SPACE)) {
                        int x = 0;
                        run_on_directory_or_entry_at_cursor(cursor, &x, G.root->entries, action_on_space);
                }

                BeginDrawing();
                ClearBackground(BLACK);

                write_folder_entries(cursor);

                // window bounds for debugging
                DrawRectangleLines(0, 0, G.grid.cols * G.csize.x, G.grid.rows * G.csize.y, RED);

                EndDrawing();
        }
}

static void
config_free(Config *config)
{
        (void) config;
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
load_config(Config *config, const char *path)
{
        Conf conf = NULL;
        char *found = NULL;

        if (path == NULL && (path = found = find_config()) == NULL) return 0;

        if (Conf_open(&conf, path) != CONF_OK) {
                fprintf(stderr, NAME ": can't load %s\n", path);
                free(found);
                return 1;
        }

        Conf_get_int(conf, &config->width, "Config.window.width");
        Conf_get_int(conf, &config->height, "Config.window.height");
        Conf_get_str(conf, &config->title, "Config.window.title");

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

        if (load_config(&config, config_path)) {
                config_free(&config);
                flag_free();
                return 1;
        }

        explorer(0);

        config_free(&config);
        flag_free();
        return 0;
}
