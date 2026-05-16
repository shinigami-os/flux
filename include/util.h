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

int flux_cache_key(const char *name, const char *version, const char *cflags, char *out, size_t outlen);
int flux_cache_lookup(const char *key, char *path_out, size_t path_outlen);
int flux_cache_store(const char *key, const char *destdir, const char *secret_key_path);
int flux_cache_verify(const char *path, const char *pub_path);

#endif