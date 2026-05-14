#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/parser.h"
#include "../include/util.h"

int parse_kotodama(flux_recipe_t *recipe, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return FLUX_ERR_KOTODAMA;

    char line[512];
    koto_state_t state = KOTO_NONE;
    int bdep_i = 0;
    int rdep_i = 0;

    while (fgets(line, sizeof(line), f)) {
        strip_newline(line);
        char *trimmed = trim_left(line);

        // skip empty lines and comments
        if (*trimmed == '\0' || *trimmed == '#') continue;

        // detect section headers
        if (strcmp(trimmed, "[meta]") == 0) { state = KOTO_META; continue; }
        if (strcmp(trimmed, "[source]") == 0) { state = KOTO_SOURCE; continue; }
        if (strcmp(trimmed, "[deps]") == 0) { state = KOTO_DEPS; continue; }
        if (strcmp(trimmed, "[build]") == 0) { state = KOTO_BUILD; continue; }

        // detect hook headers
        if (strcmp(trimmed, "%pre-build") == 0) { state = KOTO_HOOK_PRE_BUILD; continue; }
        if (strcmp(trimmed, "%build") == 0) { state = KOTO_HOOK_BUILD; continue; }
        if (strcmp(trimmed, "%post-build") == 0) { state = KOTO_HOOK_POST_BUILD; continue; }
        if (strcmp(trimmed, "%install") == 0) { state = KOTO_HOOK_INSTALL; continue; }

        // hook content: append line to the right buffer
        if (state == KOTO_HOOK_PRE_BUILD) {
            strncat(recipe->hook_pre_build, trimmed, FLUX_MAX_HOOK_LEN - strlen(recipe->hook_pre_build) - 1);
            strncat(recipe->hook_pre_build, "\n", FLUX_MAX_HOOK_LEN - strlen(recipe->hook_pre_build) - 1);
            continue;
        }
        if (state == KOTO_HOOK_BUILD) {
            strncat(recipe->hook_build, trimmed, FLUX_MAX_HOOK_LEN - strlen(recipe->hook_build) - 1);
            strncat(recipe->hook_build, "\n", FLUX_MAX_HOOK_LEN - strlen(recipe->hook_build) - 1);
            continue;
        }
        if (state == KOTO_HOOK_POST_BUILD) {
            strncat(recipe->hook_post_build, trimmed, FLUX_MAX_HOOK_LEN - strlen(recipe->hook_post_build) - 1);
            strncat(recipe->hook_post_build, "\n", FLUX_MAX_HOOK_LEN - strlen(recipe->hook_post_build) - 1);
            continue;
        }
        if (state == KOTO_HOOK_INSTALL) {
            strncat(recipe->hook_install, trimmed, FLUX_MAX_HOOK_LEN - strlen(recipe->hook_install) - 1);
            strncat(recipe->hook_install, "\n", FLUX_MAX_HOOK_LEN - strlen(recipe->hook_install) - 1);
            continue;
        }

        // key = value parsing
        char *eq = strchr(trimmed, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = trimmed;
        char *val = eq + 1;
        trim_right(key);
        key = trim_left(key);
        val = trim_left(val);
        trim_right(val);

        if (state == KOTO_META) {
            if (strcmp(key, "name") == 0) strncpy(recipe->name, val, FLUX_MAX_NAME_LEN - 1);
            if (strcmp(key, "version") == 0) strncpy(recipe->version, val, FLUX_MAX_VERSION_LEN - 1);
            if (strcmp(key, "description") == 0) strncpy(recipe->description, val, FLUX_MAX_DESC_LEN - 1);
            if (strcmp(key, "license") == 0) strncpy(recipe->license, val, FLUX_MAX_LICENSE_LEN - 1);
            if (strcmp(key, "size") == 0) recipe->size = atoi(val);
        }
        if (state == KOTO_SOURCE) {
            if (strcmp(key, "url") == 0) strncpy(recipe->url, val, FLUX_MAX_URL_LEN - 1);
            if (strcmp(key, "sha256") == 0) strncpy(recipe->sha256, val, FLUX_MAX_SHA256_LEN - 1);
        }
        if (state == KOTO_BUILD) {
            if (strcmp(key, "cflags") == 0) strncpy(recipe->cflags, val, FLUX_MAX_CFLAGS_LEN - 1);
            if (strcmp(key, "ldflags") == 0) strncpy(recipe->ldflags, val, FLUX_MAX_LDFLAGS_LEN - 1);
        }
        if (state == KOTO_DEPS) {
            if (strcmp(key, "build") == 0) {
                char tmp[512];
                strncpy(tmp, val, sizeof(tmp) - 1);
                char *tok = strtok(tmp, " ");
                while (tok && bdep_i < FLUX_MAX_DEPS) {
                    strncpy(recipe->deps[bdep_i++], tok, FLUX_MAX_NAME_LEN - 1);
                    tok = strtok(NULL, " ");
                }
            }
            if (strcmp(key, "runtime") == 0) {
                char tmp[512];
                strncpy(tmp, val, sizeof(tmp) - 1);
                char *tok = strtok(tmp, " ");
                while (tok && rdep_i < FLUX_MAX_RDEPS) {
                    strncpy(recipe->rdeps[rdep_i++], tok, FLUX_MAX_NAME_LEN - 1);
                    tok = strtok(NULL, " ");
                }
            }
        }
    }

    fclose(f);
    return FLUX_ERR_NONE;
}