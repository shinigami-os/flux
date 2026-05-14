#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include "../include/flux.h"
#include "../include/util.h"
#include "../include/parser.h"

static int fetch_source(const char *url, const char *dest) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "curl -L -o \"%s\" \"%s\"", dest, url);
    return system(cmd);
}

static int verify_sha256(const char *path, const char *expected) {
    if (strcmp(expected, "SKIP") == 0) {
        fprintf(stderr, "flux: warning: sha256 check skipped for %s\n", path);
        return 0;
    }
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "echo \"%s  %s\" | sha256sum -c --quiet", expected, path);
    return system(cmd);
}

static int extract_tarball(const char *tarball, const char *dest) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\" && tar -xf \"%s\" -C \"%s\" --strip-components=1", dest, tarball, dest);
    return system(cmd);
}

static int run_hook(const char *hook, const char *build_dir, const char *destdir) {
    if (strlen(hook) == 0) return 0;

    // write hook to a temp script
    char script_path[256];
    snprintf(script_path, sizeof(script_path), "%s/.flux_hook.sh", build_dir);
    FILE *f = fopen(script_path, "w");
    if (!f) return FLUX_ERR_GENERAL;
    fprintf(f, "#!/bin/sh\nset -e\ncd \"%s\"\nexport DESTDIR=\"%s\"\n%s\n", build_dir, destdir, hook);
    fclose(f);
    chmod(script_path, 0755);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "sh \"%s\"", script_path);
    int ret = system(cmd);
    remove(script_path);
    return ret;
}

int flux_install(int argc, char **argv, const char *usage) {
    if (argc < 1) {
        flux_usage_error(usage);
        return FLUX_ERR_USAGE;
    }

    const char *pkg = argv[0];
    printf("[flux] installing: %s\n", pkg);

    // step 1: load config
    flux_config_t config;
    memset(&config, 0, sizeof(config));
    int err = flux_load_config(&config);
    if (err != FLUX_ERR_NONE) return err;

    // check if already installed
    if (flux_db_is_installed(pkg)) {
        printf("[flux] %s is already installed\n", pkg);
        return FLUX_ERR_NONE;
    }

    // step 2: find kotodama file
    char koto_path[FLUX_MAX_PATH_LEN * 2];
    snprintf(koto_path, sizeof(koto_path), "%s/%s/kotodama", config.local_repo_path, pkg);

    struct stat st;
    if (stat(koto_path, &st) != 0) {
        fprintf(stderr, "flux: no recipe found for '%s'\n", pkg);
        return FLUX_ERR_NOT_FOUND;
    }

    // step 3: parse recipe
    flux_recipe_t recipe;
    memset(&recipe, 0, sizeof(recipe));
    err = parse_kotodama(&recipe, koto_path);
    if (err != FLUX_ERR_NONE) return err;

    printf("[flux] %s version %s\n", recipe.name, recipe.version);

    // step 4: check binary cache (stubbed: always miss)
    printf("[flux] binary cache: miss, building from source\n");

    // step 5: dep resolution (stubbed)
    printf("[flux] dependency resolution: stub, skipping\n");

    // step 6: fetch source
    char build_dir[256];
    snprintf(build_dir, sizeof(build_dir), "/tmp/flux-build/%s", pkg);

    char tarball[256];
    snprintf(tarball, sizeof(tarball), "/tmp/flux-build/%s.tar.gz", pkg);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p /tmp/flux-build");
    system(cmd);

    printf("[flux] fetching source: %s\n", recipe.url);
    if (fetch_source(recipe.url, tarball) != 0) {
        fprintf(stderr, "flux: failed to fetch source\n");
        return FLUX_ERR_NETWORK;
    }

    // step 7: verify sha256
    printf("[flux] verifying checksum...\n");
    if (verify_sha256(tarball, recipe.sha256) != 0) {
        fprintf(stderr, "flux: checksum verification failed\n");
        return FLUX_ERR_GENERAL;
    }

    // step 8: extract
    printf("[flux] extracting...\n");
    if (extract_tarball(tarball, build_dir) != 0) {
        fprintf(stderr, "flux: failed to extract tarball\n");
        return FLUX_ERR_GENERAL;
    }

    // step 9: run hooks
    char destdir[256];
    snprintf(destdir, sizeof(destdir), "/tmp/flux-build/%s-destdir", pkg);
    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", destdir);
    system(cmd);

    printf("[flux] running pre-build...\n");
    if (run_hook(recipe.hook_pre_build, build_dir, destdir) != 0) {
        fprintf(stderr, "flux: pre-build failed\n");
        return FLUX_ERR_BUILD;
    }

    printf("[flux] building...\n");
    if (run_hook(recipe.hook_build, build_dir, destdir) != 0) {
        fprintf(stderr, "flux: build failed\n");
        return FLUX_ERR_BUILD;
    }

    printf("[flux] running post-build...\n");
    if (run_hook(recipe.hook_post_build, build_dir, destdir) != 0) {
        fprintf(stderr, "flux: post-build failed\n");
        return FLUX_ERR_BUILD;
    }

    printf("[flux] installing files...\n");
    if (run_hook(recipe.hook_install, build_dir, destdir) != 0) {
        fprintf(stderr, "flux: install hook failed\n");
        return FLUX_ERR_BUILD;
    }

    // step 10: copy from destdir to live system
    snprintf(cmd, sizeof(cmd), "cp -a \"%s\"/. /", destdir);
    if (system(cmd) != 0) {
        fprintf(stderr, "flux: failed to copy files to system\n");
        return FLUX_ERR_GENERAL;
    }

    // step 11: register in package db
    flux_pkg_info_t info;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    memset(&info, 0, sizeof(info));
    strncpy(info.name,    recipe.name,    FLUX_MAX_NAME_LEN - 1);
    strncpy(info.version, recipe.version, FLUX_MAX_VERSION_LEN - 1);
    strftime(info.install_date, sizeof(info.install_date), "%Y-%m-%d %H:%M:%S", t);
    info.auto_installed = 0;
    flux_db_register(&info, NULL, 0);

    // cleanup
    char cleanup_cmd[1024];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf \"%s\" \"%s\" \"%s\"", build_dir, destdir, tarball);
    system(cleanup_cmd);

    printf("[flux] %s installed successfully\n", pkg);
    return FLUX_ERR_NONE;
}