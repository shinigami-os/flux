#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/flux.h"
#include "../include/util.h"
#include "../include/alpine.h"

// defensive cap on chain depth, same reasoning as autoremove's 50-pass cap - a real dependency
// graph never gets anywhere near this deep, it only matters if something formed a cycle
#define FLUX_WHY_MAX_DEPTH 50

// prints every installed package that directly depends on target, recursing into each one that's
// itself only auto-installed (an explicitly-installed dependent is where a chain ends - that's the
// actual reason it's on the system, no further "why is *that* installed" to answer). visited guards
// against a cycle turning this into infinite recursion; returns how many direct dependents were found
static int print_dependents(const char *target, pkg_dep_cache_t *cache, char names[][FLUX_MAX_NAME_LEN], int count,
                             const alpine_repos_t *repos, int depth,
                             char visited[][FLUX_MAX_NAME_LEN], int *visited_count) {
    if (depth > FLUX_WHY_MAX_DEPTH) return 0;

    for (int v = 0; v < *visited_count; v++)
        if (strcmp(visited[v], target) == 0) return 0;
    if (*visited_count < FLUX_MAX_INSTALL_QUEUE)
        strncpy(visited[(*visited_count)++], target, FLUX_MAX_NAME_LEN - 1);

    char indent[FLUX_WHY_MAX_DEPTH * 2 + 1];
    int indent_len = depth * 2;
    if (indent_len > FLUX_WHY_MAX_DEPTH * 2) indent_len = FLUX_WHY_MAX_DEPTH * 2;
    memset(indent, ' ', (size_t)indent_len);
    indent[indent_len] = '\0';

    int found = 0;
    for (int j = 0; j < count; j++) {
        if (strcmp(names[j], target) == 0) continue;
        if (!cached_depends_on(&cache[j], target, repos)) continue;
        found++;

        flux_pkg_info_t info;
        int explicit_install = (flux_db_read_info(names[j], &info) == FLUX_ERR_NONE) && !info.auto_installed;

        printf("%s- %s%s\n", indent, names[j], explicit_install ? " (explicitly installed)" : "");
        if (!explicit_install)
            print_dependents(names[j], cache, names, count, repos, depth + 1, visited, visited_count);
    }
    return found;
}

int flux_why(int argc, char **argv, const char *usage) {
    if (argc != 1) {
        flux_usage_error(usage);
        return FLUX_ERR_USAGE;
    }
    const char *pkg = argv[0];

    if (!flux_db_is_installed(pkg)) {
        flux_err("%s is not installed", pkg);
        return FLUX_ERR_NOT_FOUND;
    }

    flux_config_t config;
    memset(&config, 0, sizeof(config));
    if (flux_load_config(&config) != FLUX_ERR_NONE) return FLUX_ERR_GENERAL;

    // loaded once (no network - whatever was last cached), same as autoremove: most installed
    // packages are plain Alpine ones checked against their real D: line, not a kotodama file
    char arch[ALPINE_MAX_ARCH_LEN];
    alpine_repos_t repos;
    memset(&repos, 0, sizeof(repos));
    if (alpine_arch_from_target(config.package_target, arch, sizeof(arch)) == FLUX_ERR_NONE)
        alpine_repos_load(&config, arch, &repos);

    char names[FLUX_MAX_INSTALL_QUEUE][FLUX_MAX_NAME_LEN];
    int count = 0;
    flux_db_list_installed(names, FLUX_MAX_INSTALL_QUEUE, &count);

    // heap, not stack: each entry is ~35KB (ALPINE_MAX_TOKENS_PER_LINE is a generous upper
    // bound) - FLUX_MAX_INSTALL_QUEUE of them is tens of MB
    pkg_dep_cache_t *cache = malloc((size_t)FLUX_MAX_INSTALL_QUEUE * sizeof(pkg_dep_cache_t));
    char *visited = malloc((size_t)FLUX_MAX_INSTALL_QUEUE * FLUX_MAX_NAME_LEN);
    if (!cache || !visited) {
        free(cache);
        free(visited);
        alpine_repos_free(&repos);
        flux_err("out of memory");
        return FLUX_ERR_GENERAL;
    }
    for (int j = 0; j < count; j++)
        build_dep_cache(names[j], &config, &repos, &cache[j]);

    flux_pkg_info_t info;
    int have_info = (flux_db_read_info(pkg, &info) == FLUX_ERR_NONE);
    int explicit_install = have_info && !info.auto_installed;

    flux_action("Why is %s installed?", pkg);
    if (explicit_install)
        flux_ok("explicitly installed");

    int visited_count = 0;
    int dependent_count = print_dependents(pkg, cache, names, count,
                                            &repos, 0, (char (*)[FLUX_MAX_NAME_LEN])visited, &visited_count);

    if (dependent_count == 0) {
        if (explicit_install)
            printf("nothing else depends on it.\n");
        else
            flux_warn("auto-installed, but nothing depends on it - this is an orphan; run 'flux autoremove' to clean it up");
    }

    free(cache);
    free(visited);
    alpine_repos_free(&repos);
    return FLUX_ERR_NONE;
}
