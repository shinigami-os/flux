#ifndef UTIL_H
#define UTIL_H

#include "flux.h"
#include <stddef.h>

void flux_usage_error(const char *usage);

int flux_load_config(flux_config_t *config);
void strip_newline(char *s);
char *trim_left(char *s);
void trim_right(char *s);

int flux_db_register(const flux_pkg_info_t *info, const char **files, int file_count);
int flux_db_is_installed(const char *name);
int flux_db_remove(const char *name);
int flux_db_read_info(const char *name, flux_pkg_info_t *info);
int flux_db_set_auto_installed(const char *name, int auto_installed);
int flux_db_list_installed(char names[][FLUX_MAX_NAME_LEN], int max, int *count);
int flux_recipe_depends_on(const char *recipe_name, const char *dep_name, const flux_config_t *config);
int flux_autoremove_orphans(int *removed_count);

int flux_cache_key(const char *name, const char *version, const char *cflags, const char *target, char *out, size_t outlen);
int flux_cache_lookup(const char *key, char *path_out, size_t path_outlen);
int flux_cache_store(const char *key, const char *destdir, const char *secret_key_path);
int flux_cache_verify(const char *path, const char *pub_path);

int flux_fetch_latest_git_tag(const char *repo_url, char *out, size_t outlen);

int flux_colors_enabled(void);
void flux_log(const char *fmt, ...);
void flux_ok(const char *fmt, ...);
void flux_warn(const char *fmt, ...);
void flux_err(const char *fmt, ...);

#endif