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
        fprintf(stderr, "flux: cannot open recipe repo at %s\n", config.local_repo_path);
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
            printf("%-20s %s\n", recipe.name, recipe.description);
            found++;
        }
    }

    closedir(dir);

    if (!found) {
        printf("[flux] no results for '%s'\n", query);
        return FLUX_ERR_NOT_FOUND;
    }

    return FLUX_ERR_NONE;
}