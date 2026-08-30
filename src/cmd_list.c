#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../include/flux.h"
#include "../include/util.h"

static int cmp_names(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

int flux_list(int argc, char **argv, const char *usage) {
    int show_auto = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--auto") == 0) {
            show_auto = 1;
        } else {
            flux_usage_error(usage);
            return FLUX_ERR_USAGE;
        }
    }

    char names[FLUX_MAX_INSTALL_QUEUE][FLUX_MAX_NAME_LEN];
    int count = 0;
    if (flux_db_list_installed(names, FLUX_MAX_INSTALL_QUEUE, &count) != FLUX_ERR_NONE) {
        flux_err("failed to read package database");
        return FLUX_ERR_GENERAL;
    }

    qsort(names, count, FLUX_MAX_NAME_LEN, cmp_names);

    int shown = 0;
    for (int i = 0; i < count; i++) {
        flux_pkg_info_t info;
        if (flux_db_read_info(names[i], &info) != FLUX_ERR_NONE) continue;
        if (!show_auto && info.auto_installed) continue;

        if (!shown) {
            if (flux_colors_enabled()) printf("\033[1m%-32s %-12s %-9s %s\033[0m\n", "PACKAGE", "VERSION", "SOURCE", "");
            else printf("%-32s %-12s %-9s %s\n", "PACKAGE", "VERSION", "SOURCE", "");
        }
        if (flux_colors_enabled())
            printf("\033[32m%-32s\033[0m %-12s %-9s %s\n", info.name, info.version, info.source, info.auto_installed ? "auto" : "");
        else
            printf("%-32s %-12s %-9s %s\n", info.name, info.version, info.source, info.auto_installed ? "auto" : "");
        shown++;
    }

    if (shown == 0) {
        if (!show_auto)
            flux_warn("no manually installed packages (run 'flux list -a' to see all)");
        else
            flux_warn("no packages installed");
    }

    return FLUX_ERR_NONE;
}
