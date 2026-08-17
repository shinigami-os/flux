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
    double t_start = flux_now_seconds();

    flux_config_t config;
    memset(&config, 0, sizeof(config));
    if (flux_load_config(&config) != FLUX_ERR_NONE) return FLUX_ERR_GENERAL;

    struct stat st;
    if (stat(config.local_repo_path, &st) != 0) {
        flux_err("recipe repo not found at %s", config.local_repo_path);
        flux_err("hint: run 'flux update' to download the recipe repo");
        return FLUX_ERR_GENERAL;
    }

    char koto_path[FLUX_MAX_PATH_LEN * 2 + 16];
    snprintf(koto_path, sizeof(koto_path), "%s/%s/kotodama", config.local_repo_path, pkg);
    if (stat(koto_path, &st) != 0) {
        flux_err("no recipe found for '%s'", pkg);
        return FLUX_ERR_NOT_FOUND;
    }

    flux_recipe_t recipe;
    memset(&recipe, 0, sizeof(recipe));
    if (parse_kotodama(&recipe, koto_path) != FLUX_ERR_NONE) return FLUX_ERR_KOTODAMA;

    int is_meta = (strlen(recipe.url) == 0);

    // nothing to do, checked before any cache lookup
    if (is_meta && strlen(recipe.hook_install) == 0) {
        flux_ok("%s is a meta-package, nothing to build", pkg);
        return FLUX_ERR_NONE;
    }

    flux_action("Building %s %s%s", recipe.name, recipe.version, cross ? " (cross)" : "");

    char cache_key[256];
    memset(cache_key, 0, sizeof(cache_key));
    char cache_path[FLUX_MAX_PATH_LEN];
    if (!is_meta) {
        char native_target[64];
        const char *cross_target;
        if (cross) {
            cross_target = "x86_64-linux-musl";
        } else if (flux_native_target(native_target, sizeof(native_target)) == FLUX_ERR_NONE) {
            cross_target = native_target;
        } else {
            cross_target = config.package_target;
        }
        if (flux_cache_key(recipe.name, recipe.version, recipe.cflags, cross_target, cache_key, sizeof(cache_key)) != FLUX_ERR_NONE) {
            flux_err("failed to compute cache key");
            return FLUX_ERR_GENERAL;
        }

        // flux build never pulls from the remote cache, only ever builds or uses a local hit
        if (flux_cache_lookup_local(cache_key, cache_path, sizeof(cache_path)) == FLUX_ERR_NONE) {
            flux_ok("%s is already cached at %s", pkg, cache_path);
            return FLUX_ERR_NONE;
        }
    }

    char build_dir[256];
    char tarball[512];
    char destdir[256];
    char cmd[2048];
    snprintf(build_dir, sizeof(build_dir), "/tmp/flux-build/%s", pkg);
    snprintf(destdir,   sizeof(destdir),   "/tmp/flux-build/%s-destdir", pkg);
    tarball[0] = '\0';

    system("mkdir -p /tmp/flux-build");

    int is_git = (!is_meta && strncmp(recipe.url, "git+", 4) == 0);

    if (is_git) {
        const char *git_url_start = recipe.url + 4;
        char git_url[512];
        char git_branch[256] = "";
        const char *hash = strchr(git_url_start, '#');
        if (hash) {
            size_t url_len = (size_t)(hash - git_url_start);
            if (url_len >= sizeof(git_url)) url_len = sizeof(git_url) - 1;
            strncpy(git_url, git_url_start, url_len);
            git_url[url_len] = '\0';
            strncpy(git_branch, hash + 1, sizeof(git_branch) - 1);
        } else {
            strncpy(git_url, git_url_start, sizeof(git_url) - 1);
            git_url[sizeof(git_url) - 1] = '\0';
        }

        flux_step("cloning: %s", git_url);
        if (strlen(git_branch) > 0) {
            snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" && git clone --depth=1 --recurse-submodules --shallow-submodules --branch \"%s\" \"%s\" \"%s\"",
                     build_dir, git_branch, git_url, build_dir);
        } else {
            snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" && git clone --depth=1 --recurse-submodules --shallow-submodules \"%s\" \"%s\"",
                     build_dir, git_url, build_dir);
        }
        if (system(cmd) != 0) {
            flux_err("failed to clone git repository");
            return FLUX_ERR_NETWORK;
        }

        if (strlen(recipe.sha256) > 0) {
            flux_step("verifying commit...");
            char sha_cmd[640];
            snprintf(sha_cmd, sizeof(sha_cmd),
                     "git -C \"%s\" rev-parse HEAD | tr -d '\\n' > /tmp/flux_hash_actual", build_dir);
            system(sha_cmd);
            FILE *f = fopen("/tmp/flux_hash_actual", "r");
            if (!f) return FLUX_ERR_GENERAL;
            char actual[65] = {0};
            fread(actual, 1, 64, f);
            fclose(f);
            remove("/tmp/flux_hash_actual");
            if (strcmp(actual, recipe.sha256) != 0) {
                flux_err("commit mismatch (expected %s, got %s)", recipe.sha256, actual);
                return FLUX_ERR_GENERAL;
            }
        }
    } else if (!is_meta) {
        const char *url_basename = strrchr(recipe.url, '/');
        url_basename = url_basename ? url_basename + 1 : recipe.url;
        snprintf(tarball, sizeof(tarball), "/tmp/flux-build/%s", url_basename);

        flux_step("fetching source: %s", recipe.url);
        snprintf(cmd, sizeof(cmd), "curl -L -o \"%s\" \"%s\"", tarball, recipe.url);
        if (system(cmd) != 0) {
            flux_err("failed to fetch source");
            return FLUX_ERR_NETWORK;
        }

        flux_step("verifying checksum...");
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
            flux_err("checksum mismatch (expected %s, got %s)", recipe.sha256, actual);
            return FLUX_ERR_GENERAL;
        }

        flux_step("extracting...");
        if (flux_extract_source(tarball, build_dir) != 0) {
            flux_err("failed to extract tarball");
            return FLUX_ERR_GENERAL;
        }
    } else {
        snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", build_dir);
        system(cmd);
    }

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
                fprintf(_f, "export PKG_CONFIG_PATH=\"%s/usr/lib/pkgconfig:%s/usr/share/pkgconfig\"\n", config.flux_cross_compile_sysroot, config.flux_cross_compile_sysroot); \
                fprintf(_f, "export PKG_CONFIG_LIBDIR=\"%s/usr/lib/pkgconfig\"\n", config.flux_cross_compile_sysroot); \
                fprintf(_f, "export PKG_CONFIG_SYSROOT_DIR=\"%s\"\n", config.flux_cross_compile_sysroot); \
            } \
            fprintf(_f, "%s\n", hook); \
            fclose(_f); \
            chmod(_script, 0755); \
            char _cmd[512]; \
            snprintf(_cmd, sizeof(_cmd), "sh \"%s\"", _script); \
            int _ret = system(_cmd); \
            remove(_script); \
            if (_ret != 0) { \
                flux_err("%s failed", label); \
                return FLUX_ERR_BUILD; \
            } \
        } \
    } while(0)

    if (!is_meta) {
        flux_step("running pre-build...");
        RUN_HOOK(recipe.hook_pre_build, "pre-build");
        flux_step("building...");
        RUN_HOOK(recipe.hook_build, "build");
        flux_step("running post-build...");
        RUN_HOOK(recipe.hook_post_build, "post-build");
    }
    flux_step("installing to destdir...");
    RUN_HOOK(recipe.hook_install, "install");

    if (!is_meta && cross && strlen(config.flux_cross_compile_sysroot) > 0) {
        // libtool bakes cross-sysroot paths from PKG_CONFIG_LIBDIR into .la dependency_libs; strip them since the target has its deps under plain /usr/lib
        char destdir_la_patch[FLUX_MAX_PATH_LEN * 2 + 128];
        snprintf(destdir_la_patch, sizeof(destdir_la_patch),
            "find \"%s\" -name \"*.la\" | xargs -r sed -i"
            " \"s|%s/usr/|/usr/|g\""
            " 2>/dev/null || true",
            destdir, config.flux_cross_compile_sysroot);
        system(destdir_la_patch);
        flux_step("stripped cross-sysroot paths from destdir .la files");
    }

    if (!is_meta && cross && !recipe.no_sysroot_stage && strlen(config.flux_cross_compile_sysroot) > 0) {
        char sysroot_cmd[FLUX_MAX_PATH_LEN * 2 + 32];
        snprintf(sysroot_cmd, sizeof(sysroot_cmd), "cp -a \"%s\"/. \"%s\"/", destdir, config.flux_cross_compile_sysroot);
        system(sysroot_cmd);
        flux_step("installed cross build artifacts to sysroot");

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
        flux_step("patched .la files in sysroot");
    }

    #undef RUN_HOOK

    // store in cache (meta-packages never reach here)
    if (!is_meta) {
        flux_step("caching...");
        if (flux_cache_store(cache_key, destdir, config.flux_secret_key_path) != FLUX_ERR_NONE) {
            flux_err("failed to store in cache");
            return FLUX_ERR_CACHE;
        }
    }

    char cleanup[1280];
    if (strlen(tarball) > 0) {
        snprintf(cleanup, sizeof(cleanup), "rm -rf \"%s\" \"%s\" \"%s\"", build_dir, destdir, tarball);
    } else {
        snprintf(cleanup, sizeof(cleanup), "rm -rf \"%s\" \"%s\"", build_dir, destdir);
    }
    system(cleanup);

    flux_ok("%s built and cached at /var/cache/flux/%s.tar.zst in %.1fs", pkg, cache_key, flux_now_seconds() - t_start);
    return FLUX_ERR_NONE;
}
