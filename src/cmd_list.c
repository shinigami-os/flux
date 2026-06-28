#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../include/flux.h"
#include "../include/util.h"

static int cmp_names(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

int flux_list(int argc, char **argv, const char *usage) {
    int auto_only = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--auto") == 0) {
            auto_only = 1;
        } else {
            flux_usage_error(usage);
            return FLUX_ERR_USAGE;
        }
    }

    char names[FLUX_MAX_INSTALL_QUEUE][FLUX_MAX_NAME_LEN];
    int count = 0;
    if (flux_db_list_installed(names, FLUX_MAX_INSTALL_QUEUE, &count) != FLUX_ERR_NONE) {
        fprintf(stderr, "flux: failed to read package database\n");
        return FLUX_ERR_GENERAL;
    }

    qsort(names, count, FLUX_MAX_NAME_LEN, cmp_names);

    int shown = 0;
    for (int i = 0; i < count; i++) {
        flux_pkg_info_t info;
        if (flux_db_read_info(names[i], &info) != FLUX_ERR_NONE) continue;
        if (auto_only && !info.auto_installed) continue;

        printf("%-32s %-12s %s\n", info.name, info.version, info.auto_installed ? "auto" : "manual");
        shown++;
    }

    if (shown == 0) {
        if (auto_only)
            printf("[flux] no auto-installed packages\n");
        else
            printf("[flux] no packages installed\n");
    }

    return FLUX_ERR_NONE;
}
