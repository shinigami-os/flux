#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "../include/alpine.h"
#include "../include/util.h"

static const char *TRIGGER_NAMES[ALPINE_TRIGGER_COUNT] = { ".pre-install", ".post-install", ".trigger" };

const char *alpine_trigger_script_name(int index) {
    if (index < 0 || index >= ALPINE_TRIGGER_COUNT) return "";
    return TRIGGER_NAMES[index];
}

int alpine_has_triggers(const char *control_extract_dir) {
    for (int i = 0; i < ALPINE_TRIGGER_COUNT; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", control_extract_dir, TRIGGER_NAMES[i]);
        struct stat st;
        if (stat(path, &st) == 0) return 1;
    }
    return 0;
}

int alpine_read_trigger_script(const char *control_extract_dir, const char *script_name, char *body, size_t body_len) {
    body[0] = '\0';

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", control_extract_dir, script_name);

    struct stat st;
    if (stat(path, &st) != 0) return FLUX_ERR_NONE;

    FILE *f = fopen(path, "r");
    if (!f) return FLUX_ERR_GENERAL;
    size_t n = fread(body, 1, body_len - 1, f);
    fclose(f);
    body[n] = '\0';
    return FLUX_ERR_NONE;
}

int alpine_run_trigger_script(const char *body) {
    return flux_run_script(body, NULL, 0);
}
