#ifndef FLUX_H
#define FLUX_H

// release-based, matches Kira's own scheme: YY.MM, optionally -N for a hotfix (e.g. 26.06-1)
#define FLUX_VERSION "26.09"
#define FLUX_REPO_URL "https://github.com/shinigami-os/flux"
#define FLUX_RECIPES_REPO_URL "https://github.com/shinigami-os/flux-recipes"
// matches flux-recipes' actual GitHub default branch - only needed for the no-git tarball fallback below, git clone/pull just follow the default on their own
#define FLUX_RECIPES_BRANCH "kira-only"
#define KIRA_BASE_REPO_URL "https://github.com/shinigami-os/kira-base"

#define FLUX_ERR_NONE 0
#define FLUX_ERR_GENERAL 1
#define FLUX_ERR_USAGE 2
#define FLUX_ERR_NOT_FOUND 3
#define FLUX_ERR_DEPENDENCY 4
#define FLUX_ERR_BUILD 5
#define FLUX_ERR_CACHE 6
#define FLUX_ERR_NETWORK 7
#define FLUX_ERR_PERMISSION 8
#define FLUX_ERR_CONTAINER 9
#define FLUX_ERR_SOURCE 10
#define FLUX_ERR_KOTODAMA 11


#define FLUX_MAX_DEPS        64
#define FLUX_MAX_RDEPS       64
#define FLUX_MAX_NAME_LEN    64
#define FLUX_MAX_HOOK_LEN    4096
#define FLUX_MAX_VERSION_LEN 32
#define FLUX_MAX_DESC_LEN    256
#define FLUX_MAX_LICENSE_LEN 64
#define FLUX_MAX_URL_LEN     256
#define FLUX_MAX_SHA256_LEN  65
#define FLUX_MAX_CFLAGS_LEN  1024
#define FLUX_MAX_LDFLAGS_LEN 1024
#define FLUX_MAX_PATH_LEN    256
// heap-allocated wherever it's used (see cmd_install.c), not stack - several
// already-installed packages were silently hitting the old 4096 cap
#define FLUX_MAX_INSTALLED_FILES 32768
// also doubles as the cap on flux_db_list_installed() - a kira-desktop-* meta-package's Alpine deps alone can put a real system's installed count well past the old 256
#define FLUX_MAX_INSTALL_QUEUE 1024


typedef int (*flux_cmd_fn)(int argc, char **argv, const char *usage);

typedef struct {
    const char *name;
    flux_cmd_fn handler;
    const char *desc;
    const char *usage;
} flux_cmd_t;

typedef struct {
    char local_repo_path[FLUX_MAX_PATH_LEN];
    char remote_repo_url[FLUX_MAX_PATH_LEN];
    char binary_cache_url[FLUX_MAX_URL_LEN];
    char default_build_flags[FLUX_MAX_CFLAGS_LEN];
    char flux_pub_path[FLUX_MAX_PATH_LEN];
    char flux_secret_key_path[FLUX_MAX_PATH_LEN];
    char flux_cross_compile_prefix[FLUX_MAX_PATH_LEN];
    char flux_cross_compile_sysroot[FLUX_MAX_PATH_LEN];
    char flux_cross_toolchain_path[FLUX_MAX_PATH_LEN];
    char flux_cross_gcc_libpath[FLUX_MAX_PATH_LEN];
    char package_target[64];
    char alpine_mirror_url[FLUX_MAX_URL_LEN];
    char alpine_branch[32];
}flux_config_t;

typedef struct {
    char name[FLUX_MAX_NAME_LEN];
    char version[FLUX_MAX_VERSION_LEN];
    char description[FLUX_MAX_DESC_LEN];
    char license[FLUX_MAX_LICENSE_LEN];
    int size;
    // zero-default means "stage into the cross sysroot", matching pre-existing recipes
    int no_sysroot_stage;
    char url[FLUX_MAX_URL_LEN];
    char sha256[FLUX_MAX_SHA256_LEN];
    char cflags[FLUX_MAX_CFLAGS_LEN];
    char ldflags[FLUX_MAX_LDFLAGS_LEN];
    char rdeps[FLUX_MAX_RDEPS][FLUX_MAX_NAME_LEN];
    char deps[FLUX_MAX_DEPS][FLUX_MAX_NAME_LEN];
    char hook_pre_build[FLUX_MAX_HOOK_LEN];
    char hook_build[FLUX_MAX_HOOK_LEN];
    char hook_post_build[FLUX_MAX_HOOK_LEN];
    char hook_install[FLUX_MAX_HOOK_LEN];
    // runs only during install, against the real root, never cached
    char hook_post_install[FLUX_MAX_HOOK_LEN];
}flux_recipe_t;

typedef struct {
    char name[FLUX_MAX_NAME_LEN];
    char version[FLUX_MAX_VERSION_LEN];
    char install_date[32];
    int  auto_installed; // 1 = pulled in as dep, 0 = explicitly installed
    // "kotodama" or "alpine"; empty on-disk means a pre-v2 entry, treated as kotodama everywhere it's displayed
    char source[16];
} flux_pkg_info_t;

typedef struct {
    char name[FLUX_MAX_NAME_LEN];
    char version[FLUX_MAX_VERSION_LEN]; // resolved at queue-build time, so the confirmation table needs no second lookup
    char source; // 'K' = kotodama, 'A' = alpine
} flux_queue_entry_t;

typedef struct {
    flux_queue_entry_t pkgs[FLUX_MAX_INSTALL_QUEUE];
    int  count;
} flux_install_queue_t;

#endif
