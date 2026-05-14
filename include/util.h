#ifndef UTIL_H
#define UTIL_H

#include "flux.h"

void flux_usage_error(const char *usage);
int flux_load_config(flux_config_t *config);
void strip_newline(char *s);
char *trim_left(char *s);
void trim_right(char *s);

int flux_db_register(const flux_pkg_info_t *info, const char **files, int file_count);
int flux_db_is_installed(const char *name);
int flux_db_remove(const char *name);

#endif