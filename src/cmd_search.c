#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "../include/flux.h"
#include "../include/util.h"
#include "../include/parser.h"
#include "../include/alpine.h"

static void print_header(int *header_printed) {
    if (*header_printed) return;
    if (flux_colors_enabled()) printf("\033[1m%-24s %-9s  DESCRIPTION\033[0m\n", "PACKAGE", "SOURCE");
    else printf("%-24s %-9s  DESCRIPTION\n", "PACKAGE", "SOURCE");
    *header_printed = 1;
}

static void print_result(const char *name, const char *source, const char *description, int *header_printed) {
    print_header(header_printed);
    if (flux_colors_enabled()) printf("\033[32m%-24s\033[0m %-9s  %s\n", name, source, description);
    else printf("%-24s %-9s  %s\n", name, source, description);
}

int flux_search(int argc, char **argv, const char *usage) {
    if (argc < 1) {
        flux_usage_error(usage);
        return FLUX_ERR_USAGE;
    }

    const char *query = argv[0];

    flux_config_t config;
    memset(&config, 0, sizeof(config));
    if (flux_load_config(&config) != FLUX_ERR_NONE) return FLUX_ERR_GENERAL;

    int found = 0;

    // kira-* recipes first - always available, no network/index needed
    DIR *dir = opendir(config.local_repo_path);
    if (!dir) {
        flux_err("cannot open recipe repo at %s", config.local_repo_path);
        return FLUX_ERR_SOURCE;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char koto_path[FLUX_MAX_PATH_LEN * 2 + 16];
        snprintf(koto_path, sizeof(koto_path), "%s/%s/kotodama", config.local_repo_path, entry->d_name);

        flux_recipe_t recipe;
        memset(&recipe, 0, sizeof(recipe));
        if (parse_kotodama(&recipe, koto_path) != FLUX_ERR_NONE) continue;

        if (strstr(recipe.name, query) || strstr(recipe.description, query)) {
            print_result(recipe.name, "kotodama", recipe.description, &found);
        }
    }
    closedir(dir);

    // then the Alpine index, if one has been synced - best effort, missing it
    // shouldn't fail a search that already found kira-* recipes just fine
    char arch[ALPINE_MAX_ARCH_LEN];
    alpine_repos_t repos;
    if (alpine_arch_from_target(config.package_target, arch, sizeof(arch)) == FLUX_ERR_NONE &&
        alpine_repos_load(&config, arch, &repos) == FLUX_ERR_NONE) {
        alpine_index_t *indexes[2] = { &repos.main, &repos.community };
        for (int i = 0; i < 2; i++) {
            alpine_index_t *idx = indexes[i];
            for (int j = 0; j < idx->count; j++) {
                alpine_pkg_t *pkg = &idx->pkgs[j];
                if (strstr(pkg->name, query) || strstr(pkg->description, query)) {
                    print_result(pkg->name, "alpine", pkg->description, &found);
                }
            }
        }
        alpine_repos_free(&repos);
    }

    if (!found) {
        flux_warn("no results for '%s'", query);
        return FLUX_ERR_NOT_FOUND;
    }

    return FLUX_ERR_NONE;
}
