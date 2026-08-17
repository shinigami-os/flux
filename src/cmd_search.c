#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "../include/flux.h"
#include "../include/util.h"
#include "../include/parser.h"

int flux_search(int argc, char **argv, const char *usage) {
    if (argc < 1) {
        flux_usage_error(usage);
        return FLUX_ERR_USAGE;
    }

    const char *query = argv[0];

    flux_config_t config;
    memset(&config, 0, sizeof(config));
    if (flux_load_config(&config) != FLUX_ERR_NONE) return FLUX_ERR_GENERAL;

    DIR *dir = opendir(config.local_repo_path);
    if (!dir) {
        flux_err("cannot open recipe repo at %s", config.local_repo_path);
        return FLUX_ERR_SOURCE;
    }

    struct dirent *entry;
    int found = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char koto_path[FLUX_MAX_PATH_LEN * 2 + 16];
        snprintf(koto_path, sizeof(koto_path), "%s/%s/kotodama", config.local_repo_path, entry->d_name);

        flux_recipe_t recipe;
        memset(&recipe, 0, sizeof(recipe));
        if (parse_kotodama(&recipe, koto_path) != FLUX_ERR_NONE) continue;

        if (strstr(recipe.name, query) || strstr(recipe.description, query)) {
            if (!found && flux_colors_enabled()) printf("\033[1m%-24s  DESCRIPTION\033[0m\n", "PACKAGE");
            else if (!found) printf("%-24s  DESCRIPTION\n", "PACKAGE");
            if (flux_colors_enabled()) printf("\033[32m%-24s\033[0m  %s\n", recipe.name, recipe.description);
            else printf("%-24s  %s\n", recipe.name, recipe.description);
            found++;
        }
    }

    closedir(dir);

    if (!found) {
        flux_warn("no results for '%s'", query);
        return FLUX_ERR_NOT_FOUND;
    }

    return FLUX_ERR_NONE;
}