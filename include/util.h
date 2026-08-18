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

int flux_native_target(char *out, size_t outlen);
int flux_is_archive_name(const char *name);
int flux_extract_source(const char *fetched_path, const char *dest);
int flux_cache_key(const char *name, const char *version, const char *cflags, const char *target, char *out, size_t outlen);
int flux_cache_lookup(const char *key, char *path_out, size_t path_outlen);
int flux_cache_lookup_local(const char *key, char *path_out, size_t path_outlen);
int flux_cache_store(const char *key, const char *destdir, const char *secret_key_path);
int flux_cache_verify(const char *path, const char *pub_path);

int flux_fetch_latest_git_tag(const char *repo_url, char *out, size_t outlen);
int flux_fetch_latest_kernel_version(const char *cache_url, char *out, size_t outlen);

int flux_colors_enabled(void);
void flux_log(const char *fmt, ...);
void flux_ok(const char *fmt, ...);
void flux_warn(const char *fmt, ...);
void flux_err(const char *fmt, ...);

// UI toolkit: consistent styled output across every command, nala-style
void flux_action(const char *fmt, ...);   // bold top-level header, e.g. "Installing nouveau-firmware 20260810"
void flux_step(const char *fmt, ...);     // indented in-progress sub-step under an action
int  flux_term_width(void);               // current terminal width, or 72 if not a tty
void flux_rule(void);                     // a full-width horizontal divider

typedef struct {
    char col1[FLUX_MAX_NAME_LEN];
    char col2[FLUX_MAX_VERSION_LEN * 2 + 8]; // wide enough for "old -> new" version pairs, not just a single version
} flux_table_row_t;

// prints a bordered two-column table (e.g. package name / version)
void flux_print_table(const char *title, const flux_table_row_t *rows, int count);

double flux_now_seconds(void);            // monotonic-ish wall clock for elapsed-time summaries

int flux_download(const char *url, const char *dest); // silences curl's own meter, draws a styled progress bar instead

#endif