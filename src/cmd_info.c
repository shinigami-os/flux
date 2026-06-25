#include <stdio.h>
#include <string.h>
#include "../include/flux.h"
#include "../include/util.h"
#include "../include/parser.h"

int flux_info(int argc, char **argv, const char *usage) {
    if (argc < 1) {
        flux_usage_error(usage);
        return FLUX_ERR_USAGE;
    }

    const char *pkg = argv[0];

    flux_config_t config;
    memset(&config, 0, sizeof(config));
    if (flux_load_config(&config) != FLUX_ERR_NONE) return FLUX_ERR_GENERAL;

    // try to find the recipe
    char koto_path[FLUX_MAX_PATH_LEN * 2 + 16];
    snprintf(koto_path, sizeof(koto_path), "%s/%s/kotodama", config.local_repo_path, pkg);

    flux_recipe_t recipe;
    memset(&recipe, 0, sizeof(recipe));
    int has_recipe = (parse_kotodama(&recipe, koto_path) == FLUX_ERR_NONE);

    if (!has_recipe && !flux_db_is_installed(pkg)) {
        fprintf(stderr, "flux: no package '%s' found\n", pkg);
        return FLUX_ERR_NOT_FOUND;
    }

    int installed = flux_db_is_installed(pkg);

    printf("\n");
    printf("  %-16s %s\n",   "name:",        has_recipe ? recipe.name        : pkg);
    printf("  %-16s %s\n",   "version:",     has_recipe ? recipe.version     : "unknown");
    printf("  %-16s %s\n",   "description:", has_recipe ? recipe.description : "unknown");
    printf("  %-16s %s\n",   "license:",     has_recipe ? recipe.license     : "unknown");
    printf("  %-16s %dMB\n", "size:",        has_recipe ? recipe.size        : 0);
    printf("  %-16s %s\n",   "status:",      installed  ? "installed"        : "not installed");

    if (installed) {
        flux_pkg_info_t db_info;
        if (flux_db_read_info(pkg, &db_info) == FLUX_ERR_NONE) {
            printf("  %-16s %s\n", "installed on:", db_info.install_date);
            printf("  %-16s %s\n", "auto-installed:", db_info.auto_installed ? "yes" : "no");
        }
    }

    if (has_recipe && strlen(recipe.deps[0]) > 0) {
        printf("  %-16s", "build deps:");
        for (int i = 0; i < FLUX_MAX_DEPS && strlen(recipe.deps[i]) > 0; i++)
            printf(" %s", recipe.deps[i]);
        printf("\n");
    }

    if (has_recipe && strlen(recipe.rdeps[0]) > 0) {
        printf("  %-16s", "runtime deps:");
        for (int i = 0; i < FLUX_MAX_RDEPS && strlen(recipe.rdeps[i]) > 0; i++)
            printf(" %s", recipe.rdeps[i]);
        printf("\n");
    }

    printf("\n");
    return FLUX_ERR_NONE;
}