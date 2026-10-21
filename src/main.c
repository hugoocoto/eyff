// eyff - explore Your F*cking Files
// Copyright (C) 2026 Hugo Coto
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lua_compat.h" // before conf.h

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
        char unused; // C99 needs a member: replace it with the first setting
} Config;

void
explorer()
{
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

        *config = (Config) { 0 }; // The defaults

        if (path == NULL && (path = found = find_config()) == NULL) return 0;

        if (Conf_open(&conf, path) != CONF_OK) {
                fprintf(stderr, NAME ": can't load %s\n", path);
                free(found);
                return 1;
        }

        // Read the settings here, e.g. Conf_get_int(conf, &config->n, "Config.n").
        // Strings belong to Lua and die with the next Conf_* call: copy them.

        Conf_close(conf);
        free(found);
        return 0;
}

int
main(int argc, char **argv)
{
        const char *show_version;
        const char *config_path;
        Config config;

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

        explorer();

        config_free(&config);
        flag_free();
        return 0;
}
