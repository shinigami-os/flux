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

typedef int (*flux_cmd_fn)(int argc, char **argv, const char *usage);

typedef struct {
    const char *name;
    flux_cmd_fn handler;
    const char *desc;
    const char *usage;
} flux_cmd_t;

#endif