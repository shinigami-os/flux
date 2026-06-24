#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include "../include/flux.h"
#include "../include/util.h"
#include "../include/parser.h"

static int g_auto_installed = 0;
static int g_yes = 0;

static int fetch_source(const char *url, const char *dest) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "curl -L -o \"%s\" \"%s\"", dest, url);
    return system(cmd);
}

static int verify_sha256(const char *path, const char *expected) {
    if (strcmp(expected, "SKIP") == 0 || strlen(expected) == 0) {
        fprintf(stderr, "flux: warning: sha256 check skipped\n");
        return 0;
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "sha256sum \"%s\" | cut -d' ' -f1 | tr -d '\\n' > /tmp/flux_hash_actual", path);
    system(cmd);

    FILE *f = fopen("/tmp/flux_hash_actual", "r");
    if (!f) return 1;

    char actual[65] = {0};
    fread(actual, 1, 64, f);
    fclose(f);
    remove("/tmp/flux_hash_actual");

    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "flux: checksum mismatch\nexpected: %s\ngot:      %s\n", expected, actual);
        return 1;
    }
    return 0;
}

// extracts a fetched upstream SOURCE tarball (.tar.gz/.tar.xz/.tar.bz2/...), not
// to be confused with the binary cache archive format (always .tar.zst, extracted
// inline where it's used). `tar -xf` auto-detects the compression format from the
// file itself, and --strip-components=1 flattens the usual single top-level
// name-version/ directory so build hooks land directly in build_dir, matching
// cmd_build.c's extraction of the same kind of tarball.
static int extract_tarball(const char *tarball, const char *dest) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" && mkdir -p \"%s\" && tar -xf \"%s\" -C \"%s\" --strip-components=1", dest, dest, tarball, dest);
    return system(cmd);
}

// runs only against the real root filesystem
static int run_post_install_hook(const char *hook, const char *recipe_dir) {
    if (strlen(hook) == 0) return 0;

    printf("[flux] running post-install...\n");
    system("mkdir -p /tmp/flux-build");
    const char *script_path = "/tmp/flux-build/.flux_post_install.sh";
    FILE *f = fopen(script_path, "w");
    if (!f) return FLUX_ERR_GENERAL;
    fprintf(f, "#!/bin/sh\nset -e\nexport FLUX_RECIPE_DIR=\"%s\"\n%s\n", recipe_dir, hook);
    fclose(f);
    chmod(script_path, 0755);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "sh \"%s\"", script_path);
    int ret = system(cmd);
    remove(script_path);
    return ret;
}

static int run_hook(const char *hook, const char *build_dir, const char *destdir, const char *recipe_dir) {
    if (strlen(hook) == 0) return 0;

    char script_path[256];
    snprintf(script_path, sizeof(script_path), "%s/.flux_hook.sh", build_dir);
    FILE *f = fopen(script_path, "w");
    if (!f) return FLUX_ERR_GENERAL;
    fprintf(f, "#!/bin/sh\nset -e\ncd \"%s\"\nexport DESTDIR=\"%s\"\nexport FLUX_RECIPE_DIR=\"%s\"\n%s\n", build_dir, destdir, recipe_dir, hook);
    fclose(f);
    chmod(script_path, 0755);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "sh \"%s\"", script_path);
    int ret = system(cmd);
    remove(script_path);
    return ret;
}

static int queue_contains(flux_install_queue_t *q, const char *name) {
    for (int i = 0; i < q->count; i++)
        if (strcmp(q->pkgs[i], name) == 0) return 1;
    return 0;
}

static int collect_deps(const char *pkg, flux_config_t *config, flux_install_queue_t *queue, char visited[][FLUX_MAX_NAME_LEN], int *visited_count) {
    for (int i = 0; i < *visited_count; i++)
        if (strcmp(visited[i], pkg) == 0) return FLUX_ERR_NONE;

    if (*visited_count < FLUX_MAX_INSTALL_QUEUE) {
        strncpy(visited[*visited_count], pkg, FLUX_MAX_NAME_LEN - 1);
        (*visited_count)++;
    }

    if (flux_db_is_installed(pkg)) return FLUX_ERR_NONE;

    char koto_path[FLUX_MAX_PATH_LEN * 2 + 16];
    snprintf(koto_path, sizeof(koto_path), "%s/%s/kotodama", config->local_repo_path, pkg);

    flux_recipe_t recipe;
    memset(&recipe, 0, sizeof(recipe));
    if (parse_kotodama(&recipe, koto_path) != FLUX_ERR_NONE) {
        fprintf(stderr, "flux: no recipe found for dependency '%s'\n", pkg);
        return FLUX_ERR_DEPENDENCY;
    }

    // decide whether THIS package needs its own build deps pulled in.
    int has_source = (strlen(recipe.url) != 0);
    int has_install_hook = (strlen(recipe.hook_install) != 0);
    int pure_meta = !has_source && !has_install_hook;

    int needs_build_deps = 0;
    if (pure_meta) {
        needs_build_deps = 0;
    } else if (!has_source) {
        needs_build_deps = 1;
    } else {
        char cache_key[256];
        char cache_path[FLUX_MAX_PATH_LEN];
        memset(cache_key, 0, sizeof(cache_key));
        if (flux_cache_key(recipe.name, recipe.version, recipe.cflags, config->package_target, cache_key, sizeof(cache_key)) == FLUX_ERR_NONE) {
            needs_build_deps = (flux_cache_lookup(cache_key, cache_path, sizeof(cache_path)) != FLUX_ERR_NONE);
        } else {
            needs_build_deps = 1;
        }
    }

    char (*lists[2])[FLUX_MAX_NAME_LEN] = { recipe.rdeps, NULL };
    int counts[2] = { FLUX_MAX_RDEPS, 0 };
    if (needs_build_deps) {
        lists[1] = recipe.deps;
        counts[1] = FLUX_MAX_DEPS;
    }

    for (int l = 0; l < 2; l++) {
        if (!lists[l]) continue;
        for (int i = 0; i < counts[l]; i++) {
            if (strlen(lists[l][i]) == 0) continue;
            int err = collect_deps(lists[l][i], config, queue, visited, visited_count);
            if (err != FLUX_ERR_NONE) return err;
        }
    }

    if (!queue_contains(queue, pkg) && queue->count < FLUX_MAX_INSTALL_QUEUE) {
        strncpy(queue->pkgs[queue->count], pkg, FLUX_MAX_NAME_LEN - 1);
        queue->count++;
    }

    return FLUX_ERR_NONE;
}

static int copy_destdir_to_root(const char *destdir) {
    FILE *f = fopen("/tmp/flux_copy.sh", "w");
    if (!f) return FLUX_ERR_GENERAL;
    fprintf(f,
        "#!/bin/sh\nset -e\n"
        "find \"%s\" \\( -type f -o -type l \\) | while IFS= read -r src; do\n"
        "  dst=\"${src#%s}\"\n"
        "  mkdir -p \"$(dirname \"$dst\")\"\n"
        "  cp -a \"$src\" \"$dst\"\n"
        "done\n",
        destdir, destdir);
    fclose(f);
    chmod("/tmp/flux_copy.sh", 0755);
    int ret = system("sh /tmp/flux_copy.sh");
    remove("/tmp/flux_copy.sh");
    return ret == 0 ? FLUX_ERR_NONE : FLUX_ERR_GENERAL;
}

static int collect_files_from_destdir(const char *destdir, char files[][FLUX_MAX_PATH_LEN], const char **ptrs, int *count) {
    char find_cmd[512];
    snprintf(find_cmd, sizeof(find_cmd), "find \"%s\" -type f", destdir);
    FILE *fp = popen(find_cmd, "r");
    if (!fp) return FLUX_ERR_GENERAL;

    char line[FLUX_MAX_PATH_LEN];
    while (fgets(line, sizeof(line), fp) && *count < FLUX_MAX_INSTALLED_FILES) {
        strip_newline(line);
        const char *sys_path = line + strlen(destdir);
        strncpy(files[*count], sys_path, FLUX_MAX_PATH_LEN - 1);
        ptrs[*count] = files[*count];
        (*count)++;
    }
    pclose(fp);
    return FLUX_ERR_NONE;
}

int flux_install(int argc, char **argv, const char *usage) {
    // parse -y flag
    if (argc >= 1 && strcmp(argv[0], "-y") == 0) {
        g_yes = 1;
        argv++;
        argc--;
    }

    if (argc < 1) {
        flux_usage_error(usage);
        return FLUX_ERR_USAGE;
    }

    const char *pkg = argv[0];
    printf("[flux] installing: %s\n", pkg);

    // load config
    flux_config_t config;
    memset(&config, 0, sizeof(config));
    int err = flux_load_config(&config);
    if (err != FLUX_ERR_NONE) return err;

    if (flux_db_is_installed(pkg)) {
        printf("[flux] %s is already installed\n", pkg);
        return FLUX_ERR_NONE;
    }

    struct stat st;
    if (stat(config.local_repo_path, &st) != 0) {
        fprintf(stderr, "flux: recipe repo not found at %s\n", config.local_repo_path);
        fprintf(stderr, "hint: run 'flux update' to download the recipe repo\n");
        return FLUX_ERR_GENERAL;
    }

    // parse recipe
    char koto_path[FLUX_MAX_PATH_LEN * 2];
    snprintf(koto_path, sizeof(koto_path), "%s/%s/kotodama", config.local_repo_path, pkg);
    if (stat(koto_path, &st) != 0) {
        fprintf(stderr, "flux: no recipe found for '%s'\n", pkg);
        return FLUX_ERR_NOT_FOUND;
    }

    flux_recipe_t recipe;
    memset(&recipe, 0, sizeof(recipe));
    err = parse_kotodama(&recipe, koto_path);
    if (err != FLUX_ERR_NONE) return err;

    printf("[flux] %s version %s\n", recipe.name, recipe.version);

    char recipe_dir[FLUX_MAX_PATH_LEN * 2 + 16];
    snprintf(recipe_dir, sizeof(recipe_dir), "%s/%s", config.local_repo_path, pkg);

    int has_source = (strlen(recipe.url) != 0);
    int has_install_hook = (strlen(recipe.hook_install) != 0);
    // pure meta-package: no source to fetch and no install hook to run
    int pure_meta = !has_source && !has_install_hook;

    // meta-packages (empty [source]) are just dependency lists, optionally with trivial
    // file-drop install steps
    char destdir[256];
    snprintf(destdir, sizeof(destdir), "/tmp/flux-build/%s-destdir", pkg);
    char cache_key[256];
    memset(cache_key, 0, sizeof(cache_key));
    char cache_path[FLUX_MAX_PATH_LEN];
    int cache_hit = 0;

    if (has_source) {
        if (flux_cache_key(recipe.name, recipe.version, recipe.cflags, config.package_target, cache_key, sizeof(cache_key)) == FLUX_ERR_NONE) {
            if (flux_cache_lookup(cache_key, cache_path, sizeof(cache_path)) == FLUX_ERR_NONE) {
                printf("[flux] cache hit: %s\n", cache_path);
                if (flux_cache_verify(cache_path, config.flux_pub_path) == FLUX_ERR_NONE) {
                    char cmd[1024];
                    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\" && zstd -d \"%s\" -o /tmp/flux_cache_extract.tar && tar -C \"%s\" -xf /tmp/flux_cache_extract.tar && rm /tmp/flux_cache_extract.tar", destdir, cache_path, destdir);
                    if (system(cmd) == 0)
                        cache_hit = 1;
                }
                if (!cache_hit)
                    printf("[flux] cache verification failed, falling back to source\n");
            } else {
                printf("[flux] cache miss, building from source\n");
            }
        } else {
            printf("[flux] cache key generation failed, building from source\n");
        }
    }

    // dep resolution is now done per-package inside collect_deps
    if (!g_auto_installed) {
        flux_install_queue_t queue;
        memset(&queue, 0, sizeof(queue));
        char visited[FLUX_MAX_INSTALL_QUEUE][FLUX_MAX_NAME_LEN];
        memset(visited, 0, sizeof(visited));
        int visited_count = 0;

        int err2 = collect_deps(pkg, &config, &queue, visited, &visited_count);
        if (err2 != FLUX_ERR_NONE) return err2;

        if (queue.count > 1 || (queue.count == 1 && strcmp(queue.pkgs[0], pkg) != 0)) {
            printf("\nThe following packages will be installed:\n  ");
            for (int i = 0; i < queue.count; i++) {
                char kp[FLUX_MAX_PATH_LEN * 2 + 16];
                snprintf(kp, sizeof(kp), "%s/%s/kotodama", config.local_repo_path, queue.pkgs[i]);
                flux_recipe_t r;
                memset(&r, 0, sizeof(r));
                parse_kotodama(&r, kp);
                printf("%s -v%s", queue.pkgs[i], r.version);
                if (i < queue.count - 1) printf("  ");
            }
            printf("\n\nProceed? [Y/n] ");
            if (g_yes) {
                printf("Y\n");
            } else {
                char answer[8] = {0};
                if (fgets(answer, sizeof(answer), stdin)) {
                    if (answer[0] == 'n' || answer[0] == 'N') {
                        printf("Aborted.\n");
                        return FLUX_ERR_NONE;
                    }
                }
            }
            printf("\n");
        }

        g_auto_installed = 1;
        for (int i = 0; i < queue.count - 1; i++) {
            char *dep_argv[] = { queue.pkgs[i] };
            int dep_err = flux_install(1, dep_argv, "flux install <pkg>");
            if (dep_err != FLUX_ERR_NONE) {
                fprintf(stderr, "flux: failed to install dependency '%s'\n", queue.pkgs[i]);
                g_auto_installed = 0;
                return FLUX_ERR_DEPENDENCY;
            }
        }
        g_auto_installed = 0;
    }

    // install from cache
    if (cache_hit) {
        if (copy_destdir_to_root(destdir) != FLUX_ERR_NONE) {
            fprintf(stderr, "flux: failed to copy cached files to system\n");
            return FLUX_ERR_GENERAL;
        }

        char installed_files[FLUX_MAX_INSTALLED_FILES][FLUX_MAX_PATH_LEN];
        const char *file_ptrs[FLUX_MAX_INSTALLED_FILES];
        int file_count = 0;
        collect_files_from_destdir(destdir, installed_files, file_ptrs, &file_count);

        flux_pkg_info_t info;
        memset(&info, 0, sizeof(info));
        strncpy(info.name,    recipe.name,    FLUX_MAX_NAME_LEN - 1);
        strncpy(info.version, recipe.version, FLUX_MAX_VERSION_LEN - 1);
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        strftime(info.install_date, sizeof(info.install_date), "%Y-%m-%d %H:%M:%S", t);
        info.auto_installed = g_auto_installed;
        flux_db_register(&info, file_ptrs, file_count);

        char cleanup[512];
        snprintf(cleanup, sizeof(cleanup), "rm -rf \"%s\"", destdir);
        system(cleanup);

        if (run_post_install_hook(recipe.hook_post_install, recipe_dir) != 0) {
            fprintf(stderr, "flux: post-install failed\n");
            return FLUX_ERR_BUILD;
        }

        printf("[flux] %s installed successfully (from cache)\n", pkg);
        return FLUX_ERR_NONE;
    }

    // pure meta-package: no source, no install hook : just dep registration plus
    // an optional post-install hook 
    if (pure_meta) {
        if (run_post_install_hook(recipe.hook_post_install, recipe_dir) != 0) {
            fprintf(stderr, "flux: post-install failed\n");
            return FLUX_ERR_BUILD;
        }

        flux_pkg_info_t info;
        memset(&info, 0, sizeof(info));
        strncpy(info.name,    recipe.name,    FLUX_MAX_NAME_LEN - 1);
        strncpy(info.version, recipe.version, FLUX_MAX_VERSION_LEN - 1);
        time_t now_m = time(NULL);
        struct tm *t_m = localtime(&now_m);
        strftime(info.install_date, sizeof(info.install_date), "%Y-%m-%d %H:%M:%S", t_m);
        info.auto_installed = g_auto_installed;
        flux_db_register(&info, NULL, 0);
        printf("[flux] %s installed successfully (meta-package)\n", pkg);
        return FLUX_ERR_NONE;
    }

    // build from source
    char build_dir[256];
    char tarball[256];
    char installed_files[FLUX_MAX_INSTALLED_FILES][FLUX_MAX_PATH_LEN];
    const char *file_ptrs[FLUX_MAX_INSTALLED_FILES];
    int file_count = 0;

    snprintf(build_dir, sizeof(build_dir), "/tmp/flux-build/%s", pkg);
    snprintf(tarball,   sizeof(tarball),   "/tmp/flux-build/%s.tar.gz", pkg);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p /tmp/flux-build");
    system(cmd);

    if (has_source) {
        printf("[flux] fetching source: %s\n", recipe.url);
        if (fetch_source(recipe.url, tarball) != 0) {
            fprintf(stderr, "flux: failed to fetch source\n");
            return FLUX_ERR_NETWORK;
        }

        printf("[flux] verifying checksum...\n");
        if (verify_sha256(tarball, recipe.sha256) != 0) {
            fprintf(stderr, "flux: checksum verification failed\n");
            return FLUX_ERR_GENERAL;
        }

        printf("[flux] extracting...\n");
        if (extract_tarball(tarball, build_dir) != 0) {
            fprintf(stderr, "flux: failed to extract tarball\n");
            return FLUX_ERR_GENERAL;
        }
    } else {
        snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", build_dir);
        system(cmd);
    }

    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", destdir);
    system(cmd);

    printf("[flux] running pre-build...\n");
    if (run_hook(recipe.hook_pre_build, build_dir, destdir, recipe_dir) != 0) {
        fprintf(stderr, "flux: pre-build failed\n");
        return FLUX_ERR_BUILD;
    }

    printf("[flux] building...\n");
    if (run_hook(recipe.hook_build, build_dir, destdir, recipe_dir) != 0) {
        fprintf(stderr, "flux: build failed\n");
        return FLUX_ERR_BUILD;
    }

    printf("[flux] running post-build...\n");
    if (run_hook(recipe.hook_post_build, build_dir, destdir, recipe_dir) != 0) {
        fprintf(stderr, "flux: post-build failed\n");
        return FLUX_ERR_BUILD;
    }

    printf("[flux] installing files...\n");
    if (run_hook(recipe.hook_install, build_dir, destdir, recipe_dir) != 0) {
        fprintf(stderr, "flux: install hook failed\n");
        return FLUX_ERR_BUILD;
    }

    if (copy_destdir_to_root(destdir) != FLUX_ERR_NONE) {
        fprintf(stderr, "flux: failed to copy files to system\n");
        return FLUX_ERR_GENERAL;
    }

    collect_files_from_destdir(destdir, installed_files, file_ptrs, &file_count);

    // register in package db
    flux_pkg_info_t info;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    memset(&info, 0, sizeof(info));
    strncpy(info.name,    recipe.name,    FLUX_MAX_NAME_LEN - 1);
    strncpy(info.version, recipe.version, FLUX_MAX_VERSION_LEN - 1);
    strftime(info.install_date, sizeof(info.install_date), "%Y-%m-%d %H:%M:%S", t);
    info.auto_installed = g_auto_installed;
    flux_db_register(&info, file_ptrs, file_count);

    if (strlen(cache_key) > 0)
        flux_cache_store(cache_key, destdir, config.flux_secret_key_path);

    char cleanup_cmd[1024];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf \"%s\" \"%s\" \"%s\"", build_dir, destdir, tarball);
    system(cleanup_cmd);

    if (run_post_install_hook(recipe.hook_post_install, recipe_dir) != 0) {
        fprintf(stderr, "flux: post-install failed\n");
        return FLUX_ERR_BUILD;
    }

    printf("[flux] %s installed successfully\n", pkg);
    return FLUX_ERR_NONE;
}
