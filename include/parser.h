#ifndef PARSER_H
#define PARSER_H

#include "flux.h"

int parse_kotodama(flux_recipe_t *config, const char *path);

typedef enum {
    KOTO_NONE,
    KOTO_META,
    KOTO_SOURCE,
    KOTO_DEPS,
    KOTO_BUILD,
    KOTO_HOOK_PRE_BUILD,
    KOTO_HOOK_BUILD,
    KOTO_HOOK_POST_BUILD,
    KOTO_HOOK_INSTALL,
} koto_state_t;

#endif