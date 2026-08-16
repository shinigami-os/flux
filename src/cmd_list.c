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
        fprintf(stderr, "flux: failed to read package database\n");
        return FLUX_ERR_GENERAL;
    }

    qsort(names, count, FLUX_MAX_NAME_LEN, cmp_names);

    int shown = 0;
    for (int i = 0; i < count; i++) {
        flux_pkg_info_t info;
        if (flux_db_read_info(names[i], &info) != FLUX_ERR_NONE) continue;
        if (!show_auto && info.auto_installed) continue;

        printf("%-32s %-12s %s\n", info.name, info.version, info.auto_installed ? "auto" : "");
        shown++;
    }

    if (shown == 0) {
        if (!show_auto)
            printf("[flux] no manually installed packages (run 'flux list -a' to see all)\n");
        else
            printf("[flux] no packages installed\n");
    }

    return FLUX_ERR_NONE;
}
