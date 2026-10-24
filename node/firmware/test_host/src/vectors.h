/* Test vectors from protocol/vectors (generated into vectors_gen.c at build time). */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *key;      /* flattened field name, e.g. "op_code", "device_info.dmx_footprint", "sw_out[2]" */
    int is_number;        /* 1: num valid (ints, bools); 0: str valid */
    long long num;
    const char *str;
} pm_vec_field_t;

typedef struct {
    const char *protocol; /* "artnet" | "rdm" | "image" */
    const char *packet;   /* "ArtPoll", "RDM", "DUB_RESPONSE", ... */
    const char *name;
    int valid;
    const uint8_t *bytes;
    size_t len;
    const pm_vec_field_t *fields;
    size_t nfields;
} pm_vec_t;

extern const pm_vec_t pm_vectors[];
extern const size_t pm_vectors_count;

/* Lookup helpers (NULL / 0 when missing). */
const pm_vec_field_t *vec_field(const pm_vec_t *v, const char *key);
int vec_has(const pm_vec_t *v, const char *key);
long long vec_num(const pm_vec_t *v, const char *key);
const char *vec_str(const pm_vec_t *v, const char *key);
/* Decode a lowercase/uppercase hex string into buf, return length (or -1). */
int vec_hex(const char *hex, uint8_t *buf, size_t cap);
