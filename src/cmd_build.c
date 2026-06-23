#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../include/flux.h"
#include "../include/util.h"
#include "../include/parser.h"

int flux_build(int argc, char **argv, const char *usage) {
    if (argc < 1) {
        flux_usage_error(usage);
        return FLUX_ERR_USAGE;
    }

    int cross = 0;
    int pkg_idx = 0;
    if (argc >= 1 && strcmp(argv[0], "--cross") == 0) {
        cross = 1;
        pkg_idx = 1;
        if (argc < 2) {
            flux_usage_error(usage);
            return FLUX_ERR_USAGE;
        }
    }
    const char *pkg = argv[pkg_idx];

    printf("[flux] building: %s\n", pkg);

    // load config
    flux_config_t config;
    memset(&config, 0, sizeof(config));
    if (flux_load_config(&config) != FLUX_ERR_NONE) return FLUX_ERR_GENERAL;

    // find kotodama
    struct stat st;
    if (stat(config.local_repo_path, &st) != 0) {
        fprintf(stderr, "flux: recipe repo not found at %s\n", config.local_repo_path);
        fprintf(stderr, "hint: run 'flux update' to download the recipe repo\n");
        return FLUX_ERR_GENERAL;
    }

    char koto_path[FLUX_MAX_PATH_LEN * 2 + 16];
    snprintf(koto_path, sizeof(koto_path), "%s/%s/kotodama", config.local_repo_path, pkg);
    if (stat(koto_path, &st) != 0) {
        fprintf(stderr, "flux: no recipe found for '%s'\n", pkg);
        return FLUX_ERR_NOT_FOUND;
    }

    // parse recipe
    flux_recipe_t recipe;
    memset(&recipe, 0, sizeof(recipe));
    if (parse_kotodama(&recipe, koto_path) != FLUX_ERR_NONE) return FLUX_ERR_KOTODAMA;

    printf("[flux] %s version %s\n", recipe.name, recipe.version);

    int is_meta = (strlen(recipe.url) == 0);

    // pure meta-package: no source and no install hook — nothing to do at all.
    // Checked before any cache lookup since meta-packages never touch the cache.
    if (is_meta && strlen(recipe.hook_install) == 0) {
        printf("[flux] %s is a meta-package, nothing to build\n", pkg);
        return FLUX_ERR_NONE;
    }

    char cache_key[256];
    memset(cache_key, 0, sizeof(cache_key));
    char cache_path[FLUX_MAX_PATH_LEN];
    if (!is_meta) {
        const char *cross_target = cross ? "x86_64-linux-musl" : config.package_target;
        if (flux_cache_key(recipe.name, recipe.version, recipe.cflags, cross_target, cache_key, sizeof(cache_key)) != FLUX_ERR_NONE) {
            fprintf(stderr, "flux: failed to compute cache key\n");
            return FLUX_ERR_GENERAL;
        }

        if (flux_cache_lookup(cache_key, cache_path, sizeof(cache_path)) == FLUX_ERR_NONE) {
            printf("[flux] %s is already cached at %s\n", pkg, cache_path);
            return FLUX_ERR_NONE;
        }
    }

    char build_dir[256];
    char tarball[512];
    char destdir[256];
    char cmd[2048];
    snprintf(build_dir, sizeof(build_dir), "/tmp/flux-build/%s", pkg);
    snprintf(destdir,   sizeof(destdir),   "/tmp/flux-build/%s-destdir", pkg);

    system("mkdir -p /tmp/flux-build");

    if (!is_meta) {
        const char *url_basename = strrchr(recipe.url, '/');
        url_basename = url_basename ? url_basename + 1 : recipe.url;
        snprintf(tarball, sizeof(tarball), "/tmp/flux-build/%s", url_basename);

        printf("[flux] fetching source: %s\n", recipe.url);
        snprintf(cmd, sizeof(cmd), "curl -L -o \"%s\" \"%s\"", tarball, recipe.url);
        if (system(cmd) != 0) {
            fprintf(stderr, "flux: failed to fetch source\n");
            return FLUX_ERR_NETWORK;
        }

        // verify sha256
        printf("[flux] verifying checksum...\n");
        char sha_cmd[640];
        snprintf(sha_cmd, sizeof(sha_cmd), "sha256sum \"%s\" | cut -d' ' -f1 | tr -d '\\n' > /tmp/flux_hash_actual", tarball);
        system(sha_cmd);
        FILE *f = fopen("/tmp/flux_hash_actual", "r");
        if (!f) return FLUX_ERR_GENERAL;
        char actual[65] = {0};
        fread(actual, 1, 64, f);
        fclose(f);
        remove("/tmp/flux_hash_actual");
        if (strcmp(actual, recipe.sha256) != 0) {
            fprintf(stderr, "flux: checksum mismatch\nexpected: %s\ngot:      %s\n",
                    recipe.sha256, actual);
            return FLUX_ERR_GENERAL;
        }

        // extract
        printf("[flux] extracting...\n");
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" && mkdir -p \"%s\" && tar -xf \"%s\" -C \"%s\" --strip-components=1", build_dir, build_dir, tarball, build_dir);
        if (system(cmd) != 0) {
            fprintf(stderr, "flux: failed to extract tarball\n");
            return FLUX_ERR_GENERAL;
        }
    } else {
        snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", build_dir);
        system(cmd);
    }

    // run hooks
    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", destdir);
    system(cmd);

    if (!is_meta && cross) {
        char patch_cmd[FLUX_MAX_PATH_LEN * 2];
        snprintf(patch_cmd, sizeof(patch_cmd),
            "find \"%s\" -name configure -type f"
            " | xargs -r sed -i \"s|oldincludedir='/usr/include'|oldincludedir='/nonexistent'|g\""
            " 2>/dev/null || true",
            build_dir);
        system(patch_cmd);
    }

    // recipe dir exposed to hooks as FLUX_RECIPE_DIR
    char recipe_dir[FLUX_MAX_PATH_LEN * 2 + 16];
    snprintf(recipe_dir, sizeof(recipe_dir), "%s/%s", config.local_repo_path, pkg);

    // helper: write and run a hook script
    #define RUN_HOOK(hook, label) do { \
        if (strlen(hook) > 0) { \
            char _script[FLUX_MAX_PATH_LEN + 16]; \
            snprintf(_script, sizeof(_script), "%s/.flux_hook.sh", build_dir); \
            FILE *_f = fopen(_script, "w"); \
            if (!_f) return FLUX_ERR_GENERAL; \
            fprintf(_f, "#!/bin/sh\nset -e\ncd \"%s\"\nexport DESTDIR=\"%s\"\n", build_dir, destdir); \
            fprintf(_f, "export FLUX_RECIPE_DIR=\"%s\"\n", recipe_dir); \
            if (cross) { \
                fprintf(_f, "export PATH=\"%s:/usr/local/bin:/usr/bin:/bin:$PATH\"\n", config.flux_cross_toolchain_path); \
                fprintf(_f, "export CC=\"%sgcc\"\n", config.flux_cross_compile_prefix); \
                fprintf(_f, "export CXX=\"%sg++\"\n", config.flux_cross_compile_prefix); \
                fprintf(_f, "export AR=\"%sar\"\n", config.flux_cross_compile_prefix); \
                fprintf(_f, "export LD=\"%sld\"\n", config.flux_cross_compile_prefix); \
                fprintf(_f, "export STRIP=\"%sstrip\"\n", config.flux_cross_compile_prefix); \
                fprintf(_f, "export CROSS_COMPILE=\"%s\"\n", config.flux_cross_compile_prefix); \
                fprintf(_f, "export CPP=\"%sgcc -E\"\n", config.flux_cross_compile_prefix); \
                fprintf(_f, "export FLUX_CROSS_HOST=\"x86_64-linux-musl\"\n"); \
                fprintf(_f, "export FLUX_CROSS_SYSROOT=\"%s\"\n", config.flux_cross_compile_sysroot); \
            } \
            fprintf(_f, "export PKG_CONFIG_PATH=\"%s/usr/lib/pkgconfig:%s/usr/share/pkgconfig\"\n", config.flux_cross_compile_sysroot, config.flux_cross_compile_sysroot); \
            fprintf(_f, "export PKG_CONFIG_LIBDIR=\"%s/usr/lib/pkgconfig\"\n", config.flux_cross_compile_sysroot); \
            fprintf(_f, "export PKG_CONFIG_SYSROOT_DIR=\"%s\"\n", config.flux_cross_compile_sysroot); \
            fprintf(_f, "%s\n", hook); \
            fclose(_f); \
            chmod(_script, 0755); \
            char _cmd[512]; \
            snprintf(_cmd, sizeof(_cmd), "sh \"%s\"", _script); \
            int _ret = system(_cmd); \
            remove(_script); \
            if (_ret != 0) { \
                fprintf(stderr, "flux: %s failed\n", label); \
                return FLUX_ERR_BUILD; \
            } \
        } \
    } while(0)

    if (!is_meta) {
        printf("[flux] running pre-build...\n");
        RUN_HOOK(recipe.hook_pre_build, "pre-build");
        printf("[flux] building...\n");
        RUN_HOOK(recipe.hook_build, "build");
        printf("[flux] running post-build...\n");
        RUN_HOOK(recipe.hook_post_build, "post-build");
    }
    printf("[flux] installing to destdir...\n");
    RUN_HOOK(recipe.hook_install, "install");

    if (!is_meta && cross && strlen(config.flux_cross_compile_sysroot) > 0) {
        char sysroot_cmd[FLUX_MAX_PATH_LEN * 2 + 32];
        snprintf(sysroot_cmd, sizeof(sysroot_cmd), "cp -a \"%s\"/. \"%s\"/", destdir, config.flux_cross_compile_sysroot);
        system(sysroot_cmd);
        printf("[flux] installed cross build artifacts to sysroot\n");

        char la_patch[FLUX_MAX_PATH_LEN * 4 + 128];
        snprintf(la_patch, sizeof(la_patch),
            "find \"%s\" -name \"*.la\" | xargs -r sed -i"
            " -e \"s|^libdir='/usr/|libdir='%s/usr/|g\""
            " -e \"s| /usr/lib/| %s/usr/lib/|g\""
            " 2>/dev/null || true",
            config.flux_cross_compile_sysroot,
            config.flux_cross_compile_sysroot,
            config.flux_cross_compile_sysroot);
        system(la_patch);
        printf("[flux] patched .la files in sysroot\n");
    }

    #undef RUN_HOOK

    // store in cache (meta-packages never reach here as cacheable -- see above)
    if (!is_meta) {
        printf("[flux] caching...\n");
        if (flux_cache_store(cache_key, destdir, config.flux_secret_key_path) != FLUX_ERR_NONE) {
            fprintf(stderr, "flux: failed to store in cache\n");
            return FLUX_ERR_CACHE;
        }
    }

    // cleanup
    char cleanup[1280];
    snprintf(cleanup, sizeof(cleanup), "rm -rf \"%s\" \"%s\" \"%s\"", build_dir, destdir, tarball);
    system(cleanup);

    printf("[flux] %s built and cached at /var/cache/flux/%s.tar.zst\n", pkg, cache_key);
    return FLUX_ERR_NONE;
}
