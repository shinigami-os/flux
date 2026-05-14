#ifndef FLUX_H
#define FLUX_H


#define FLUX_ERR_NONE 0 // success
#define FLUX_ERR_GENERAL 1 // general unrecoverable error
#define FLUX_ERR_USAGE 2 // Usage error (bad command, missing argument)
#define FLUX_ERR_NOT_FOUND 3 // Package not found
#define FLUX_ERR_DEPENDENCY 4 // Dependency resolution failure
#define FLUX_ERR_BUILD 5 // Build failure
#define FLUX_ERR_CACHE 6 // Cache error
#define FLUX_ERR_NETWORK 7 // Network error
#define FLUX_ERR_PERMISSION 8 // Permission error (needs root)
#define FLUX_ERR_CONTAINER 9 // Compat container error
#define FLUX_ERR_SOURCE 10 // Invalid or unavailable source
#define FLUX_ERR_KOTODAMA 11 // malformed Kotodama file


#define FLUX_MAX_DEPS        20
#define FLUX_MAX_RDEPS       10
#define FLUX_MAX_NAME_LEN    64
#define FLUX_MAX_HOOK_LEN    4096
#define FLUX_MAX_VERSION_LEN 32
#define FLUX_MAX_DESC_LEN    256
#define FLUX_MAX_LICENSE_LEN 64
#define FLUX_MAX_URL_LEN     256
#define FLUX_MAX_SHA256_LEN  64
#define FLUX_MAX_CFLAGS_LEN  1024
#define FLUX_MAX_LDFLAGS_LEN 1024
#define FLUX_MAX_PATH_LEN    256
#define FLUX_MAX_INSTALLED_FILES 4096


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
}flux_config_t;

typedef struct {
    char name[FLUX_MAX_NAME_LEN];
    char version[FLUX_MAX_VERSION_LEN];
    char description[FLUX_MAX_DESC_LEN];
    char license[FLUX_MAX_LICENSE_LEN];
    int size;
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
}flux_recipe_t;

typedef struct {
    char name[FLUX_MAX_NAME_LEN];
    char version[FLUX_MAX_VERSION_LEN];
    char install_date[32];
    int  auto_installed; // 1 = pulled in as dep, 0 = explicitly installed
} flux_pkg_info_t;

#endif