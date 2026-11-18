#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../include/flux.h"
#include "../include/util.h"
#include "../include/parser.h"

#define FLUX_CACHE_DIR "/var/cache/flux"
#define FLUX_CACHE_MAX_ENTRIES 4096
#define FLUX_CACHE_ARCHIVE_SUFFIX ".tar.zst"
#define FLUX_CACHE_SIG_SUFFIX ".tar.zst.minisig"

typedef struct {
    char name[FLUX_MAX_PATH_LEN];
    long size;
} cache_entry_t;

static int has_suffix(const char *s, const char *suffix) {
    size_t sl = strlen(s), xl = strlen(suffix);
    return sl >= xl && strcmp(s + sl - xl, suffix) == 0;
}

// strips the archive/signature suffix so an archive and its .minisig share one key
static void entry_key(const char *file, char *out, size_t outlen) {
    snprintf(out, outlen, "%s", file);
    char *dot = strstr(out, FLUX_CACHE_ARCHIVE_SUFFIX);
    if (dot) *dot = '\0';
}

// the cache key is <name>_<version>-<6 hex of the cflags hash>: an entry is in use when it is
// exactly the installed version, or the version the local recipe currently asks for, of a package
// that is still installed. Anything else can only ever be a rebuild nobody will look up again
static int key_in_use(const char *key, char (*names)[FLUX_MAX_NAME_LEN], int count, const flux_config_t *config) {
    for (int i = 0; i < count; i++) {
        char versions[2][FLUX_MAX_VERSION_LEN];
        int vcount = 0;
        versions[0][0] = versions[1][0] = '\0';

        flux_pkg_info_t info;
        if (flux_db_read_info(names[i], &info) == FLUX_ERR_NONE && info.version[0]) {
            snprintf(versions[vcount++], FLUX_MAX_VERSION_LEN, "%s", info.version);
        }

        char koto_path[FLUX_MAX_PATH_LEN * 2 + 16];
        snprintf(koto_path, sizeof(koto_path), "%s/%s/kotodama", config->local_repo_path, names[i]);
        flux_recipe_t *recipe = calloc(1, sizeof(*recipe));
        if (recipe && parse_kotodama(recipe, koto_path) == FLUX_ERR_NONE && recipe->version[0])
            snprintf(versions[vcount++], FLUX_MAX_VERSION_LEN, "%s", recipe->version);
        free(recipe);

        for (int v = 0; v < vcount; v++) {
            char prefix[FLUX_MAX_NAME_LEN + FLUX_MAX_VERSION_LEN + 4];
            snprintf(prefix, sizeof(prefix), "%s_%s-", names[i], versions[v]);
            size_t pl = strlen(prefix);
            if (strncmp(key, prefix, pl) == 0 && strlen(key + pl) == 6) return 1;
        }
    }
    return 0;
}

static void format_size(long bytes, char *out, size_t outlen) {
    snprintf(out, outlen, "%.1fMB", bytes / 1024.0 / 1024.0);
}

static int cache_clean(int argc, char **argv, const char *usage) {
    int all = 0, yes = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--all") == 0) all = 1;
        else if (strcmp(argv[i], "--unused") == 0) all = 0;
        else if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--yes") == 0) yes = 1;
        else {
            flux_usage_error(usage);
            return FLUX_ERR_USAGE;
        }
    }

    cache_entry_t *entries = malloc(FLUX_CACHE_MAX_ENTRIES * sizeof(cache_entry_t));
    if (!entries) {
        flux_err("out of memory");
        return FLUX_ERR_GENERAL;
    }
    int count = 0;
    long total = 0;
    long freed_total = 0;

    char (*names)[FLUX_MAX_NAME_LEN] = NULL;
    int name_count = 0;
    flux_config_t config;
    memset(&config, 0, sizeof(config));
    if (!all) {
        if (flux_load_config(&config) != FLUX_ERR_NONE) { free(entries); return FLUX_ERR_GENERAL; }
        names = malloc((size_t)FLUX_MAX_INSTALL_QUEUE * FLUX_MAX_NAME_LEN);
        if (!names) { free(entries); flux_err("out of memory"); return FLUX_ERR_GENERAL; }
        flux_db_list_installed(names, FLUX_MAX_INSTALL_QUEUE, &name_count);
    }

    flux_action(all ? "Cleaning the whole binary cache" : "Cleaning unused binary cache entries");

    DIR *d = opendir(FLUX_CACHE_DIR);
    if (!d) {
        flux_ok("cache is empty (%s does not exist)", FLUX_CACHE_DIR);
        free(entries);
        free(names);
        return FLUX_ERR_NONE;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL && count < FLUX_CACHE_MAX_ENTRIES) {
        if (!has_suffix(de->d_name, FLUX_CACHE_ARCHIVE_SUFFIX) && !has_suffix(de->d_name, FLUX_CACHE_SIG_SUFFIX)) continue;

        char path[FLUX_MAX_PATH_LEN * 2];
        snprintf(path, sizeof(path), "%s/%s", FLUX_CACHE_DIR, de->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        total += st.st_size;

        if (!all) {
            char key[FLUX_MAX_PATH_LEN];
            entry_key(de->d_name, key, sizeof(key));
            if (key_in_use(key, names, name_count, &config)) continue;
        }

        snprintf(entries[count].name, sizeof(entries[count].name), "%s", de->d_name);
        entries[count].size = st.st_size;
        freed_total += st.st_size;
        count++;
    }
    closedir(d);
    free(names);

    if (count == 0) {
        flux_ok("nothing to clean");
        free(entries);
        return FLUX_ERR_NONE;
    }

    for (int i = 0; i < count; i++) {
        if (has_suffix(entries[i].name, FLUX_CACHE_SIG_SUFFIX)) continue;
        char size[16];
        format_size(entries[i].size, size, sizeof(size));
        printf("  - %s (%s)\n", entries[i].name, size);
    }
    char freed[16], whole[16];
    format_size(freed_total, freed, sizeof(freed));
    format_size(total, whole, sizeof(whole));
    printf("%s of %s will be freed\n", freed, whole);

    if (!yes) {
        printf("Proceed? [y/N] ");
        fflush(stdout);
        char answer[8];
        if (!fgets(answer, sizeof(answer), stdin) || (answer[0] != 'y' && answer[0] != 'Y')) {
            flux_warn("aborted");
            free(entries);
            return FLUX_ERR_NONE;
        }
    }

    int removed = 0;
    for (int i = 0; i < count; i++) {
        char path[FLUX_MAX_PATH_LEN * 2];
        snprintf(path, sizeof(path), "%s/%s", FLUX_CACHE_DIR, entries[i].name);
        if (remove(path) == 0) removed++;
        else flux_warn("could not remove %s", path);
    }
    free(entries);

    // download scratch (sources, .apk files) is only ever reused by an interrupted install,
    // so it goes with a full clean. The Alpine index is kept: flux install needs it until the next flux check
    if (all) system("rm -rf /tmp/flux-build");

    flux_ok("removed %d file%s, %s freed", removed, removed == 1 ? "" : "s", freed);
    return FLUX_ERR_NONE;
}

int flux_cache(int argc, char **argv, const char *usage) {
    if (argc < 1) {
        flux_usage_error(usage);
        return FLUX_ERR_USAGE;
    }

    if (strcmp(argv[0], "clean") == 0)
        return cache_clean(argc - 1, argv + 1, usage);

    flux_err("unknown cache subcommand: %s", argv[0]);
    return FLUX_ERR_USAGE;
}
