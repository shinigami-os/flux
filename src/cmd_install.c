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
#include "../include/alpine.h"

static int g_auto_installed = 0;
static int g_yes = 0;
static int g_force = 0;
static int g_flatpak = 0;
// set only by install_batch's per-package recursive calls: dependency
// resolution + the summary table + the confirmation prompt already happened
// once for the whole batch, so each individual call should skip doing its
// own - kept separate from g_auto_installed, which still has to reflect
// whether THIS package was a root request or a pulled-in dep (DB bookkeeping
// for `flux autoremove`, and the "Installing" vs "installing dependency" header)
static int g_skip_deps = 0;
// set right before installing a resolved queue, so try_alpine_install() reuses the index that walk already loaded instead of refetching it per package
static alpine_repos_t *g_active_repos = NULL;

static int install_batch(int argc, char **argv, const char *usage, flux_config_t *config);
int flux_install(int argc, char **argv, const char *usage);

// returns FLUX_ERR_NOT_FOUND if flatpak is unusable or nothing matched, so the caller falls through to its own "no recipe found" error
static int try_flatpak_fallback(const char *pkg) {
    if (system("command -v flatpak >/dev/null 2>&1") != 0)
        return FLUX_ERR_NOT_FOUND;

    // remote-ls + grep works without a synced local appstream cache, unlike the otherwise more natural `flatpak search`
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "flatpak remote-ls flathub --app --columns=application 2>/dev/null | grep -i \"%s\" | head -5",
        pkg);
    FILE *f = popen(cmd, "r");
    if (!f) return FLUX_ERR_NOT_FOUND;

    char matches[5][256];
    int n = 0;
    while (n < 5 && fgets(matches[n], sizeof(matches[n]), f)) {
        strip_newline(matches[n]);
        if (strlen(matches[n]) > 0) n++;
    }
    pclose(f);

    if (n == 0) return FLUX_ERR_NOT_FOUND;

    int chosen = 0;

    if (n == 1) {
        flux_warn("no flux recipe found for '%s', but found on Flathub: %s", pkg, matches[0]);
        printf("Install via Flatpak? [y/N] ");
        fflush(stdout);
        if (!g_yes) {
            char answer[8] = {0};
            if (!fgets(answer, sizeof(answer), stdin) || (answer[0] != 'y' && answer[0] != 'Y')) {
                printf("Aborted.\n");
                return FLUX_ERR_NONE;
            }
        } else {
            printf("y\n");
        }
    } else {
        // always ask which one explicitly, even with -y/--yes, since that flag skips a yes/no confirmation, not a pick among several packages
        flux_warn("no flux recipe found for '%s', but found %d matches on Flathub:", pkg, n);
        for (int i = 0; i < n; i++)
            printf("  %d. %s\n", i + 1, matches[i]);
        printf("Install which one? [1-%d, or n to abort] ", n);
        fflush(stdout);

        char answer[16] = {0};
        if (!fgets(answer, sizeof(answer), stdin)) {
            printf("Aborted.\n");
            return FLUX_ERR_NONE;
        }
        strip_newline(answer);
        if (answer[0] == 'n' || answer[0] == 'N' || answer[0] == '\0') {
            printf("Aborted.\n");
            return FLUX_ERR_NONE;
        }
        char *endptr;
        long pick = strtol(answer, &endptr, 10);
        if (endptr == answer || pick < 1 || pick > n) {
            flux_err("invalid choice");
            return FLUX_ERR_NONE;
        }
        chosen = (int)(pick - 1);
    }

    char install_cmd[300];
    snprintf(install_cmd, sizeof(install_cmd), "flatpak install -y flathub \"%s\"", matches[chosen]);
    if (system(install_cmd) != 0) {
        flux_err("flatpak install failed");
        return FLUX_ERR_GENERAL;
    }
    return FLUX_ERR_NONE;
}

static int fetch_source(const char *url, const char *dest) {
    return flux_download(url, dest);
}

static int verify_sha256(const char *path, const char *expected) {
    if (strcmp(expected, "SKIP") == 0 || strlen(expected) == 0) {
        flux_warn("sha256 check skipped");
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
        flux_err("checksum mismatch (expected %s, got %s)", expected, actual);
        return 1;
    }
    return 0;
}

static int extract_tarball(const char *tarball, const char *dest) {
    return flux_extract_source(tarball, dest);
}

// runs only against the real root filesystem
static int run_post_install_hook(const char *hook, const char *recipe_dir) {
    if (strlen(hook) == 0) return 0;

    flux_step("running post-install...");
    char env_prefix[FLUX_MAX_PATH_LEN + 32];
    snprintf(env_prefix, sizeof(env_prefix), "export FLUX_RECIPE_DIR=\"%s\"\n", recipe_dir);
    return flux_run_script(hook, env_prefix);
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

// true if pkg is installed and already at the recipe's current version - the
// only case a plain (non -f) install should skip; a version mismatch means
// an update is available and should proceed without needing -f
static int is_installed_and_current(const char *pkg, const flux_recipe_t *recipe) {
    flux_pkg_info_t info;
    if (flux_db_read_info(pkg, &info) != FLUX_ERR_NONE) return 0;
    return strcmp(info.version, recipe->version) == 0;
}

static int queue_contains(flux_install_queue_t *q, const char *name) {
    for (int i = 0; i < q->count; i++)
        if (strcmp(q->pkgs[i].name, name) == 0) return 1;
    return 0;
}

// config + a lazily-loaded Alpine index (shared for one whole dependency walk) + names ruled out by a "!pkg" conflict
typedef struct {
    flux_config_t *config;
    alpine_repos_t repos;
    int repos_loaded;
    char forbidden[FLUX_MAX_INSTALL_QUEUE][FLUX_MAX_NAME_LEN];
    int forbidden_count;
} collect_ctx_t;

static int ensure_alpine_repos(collect_ctx_t *ctx) {
    if (ctx->repos_loaded) return FLUX_ERR_NONE;

    char arch[ALPINE_MAX_ARCH_LEN];
    if (alpine_arch_from_target(ctx->config->package_target, arch, sizeof(arch)) != FLUX_ERR_NONE)
        return FLUX_ERR_GENERAL;

    int err = alpine_repos_load(ctx->config, arch, &ctx->repos);
    ctx->repos_loaded = 1;
    return err;
}

static int collect_deps(const char *pkg, collect_ctx_t *ctx, flux_install_queue_t *queue, char visited[][FLUX_MAX_NAME_LEN], int *visited_count) {
    for (int i = 0; i < *visited_count; i++)
        if (strcmp(visited[i], pkg) == 0) return FLUX_ERR_NONE;

    if (*visited_count < FLUX_MAX_INSTALL_QUEUE) {
        strncpy(visited[*visited_count], pkg, FLUX_MAX_NAME_LEN - 1);
        (*visited_count)++;
    }

    if (!flux_is_kira_pkg(pkg)) {
        for (int i = 0; i < ctx->forbidden_count; i++) {
            if (strcmp(ctx->forbidden[i], pkg) == 0) {
                flux_err("'%s' conflicts with another package already required by this install", pkg);
                return FLUX_ERR_DEPENDENCY;
            }
        }

        if (ensure_alpine_repos(ctx) != FLUX_ERR_NONE) {
            flux_err("failed to load Alpine package index");
            return FLUX_ERR_NETWORK;
        }

        // a dependency name may itself be a virtual capability (e.g. "ninja" is provided by "samurai", not a real package) - same fallback alpine_resolve_deps() already uses for a package's own D: tokens
        const alpine_pkg_t *p = alpine_repos_find_by_name(&ctx->repos, pkg, NULL);
        if (!p) p = alpine_repos_find_provider(&ctx->repos, pkg, NULL);
        if (!p) {
            flux_err("no package found for '%s'", pkg);
            return FLUX_ERR_NOT_FOUND;
        }
        const char *real_name = p->name; // queue/install by the real package name, not whatever capability name was asked for

        // kira-base's bootstrap layer (musl, static busybox, ...) is never flux-managed - already satisfied, not installable
        if (strcmp(real_name, "musl") == 0 || strcmp(real_name, "busybox") == 0) return FLUX_ERR_NONE;

        char dep_names[ALPINE_MAX_RESOLVED_DEPS][ALPINE_MAX_NAME_LEN];
        int dep_count = 0;
        char conflicts[ALPINE_MAX_RESOLVED_DEPS][ALPINE_MAX_NAME_LEN];
        int conflict_count = 0;
        alpine_resolve_deps(&ctx->repos, p, dep_names, ALPINE_MAX_RESOLVED_DEPS, &dep_count,
                             conflicts, ALPINE_MAX_RESOLVED_DEPS, &conflict_count);

        for (int i = 0; i < conflict_count; i++) {
            if (queue_contains(queue, conflicts[i])) {
                flux_err("'%s' conflicts with '%s', already required by this install", pkg, conflicts[i]);
                return FLUX_ERR_DEPENDENCY;
            }
            if (ctx->forbidden_count < FLUX_MAX_INSTALL_QUEUE)
                strncpy(ctx->forbidden[ctx->forbidden_count++], conflicts[i], FLUX_MAX_NAME_LEN - 1);
        }

        for (int i = 0; i < dep_count; i++) {
            int err = collect_deps(dep_names[i], ctx, queue, visited, visited_count);
            if (err != FLUX_ERR_NONE) return err;
        }

        if (!queue_contains(queue, real_name) && queue->count < FLUX_MAX_INSTALL_QUEUE) {
            strncpy(queue->pkgs[queue->count].name, real_name, FLUX_MAX_NAME_LEN - 1);
            strncpy(queue->pkgs[queue->count].version, p->version, FLUX_MAX_VERSION_LEN - 1);
            queue->pkgs[queue->count].source = 'A';
            queue->count++;
        }
        return FLUX_ERR_NONE;
    }

    flux_config_t *config = ctx->config;
    char koto_path[FLUX_MAX_PATH_LEN * 2 + 16];
    snprintf(koto_path, sizeof(koto_path), "%s/%s/kotodama", config->local_repo_path, pkg);

    flux_recipe_t recipe;
    memset(&recipe, 0, sizeof(recipe));
    if (parse_kotodama(&recipe, koto_path) != FLUX_ERR_NONE) {
        flux_err("no recipe found for dependency '%s'", pkg);
        return FLUX_ERR_DEPENDENCY;
    }

    int has_source = (strlen(recipe.url) != 0);

    // g_force is only set for the root package, which is what makes a force-reinstall still walk its current deps and pick up ones a newer recipe version added (e.g. sleex gaining sleex-ui-kit).
    // a plain install (no -f) still walks past an installed dep whose recipe version moved on, same as the root package below - only a dep that's installed AND current gets skipped
    if (has_source && !g_force && is_installed_and_current(pkg, &recipe)) return FLUX_ERR_NONE;

    // decide whether THIS package needs its own build deps pulled in.
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
        char native_target[64];
        const char *cache_target;
        memset(cache_key, 0, sizeof(cache_key));
        if (flux_native_target(native_target, sizeof(native_target)) == FLUX_ERR_NONE) {
            cache_target = native_target;
        } else {
            cache_target = config->package_target;
        }
        if (flux_cache_key(recipe.name, recipe.version, recipe.cflags, cache_target, cache_key, sizeof(cache_key)) == FLUX_ERR_NONE) {
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
            int err = collect_deps(lists[l][i], ctx, queue, visited, visited_count);
            if (err != FLUX_ERR_NONE) return err;
        }
    }

    if (!queue_contains(queue, pkg) && queue->count < FLUX_MAX_INSTALL_QUEUE) {
        strncpy(queue->pkgs[queue->count].name, pkg, FLUX_MAX_NAME_LEN - 1);
        strncpy(queue->pkgs[queue->count].version, recipe.version, FLUX_MAX_VERSION_LEN - 1);
        queue->pkgs[queue->count].source = 'K';
        queue->count++;
    }

    return FLUX_ERR_NONE;
}

// one atomic "cp -a destdir/. /" - a per-file "find | while read; do cp; done" loop let one failed cp go unnoticed in practice, since a failing command inside a piped while-read subshell doesn't reliably abort the pipeline on every /bin/sh
static int copy_destdir_to_root(const char *destdir) {
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "cp -a \"%s\"/. /", destdir);
    return system(cmd) == 0 ? FLUX_ERR_NONE : FLUX_ERR_GENERAL;
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

// installs one resolved Alpine package name: no kotodama recipe, so no cache/build/hook machinery
static int try_alpine_install(const char *pkg, flux_config_t *config) {
    char arch[ALPINE_MAX_ARCH_LEN];
    if (alpine_arch_from_target(config->package_target, arch, sizeof(arch)) != FLUX_ERR_NONE) {
        flux_err("could not determine Alpine architecture from package_target");
        return FLUX_ERR_GENERAL;
    }

    alpine_repos_t local_repos;
    int own_repos = 0;
    alpine_repos_t *repos = g_active_repos;
    if (!repos) {
        if (alpine_repos_load(config, arch, &local_repos) != FLUX_ERR_NONE) {
            flux_err("failed to load Alpine package index");
            return FLUX_ERR_NETWORK;
        }
        repos = &local_repos;
        own_repos = 1;
    }

    const char *found_repo = NULL;
    const alpine_pkg_t *p = alpine_repos_find_by_name(repos, pkg, &found_repo);
    alpine_pkg_t found;
    memset(&found, 0, sizeof(found));
    if (p) {
        found = *p;
        // repos may be freed below; these point into its raw_text and are never needed past this line
        found.depends_raw = NULL;
        found.provides_raw = NULL;
    }
    if (own_repos) alpine_repos_free(repos);
    if (!p) {
        flux_err("no recipe found for '%s'", pkg);
        return FLUX_ERR_NOT_FOUND;
    }

    // defense in depth: alpine_index_load() already rejects unsafe name/version at parse time, this
    // guards the value actually being used to build shell commands below regardless of where it came from
    if (!alpine_name_is_safe(found.name) || !alpine_name_is_safe(found.version)) {
        flux_err("'%s' has unsafe characters in its Alpine name/version, refusing", pkg);
        return FLUX_ERR_SOURCE;
    }

    if (flux_db_is_installed(found.name)) {
        flux_pkg_info_t info;
        if (flux_db_read_info(found.name, &info) == FLUX_ERR_NONE && strcmp(info.source, "alpine") != 0) {
            // same name, different provenance (e.g. kotodama's own "musl" vs Alpine's "musl") - not a safe self-upgrade
            if (!g_force) {
                flux_err("'%s' is already installed from %s, not alpine - refusing to replace it silently (use -f to override)", found.name, info.source);
                return FLUX_ERR_GENERAL;
            }
            flux_warn("'%s' was installed from %s, forcing replacement with the Alpine package of the same name", found.name, info.source);
        } else if (!g_force && strcmp(info.version, found.version) == 0) {
            if (!g_auto_installed) flux_db_set_auto_installed(found.name, 0);
            flux_ok("%s is already installed", found.name);
            return FLUX_ERR_NONE;
        }
    }

    if (!g_auto_installed)
        flux_action("Installing %s %s", found.name, found.version);
    else
        flux_step("installing dependency: %s %s", found.name, found.version);

    char apk_path[512];
    flux_step("fetching source...");
    if (alpine_apk_download(config, found_repo, arch, found.name, found.version, apk_path, sizeof(apk_path)) != FLUX_ERR_NONE) {
        flux_err("failed to download %s", found.name);
        return FLUX_ERR_NETWORK;
    }

    char sig_path[256], control_path[256], data_path[256];
    if (alpine_apk_split_members(apk_path, sig_path, control_path, data_path, sizeof(sig_path)) != FLUX_ERR_NONE) {
        flux_err("malformed .apk for %s", found.name);
        return FLUX_ERR_SOURCE;
    }

    flux_step("verifying signature...");
    if (alpine_verify_signature(control_path, sig_path, ALPINE_KEYS_DIR) != FLUX_ERR_NONE) {
        return FLUX_ERR_CACHE;
    }

    char control_extract_dir[256];
    snprintf(control_extract_dir, sizeof(control_extract_dir), "/tmp/flux-build/%s-apk-control", found.name);
    if (alpine_apk_extract(control_path, control_extract_dir) != FLUX_ERR_NONE) {
        flux_err("failed to extract control member for %s", found.name);
        return FLUX_ERR_GENERAL;
    }

    char destdir[256];
    snprintf(destdir, sizeof(destdir), "/tmp/flux-build/%s-apk-destdir", found.name);
    flux_step("extracting...");
    if (alpine_apk_extract(data_path, destdir) != FLUX_ERR_NONE) {
        flux_err("failed to extract %s", found.name);
        return FLUX_ERR_GENERAL;
    }

    char (*installed_files)[FLUX_MAX_PATH_LEN] = malloc((size_t)FLUX_MAX_INSTALLED_FILES * FLUX_MAX_PATH_LEN);
    const char **file_ptrs = malloc((size_t)FLUX_MAX_INSTALLED_FILES * sizeof(char *));
    if (!installed_files || !file_ptrs) {
        free(installed_files);
        free(file_ptrs);
        flux_err("out of memory");
        return FLUX_ERR_GENERAL;
    }
    int file_count = 0;
    if (collect_files_from_destdir(destdir, installed_files, file_ptrs, &file_count) != FLUX_ERR_NONE) {
        flux_err("failed to list %s's files", found.name);
        free(installed_files);
        free(file_ptrs);
        return FLUX_ERR_GENERAL;
    }

    // checked, and any trigger scripts surfaced, BEFORE anything touches the real root
    char conflict_owner[FLUX_MAX_NAME_LEN], conflict_path[FLUX_MAX_PATH_LEN];
    if (flux_check_file_conflicts(found.name, file_ptrs, file_count,
                                   conflict_owner, sizeof(conflict_owner),
                                   conflict_path, sizeof(conflict_path)) != FLUX_ERR_NONE) {
        flux_err("'%s' conflicts with already-installed '%s' over %s", found.name, conflict_owner, conflict_path);
        free(installed_files);
        free(file_ptrs);
        return FLUX_ERR_GENERAL;
    }

    // third-party scripts, unlike kotodama's %post-install (Kira-authored, runs silently) - get explicit consent first
    int triggers_approved = 0;
    char trigger_bodies[ALPINE_TRIGGER_COUNT][FLUX_MAX_HOOK_LEN];
    memset(trigger_bodies, 0, sizeof(trigger_bodies));

    if (alpine_has_triggers(control_extract_dir)) {
        for (int i = 0; i < ALPINE_TRIGGER_COUNT; i++)
            alpine_read_trigger_script(control_extract_dir, alpine_trigger_script_name(i), trigger_bodies[i], FLUX_MAX_HOOK_LEN);

        flux_warn("%s ships scripts that will run as root on install:", found.name);
        for (int i = 0; i < ALPINE_TRIGGER_COUNT; i++) {
            if (trigger_bodies[i][0] == '\0') continue;
            printf("\n--- %s ---\n%s\n", alpine_trigger_script_name(i), trigger_bodies[i]);
        }

        if (g_yes) {
            triggers_approved = 1;
        } else {
            printf("Run these scripts? [y/N] ");
            fflush(stdout);
            char answer[8] = {0};
            triggers_approved = fgets(answer, sizeof(answer), stdin) && (answer[0] == 'y' || answer[0] == 'Y');
        }

        if (!triggers_approved) {
            flux_err("declined to run %s's install scripts, aborting install", found.name);
            free(installed_files);
            free(file_ptrs);
            return FLUX_ERR_GENERAL;
        }

        if (trigger_bodies[0][0] != '\0') {
            flux_step("running .pre-install...");
            if (alpine_run_trigger_script(trigger_bodies[0]) != 0) {
                flux_err(".pre-install failed for %s", found.name);
                free(installed_files);
                free(file_ptrs);
                return FLUX_ERR_BUILD;
            }
        }
    }

    flux_step("installing to system...");
    if (copy_destdir_to_root(destdir) != FLUX_ERR_NONE) {
        flux_err("failed to copy files to system");
        free(installed_files);
        free(file_ptrs);
        return FLUX_ERR_GENERAL;
    }

    if (triggers_approved) {
        const int post_indices[2] = { 1, 2 }; // .post-install, .trigger
        for (int j = 0; j < 2; j++) {
            int i = post_indices[j];
            if (trigger_bodies[i][0] == '\0') continue;
            flux_step("running %s...", alpine_trigger_script_name(i));
            if (alpine_run_trigger_script(trigger_bodies[i]) != 0)
                flux_warn("%s failed for %s, continuing", alpine_trigger_script_name(i), found.name);
        }
    }

    flux_pkg_info_t info;
    memset(&info, 0, sizeof(info));
    strncpy(info.name,    found.name,    FLUX_MAX_NAME_LEN - 1);
    strncpy(info.version, found.version, FLUX_MAX_VERSION_LEN - 1);
    strncpy(info.source,  "alpine",      sizeof(info.source) - 1);
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(info.install_date, sizeof(info.install_date), "%Y-%m-%d %H:%M:%S", t);
    info.auto_installed = g_auto_installed;
    flux_db_register(&info, file_ptrs, file_count);
    free(installed_files);
    free(file_ptrs);

    char cleanup[2048];
    snprintf(cleanup, sizeof(cleanup), "rm -rf \"%s\" \"%s\" \"%s\" \"%s\" \"%s\" \"%s\"",
             destdir, control_extract_dir, apk_path, sig_path, control_path, data_path);
    system(cleanup);

    flux_ok("%s v%s installed", found.name, found.version);
    return FLUX_ERR_NONE;
}

static void build_queue_row(const flux_queue_entry_t *entry, flux_table_row_t *row) {
    strncpy(row->col1, entry->name, FLUX_MAX_NAME_LEN - 1);
    strncpy(row->col2, entry->version, FLUX_MAX_VERSION_LEN - 1);
}

// prints the resolved queue and asks to proceed; 1 = proceed (including -y), 0 = user declined
static int confirm_queue(flux_install_queue_t *queue) {
    flux_table_row_t rows[FLUX_MAX_INSTALL_QUEUE];
    for (int i = 0; i < queue->count; i++)
        build_queue_row(&queue->pkgs[i], &rows[i]);

    char title[64];
    snprintf(title, sizeof(title), "%d package%s will be installed", queue->count, queue->count == 1 ? "" : "s");
    printf("\n");
    flux_print_table(title, rows, queue->count);
    printf("\nProceed? [Y/n] ");
    fflush(stdout);

    if (g_yes) {
        printf("Y\n");
        return 1;
    }
    char answer[8] = {0};
    if (fgets(answer, sizeof(answer), stdin) && (answer[0] == 'n' || answer[0] == 'N')) {
        printf("Aborted.\n");
        return 0;
    }
    return 1;
}

// installs a resolved queue in order, recursing per entry so each re-routes through the kira-*/Alpine check
static int install_resolved_queue(flux_install_queue_t *queue, alpine_repos_t *repos) {
    int saved_force = g_force;
    g_force = 0; // deps are never force-reinstalled, only the root package is
    g_active_repos = repos;
    int overall_err = FLUX_ERR_NONE;

    for (int i = 0; i < queue->count; i++) {
        g_auto_installed = (i < queue->count - 1) ? 1 : 0;
        g_skip_deps = 1;
        char *one_argv[] = { queue->pkgs[i].name };
        int err = flux_install(1, one_argv, "flux install <pkg>");
        g_skip_deps = 0;
        if (err != FLUX_ERR_NONE) {
            flux_err("failed to install '%s'", queue->pkgs[i].name);
            overall_err = (i == queue->count - 1) ? err : FLUX_ERR_DEPENDENCY;
            break;
        }
    }

    g_auto_installed = 0;
    g_active_repos = NULL;
    g_force = saved_force;
    return overall_err;
}

int flux_install(int argc, char **argv, const char *usage) {
    while (argc >= 1 && argv[0][0] == '-') {
        if (strcmp(argv[0], "-y") == 0)
            g_yes = 1;
        else if (strcmp(argv[0], "-f") == 0 || strcmp(argv[0], "--force") == 0)
            g_force = 1;
        else if (strcmp(argv[0], "--flatpak") == 0)
            g_flatpak = 1;
        else {
            flux_usage_error(usage);
            return FLUX_ERR_USAGE;
        }
        argv++;
        argc--;
    }

    if (argc < 1) {
        flux_usage_error(usage);
        return FLUX_ERR_USAGE;
    }

    const char *pkg = argv[0];

    // load config
    flux_config_t config;
    memset(&config, 0, sizeof(config));
    int err = flux_load_config(&config);
    if (err != FLUX_ERR_NONE) return err;

    // explicit opt-in only, never an automatic fallback - skips kotodama/Alpine resolution entirely
    if (g_flatpak) {
        int fp_err = try_flatpak_fallback(pkg);
        if (fp_err == FLUX_ERR_NOT_FOUND) {
            flux_err("no match on Flathub for '%s'", pkg);
            return FLUX_ERR_NOT_FOUND;
        }
        return fp_err;
    }

    // more than one package name on a top-level call (not one of install_batch's
    // own recursive single-package calls, which always pass argc == 1) - resolve
    // and install all of them together instead of only ever looking at argv[0]
    if (argc > 1 && !g_skip_deps)
        return install_batch(argc, argv, usage, &config);

    // name alone decides the source: kira-* is always kotodama, everything
    // else is always resolved against Alpine - no recipe-existence probing
    if (!flux_is_kira_pkg(pkg)) {
        // one of install_resolved_queue's own recursive calls - deps already resolved, just install this entry
        if (g_skip_deps)
            return try_alpine_install(pkg, &config);

        // a plain Alpine name has its own deps too, so it gets the same resolve-then-install-queue treatment as a kira-* root below
        flux_install_queue_t queue;
        memset(&queue, 0, sizeof(queue));
        char visited[FLUX_MAX_INSTALL_QUEUE][FLUX_MAX_NAME_LEN];
        memset(visited, 0, sizeof(visited));
        int visited_count = 0;

        collect_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.config = &config;

        int err2 = collect_deps(pkg, &ctx, &queue, visited, &visited_count);
        if (err2 != FLUX_ERR_NONE) { alpine_repos_free(&ctx.repos); return err2; }

        if (queue.count > 1 && !confirm_queue(&queue)) {
            alpine_repos_free(&ctx.repos);
            return FLUX_ERR_NONE;
        }

        int result = install_resolved_queue(&queue, &ctx.repos);
        alpine_repos_free(&ctx.repos);
        return result;
    }

    struct stat st;
    if (stat(config.local_repo_path, &st) != 0) {
        flux_err("recipe repo not found at %s", config.local_repo_path);
        flux_err("hint: run 'flux update' to download the recipe repo");
        return FLUX_ERR_GENERAL;
    }

    // parse recipe
    char koto_path[FLUX_MAX_PATH_LEN * 2];
    snprintf(koto_path, sizeof(koto_path), "%s/%s/kotodama", config.local_repo_path, pkg);
    if (stat(koto_path, &st) != 0) {
        flux_err("no recipe found for '%s'", pkg);
        return FLUX_ERR_NOT_FOUND;
    }

    flux_recipe_t recipe;
    memset(&recipe, 0, sizeof(recipe));
    err = parse_kotodama(&recipe, koto_path);
    if (err != FLUX_ERR_NONE) return err;

    if (!g_auto_installed)
        flux_action("Installing %s %s", recipe.name, recipe.version);
    else
        flux_step("installing dependency: %s %s", recipe.name, recipe.version);

    char recipe_dir[FLUX_MAX_PATH_LEN * 2 + 16];
    snprintf(recipe_dir, sizeof(recipe_dir), "%s/%s", config.local_repo_path, pkg);

    int has_source = (strlen(recipe.url) != 0);
    int has_install_hook = (strlen(recipe.hook_install) != 0);
    // pure meta-package: no source to fetch and no install hook to run
    int pure_meta = !has_source && !has_install_hook;

    // meta-packages are never marked installed; they're always re-walked so their deps and hooks can pick up changes.
    // a plain install (no -f) still proceeds when the recipe version has moved past what's installed - only an
    // installed package already at the current version is skipped; -f still forces a rebuild at the same version too
    if (has_source && flux_db_is_installed(pkg) && !g_force) {
        if (is_installed_and_current(pkg, &recipe)) {
            if (!g_auto_installed) flux_db_set_auto_installed(pkg, 0);
            flux_ok("%s is already installed", pkg);
            return FLUX_ERR_NONE;
        }
        flux_pkg_info_t old_info;
        if (flux_db_read_info(pkg, &old_info) == FLUX_ERR_NONE)
            flux_step("updating %s: %s -> %s", pkg, old_info.version, recipe.version);
    }

    // meta-packages never touch the binary cache
    char destdir[256];
    snprintf(destdir, sizeof(destdir), "/tmp/flux-build/%s-destdir", pkg);
    char cache_key[256];
    memset(cache_key, 0, sizeof(cache_key));
    char cache_path[FLUX_MAX_PATH_LEN];
    int cache_hit = 0;

    if (has_source) {
        char native_target[64];
        const char *cache_target;
        if (flux_native_target(native_target, sizeof(native_target)) == FLUX_ERR_NONE) {
            cache_target = native_target;
        } else {
            cache_target = config.package_target;
        }
        if (flux_cache_key(recipe.name, recipe.version, recipe.cflags, cache_target, cache_key, sizeof(cache_key)) == FLUX_ERR_NONE) {
            if (flux_cache_lookup(cache_key, cache_path, sizeof(cache_path)) == FLUX_ERR_NONE) {
                flux_step("cache hit: %s", cache_path);
                if (flux_cache_verify(cache_path, config.flux_pub_path) == FLUX_ERR_NONE) {
                    char cmd[1024];
                    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\" && zstd -d \"%s\" -o /tmp/flux_cache_extract.tar && tar -C \"%s\" -xf /tmp/flux_cache_extract.tar && rm /tmp/flux_cache_extract.tar", destdir, cache_path, destdir);
                    if (system(cmd) == 0)
                        cache_hit = 1;
                }
                if (!cache_hit)
                    flux_warn("cache verification failed, falling back to source");
            } else {
                flux_step("cache miss, building from source");
            }
        } else {
            flux_warn("cache key generation failed, building from source");
        }
    }

    if (!g_skip_deps) {
        flux_install_queue_t queue;
        memset(&queue, 0, sizeof(queue));
        char visited[FLUX_MAX_INSTALL_QUEUE][FLUX_MAX_NAME_LEN];
        memset(visited, 0, sizeof(visited));
        int visited_count = 0;

        collect_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.config = &config;

        int err2 = collect_deps(pkg, &ctx, &queue, visited, &visited_count);
        if (err2 != FLUX_ERR_NONE) { alpine_repos_free(&ctx.repos); return err2; }

        if ((queue.count > 1 || (queue.count == 1 && strcmp(queue.pkgs[0].name, pkg) != 0))
            && !confirm_queue(&queue)) {
            alpine_repos_free(&ctx.repos);
            return FLUX_ERR_NONE;
        }

        int saved_force = g_force;
        g_force = 0;          /* deps are never force-reinstalled, only the root package is */
        g_auto_installed = 1;
        g_active_repos = &ctx.repos;
        for (int i = 0; i < queue.count - 1; i++) {
            // this queue is already the fully-resolved transitive closure,
            // cycles included (e.g. elogind <-> polkit) - re-resolving from
            // here would give each recursive call its own fresh visited set,
            // and a genuine cycle would recurse forever each half trying to
            // install the other first. g_skip_deps makes the nested call
            // just install this one entry instead of resolving again.
            g_skip_deps = 1;
            char *dep_argv[] = { queue.pkgs[i].name };
            int dep_err = flux_install(1, dep_argv, "flux install <pkg>");
            g_skip_deps = 0;
            if (dep_err != FLUX_ERR_NONE) {
                flux_err("failed to install dependency '%s'", queue.pkgs[i].name);
                g_auto_installed = 0;
                g_active_repos = NULL;
                g_force = saved_force;
                alpine_repos_free(&ctx.repos);
                return FLUX_ERR_DEPENDENCY;
            }
        }
        g_auto_installed = 0;
        g_active_repos = NULL;
        g_force = saved_force;
        alpine_repos_free(&ctx.repos);
    }

    if (cache_hit) {
        flux_step("installing to system...");
        if (copy_destdir_to_root(destdir) != FLUX_ERR_NONE) {
            flux_err("failed to copy cached files to system");
            return FLUX_ERR_GENERAL;
        }

        // heap, not stack: at FLUX_MAX_INSTALLED_FILES=32768 this is 8MB, and
        // flux_install recurses for dependencies - stacking that per call
        // is exactly what blew the stack installing elogind+polkit together
        char (*installed_files)[FLUX_MAX_PATH_LEN] = malloc((size_t)FLUX_MAX_INSTALLED_FILES * FLUX_MAX_PATH_LEN);
        const char **file_ptrs = malloc((size_t)FLUX_MAX_INSTALLED_FILES * sizeof(char *));
        if (!installed_files || !file_ptrs) {
            free(installed_files);
            free(file_ptrs);
            flux_err("out of memory");
            return FLUX_ERR_GENERAL;
        }
        int file_count = 0;
        collect_files_from_destdir(destdir, installed_files, file_ptrs, &file_count);

        flux_pkg_info_t info;
        memset(&info, 0, sizeof(info));
        strncpy(info.name,    recipe.name,    FLUX_MAX_NAME_LEN - 1);
        strncpy(info.version, recipe.version, FLUX_MAX_VERSION_LEN - 1);
    strncpy(info.source,  "kotodama",      sizeof(info.source) - 1);
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        strftime(info.install_date, sizeof(info.install_date), "%Y-%m-%d %H:%M:%S", t);
        info.auto_installed = g_auto_installed;
        flux_db_register(&info, file_ptrs, file_count);
        free(installed_files);
        free(file_ptrs);

        char cleanup[512];
        snprintf(cleanup, sizeof(cleanup), "rm -rf \"%s\"", destdir);
        system(cleanup);

        if (run_post_install_hook(recipe.hook_post_install, recipe_dir) != 0) {
            flux_err("post-install failed");
            return FLUX_ERR_BUILD;
        }

        flux_ok("%s v%s installed", pkg, recipe.version);
        return FLUX_ERR_NONE;
    }

    if (pure_meta) {
        if (run_post_install_hook(recipe.hook_post_install, recipe_dir) != 0) {
            flux_err("post-install failed");
            return FLUX_ERR_BUILD;
        }

        flux_pkg_info_t info;
        memset(&info, 0, sizeof(info));
        strncpy(info.name,    recipe.name,    FLUX_MAX_NAME_LEN - 1);
        strncpy(info.version, recipe.version, FLUX_MAX_VERSION_LEN - 1);
    strncpy(info.source,  "kotodama",      sizeof(info.source) - 1);
        time_t now_m = time(NULL);
        struct tm *t_m = localtime(&now_m);
        strftime(info.install_date, sizeof(info.install_date), "%Y-%m-%d %H:%M:%S", t_m);
        info.auto_installed = g_auto_installed;
        flux_db_register(&info, NULL, 0);
        flux_ok("%s v%s installed", pkg, recipe.version);
        return FLUX_ERR_NONE;
    }

    char build_dir[256];
    char tarball[512];

    snprintf(build_dir, sizeof(build_dir), "/tmp/flux-build/%s", pkg);
    tarball[0] = '\0';

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p /tmp/flux-build");
    system(cmd);

    int is_git = (has_source && strncmp(recipe.url, "git+", 4) == 0);

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
        char clone_cmd[2048];
        if (strlen(git_branch) > 0) {
            snprintf(clone_cmd, sizeof(clone_cmd),
                     "rm -rf \"%s\" && git clone --depth=1 --recurse-submodules --shallow-submodules --branch \"%s\" \"%s\" \"%s\"",
                     build_dir, git_branch, git_url, build_dir);
        } else {
            snprintf(clone_cmd, sizeof(clone_cmd),
                     "rm -rf \"%s\" && git clone --depth=1 --recurse-submodules --shallow-submodules \"%s\" \"%s\"",
                     build_dir, git_url, build_dir);
        }
        if (system(clone_cmd) != 0) {
            flux_err("failed to clone git repository");
            return FLUX_ERR_NETWORK;
        }

        if (strlen(recipe.sha256) > 0) {
            flux_step("verifying commit...");
            char sha_cmd[640];
            snprintf(sha_cmd, sizeof(sha_cmd),
                     "git -C \"%s\" rev-parse HEAD | tr -d '\\n' > /tmp/flux_hash_actual", build_dir);
            system(sha_cmd);
            FILE *hf = fopen("/tmp/flux_hash_actual", "r");
            if (!hf) return FLUX_ERR_GENERAL;
            char actual[65] = {0};
            fread(actual, 1, 64, hf);
            fclose(hf);
            remove("/tmp/flux_hash_actual");
            if (strcmp(actual, recipe.sha256) != 0) {
                flux_err("commit mismatch (expected %s, got %s)", recipe.sha256, actual);
                return FLUX_ERR_GENERAL;
            }
        }
    } else if (has_source) {
        const char *url_basename = strrchr(recipe.url, '/');
        url_basename = url_basename ? url_basename + 1 : recipe.url;
        snprintf(tarball, sizeof(tarball), "/tmp/flux-build/%s", url_basename);

        if (stat(tarball, &st) == 0 && st.st_size > 0) {
            // install_batch's download phase already pulled this one in as
            // part of the batch's aggregated progress bar
            flux_step("using pre-fetched source...");
        } else {
            flux_step("fetching source...");
            if (fetch_source(recipe.url, tarball) != 0) {
                flux_err("failed to fetch source");
                return FLUX_ERR_NETWORK;
            }
        }

        flux_step("verifying checksum...");
        if (verify_sha256(tarball, recipe.sha256) != 0) {
            flux_err("checksum verification failed");
            return FLUX_ERR_GENERAL;
        }

        flux_step("extracting...");
        if (extract_tarball(tarball, build_dir) != 0) {
            flux_err("failed to extract tarball");
            return FLUX_ERR_GENERAL;
        }
    } else {
        snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", build_dir);
        system(cmd);
    }

    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", destdir);
    system(cmd);

    flux_step("running pre-build...");
    if (run_hook(recipe.hook_pre_build, build_dir, destdir, recipe_dir) != 0) {
        flux_err("pre-build failed");
        return FLUX_ERR_BUILD;
    }

    flux_step("building...");
    if (run_hook(recipe.hook_build, build_dir, destdir, recipe_dir) != 0) {
        flux_err("build failed");
        return FLUX_ERR_BUILD;
    }

    flux_step("running post-build...");
    if (run_hook(recipe.hook_post_build, build_dir, destdir, recipe_dir) != 0) {
        flux_err("post-build failed");
        return FLUX_ERR_BUILD;
    }

    flux_step("installing files...");
    if (run_hook(recipe.hook_install, build_dir, destdir, recipe_dir) != 0) {
        flux_err("install hook failed");
        return FLUX_ERR_BUILD;
    }

    flux_step("installing to system...");
    if (copy_destdir_to_root(destdir) != FLUX_ERR_NONE) {
        flux_err("failed to copy files to system");
        return FLUX_ERR_GENERAL;
    }

    // heap, not stack: at FLUX_MAX_INSTALLED_FILES=32768 this is 8MB, and
    // flux_install recurses for dependencies - stacking that per call is
    // exactly what blew the stack installing elogind+polkit together
    char (*installed_files)[FLUX_MAX_PATH_LEN] = malloc((size_t)FLUX_MAX_INSTALLED_FILES * FLUX_MAX_PATH_LEN);
    const char **file_ptrs = malloc((size_t)FLUX_MAX_INSTALLED_FILES * sizeof(char *));
    if (!installed_files || !file_ptrs) {
        free(installed_files);
        free(file_ptrs);
        flux_err("out of memory");
        return FLUX_ERR_GENERAL;
    }
    int file_count = 0;
    collect_files_from_destdir(destdir, installed_files, file_ptrs, &file_count);

    flux_pkg_info_t info;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    memset(&info, 0, sizeof(info));
    strncpy(info.name,    recipe.name,    FLUX_MAX_NAME_LEN - 1);
    strncpy(info.version, recipe.version, FLUX_MAX_VERSION_LEN - 1);
    strncpy(info.source,  "kotodama",      sizeof(info.source) - 1);
    strftime(info.install_date, sizeof(info.install_date), "%Y-%m-%d %H:%M:%S", t);
    info.auto_installed = g_auto_installed;
    flux_db_register(&info, file_ptrs, file_count);
    free(installed_files);
    free(file_ptrs);

    if (strlen(cache_key) > 0)
        flux_cache_store(cache_key, destdir, config.flux_secret_key_path);

    char cleanup_cmd[1024];
    if (strlen(tarball) > 0) {
        snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf \"%s\" \"%s\" \"%s\"", build_dir, destdir, tarball);
    } else {
        snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf \"%s\" \"%s\"", build_dir, destdir);
    }
    system(cleanup_cmd);

    if (run_post_install_hook(recipe.hook_post_install, recipe_dir) != 0) {
        flux_err("post-install failed");
        return FLUX_ERR_BUILD;
    }

    flux_ok("%s v%s installed", pkg, recipe.version);
    return FLUX_ERR_NONE;
}

// resolves + confirms + downloads + installs several top-level package names
// in one call: dependency resolution happens once for the combined,
// deduplicated set, every plain-tarball source gets pre-fetched behind one
// aggregated progress bar, then each queue member installs in dependency
// order via a recursive single-package call (g_skip_deps = 1, since the
// resolution/table/confirmation above already covers it)
static int install_batch(int argc, char **argv, const char *usage, flux_config_t *config) {
    (void)usage;

    flux_install_queue_t queue;
    memset(&queue, 0, sizeof(queue));
    char visited[FLUX_MAX_INSTALL_QUEUE][FLUX_MAX_NAME_LEN];
    memset(visited, 0, sizeof(visited));
    int visited_count = 0;

    collect_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.config = config;

    for (int i = 0; i < argc; i++) {
        if (flux_is_kira_pkg(argv[i])) {
            char koto_path[FLUX_MAX_PATH_LEN * 2];
            snprintf(koto_path, sizeof(koto_path), "%s/%s/kotodama", config->local_repo_path, argv[i]);
            struct stat st;
            if (stat(koto_path, &st) != 0) {
                flux_err("no recipe found for '%s'", argv[i]);
                alpine_repos_free(&ctx.repos);
                return FLUX_ERR_NOT_FOUND;
            }
        }
        int err = collect_deps(argv[i], &ctx, &queue, visited, &visited_count);
        if (err != FLUX_ERR_NONE) { alpine_repos_free(&ctx.repos); return err; }
    }

    if (queue.count == 0) {
        flux_ok("all packages are already installed");
        alpine_repos_free(&ctx.repos);
        return FLUX_ERR_NONE;
    }

    flux_table_row_t rows[FLUX_MAX_INSTALL_QUEUE];
    for (int i = 0; i < queue.count; i++) {
        strncpy(rows[i].col1, queue.pkgs[i].name, FLUX_MAX_NAME_LEN - 1);

        if (queue.pkgs[i].source == 'A') {
            strncpy(rows[i].col2, "alpine", FLUX_MAX_VERSION_LEN - 1);
            continue;
        }

        char kp[FLUX_MAX_PATH_LEN * 2 + 16];
        snprintf(kp, sizeof(kp), "%s/%s/kotodama", config->local_repo_path, queue.pkgs[i].name);
        flux_recipe_t r;
        memset(&r, 0, sizeof(r));
        parse_kotodama(&r, kp);

        flux_pkg_info_t old_info;
        if (flux_db_read_info(queue.pkgs[i].name, &old_info) == FLUX_ERR_NONE && strcmp(old_info.version, r.version) != 0)
            snprintf(rows[i].col2, sizeof(rows[i].col2), "%s -> %s", old_info.version, r.version);
        else
            strncpy(rows[i].col2, r.version, FLUX_MAX_VERSION_LEN - 1);
    }
    char title[64];
    snprintf(title, sizeof(title), "%d package%s will be installed",
             queue.count, queue.count == 1 ? "" : "s");
    printf("\n");
    flux_print_table(title, rows, queue.count);
    printf("\nProceed? [Y/n] ");
    fflush(stdout);
    if (g_yes) {
        printf("Y\n");
    } else {
        char answer[8] = {0};
        if (fgets(answer, sizeof(answer), stdin)) {
            if (answer[0] == 'n' || answer[0] == 'N') {
                printf("Aborted.\n");
                alpine_repos_free(&ctx.repos);
                return FLUX_ERR_NONE;
            }
        }
    }
    printf("\n");

    char native_target[64];
    const char *cache_target;
    if (flux_native_target(native_target, sizeof(native_target)) == FLUX_ERR_NONE) {
        cache_target = native_target;
    } else {
        cache_target = config->package_target;
    }

    flux_download_item_t downloads[FLUX_MAX_INSTALL_QUEUE];
    int download_count = 0;
    for (int i = 0; i < queue.count; i++) {
        if (queue.pkgs[i].source == 'A') continue;

        char kp[FLUX_MAX_PATH_LEN * 2 + 16];
        snprintf(kp, sizeof(kp), "%s/%s/kotodama", config->local_repo_path, queue.pkgs[i].name);
        flux_recipe_t r;
        memset(&r, 0, sizeof(r));
        parse_kotodama(&r, kp);

        int has_source = (strlen(r.url) != 0);
        int is_git = (has_source && strncmp(r.url, "git+", 4) == 0);
        if (!has_source || is_git) continue;

        char cache_key[256];
        char cache_path[FLUX_MAX_PATH_LEN];
        memset(cache_key, 0, sizeof(cache_key));
        if (flux_cache_key(r.name, r.version, r.cflags, cache_target, cache_key, sizeof(cache_key)) == FLUX_ERR_NONE &&
            flux_cache_lookup(cache_key, cache_path, sizeof(cache_path)) == FLUX_ERR_NONE) {
            continue; // cached already, nothing to fetch
        }

        const char *url_basename = strrchr(r.url, '/');
        url_basename = url_basename ? url_basename + 1 : r.url;
        strncpy(downloads[download_count].url, r.url, FLUX_MAX_URL_LEN - 1);
        snprintf(downloads[download_count].dest, sizeof(downloads[download_count].dest),
                 "/tmp/flux-build/%s", url_basename);
        download_count++;
    }

    if (download_count > 0) {
        system("mkdir -p /tmp/flux-build");
        if (flux_download_batch(downloads, download_count) != FLUX_ERR_NONE) {
            flux_err("failed to download sources");
            alpine_repos_free(&ctx.repos);
            return FLUX_ERR_NETWORK;
        }
    }

    int saved_force = g_force;
    int overall_err = FLUX_ERR_NONE;
    g_active_repos = &ctx.repos;
    for (int i = 0; i < queue.count; i++) {
        int is_root = 0;
        for (int j = 0; j < argc; j++) {
            if (strcmp(argv[j], queue.pkgs[i].name) == 0) { is_root = 1; break; }
        }
        g_auto_installed = !is_root;
        g_force = is_root ? saved_force : 0; // deps are never force-reinstalled, only roots are - same rule as the single-package path
        g_skip_deps = 1;
        char *one_argv[] = { queue.pkgs[i].name };
        int err = flux_install(1, one_argv, "flux install <pkg>");
        g_skip_deps = 0;
        if (err != FLUX_ERR_NONE) {
            flux_err("failed to install '%s'", queue.pkgs[i].name);
            overall_err = is_root ? err : FLUX_ERR_DEPENDENCY;
            break;
        }
    }
    g_auto_installed = 0;
    g_active_repos = NULL;
    g_force = saved_force;
    alpine_repos_free(&ctx.repos);
    return overall_err;
}
