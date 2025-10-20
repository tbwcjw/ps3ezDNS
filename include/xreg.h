#ifndef XREG_H
#define XREG_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t file_offset;
    uint16_t unk_key;
    uint16_t key_length;
    uint8_t  key_type;
    char    *key_string;
} xreg_key_t;

typedef struct {
    uint32_t file_offset;
    uint16_t unk_value_1;
    uint16_t key_offset;
    uint16_t unk_value_2;
    uint16_t value_length;
    uint8_t  value_type;   // 0=bool, 1=int, 2=string
    uint8_t *value_data;
} xreg_value_t;

typedef struct {
    uint8_t       *buffer;
    size_t         size;
    xreg_key_t    *keys;
    size_t         nkeys;
    xreg_value_t  *values;
    size_t         nvalues;
} xreg_registry_t;

xreg_registry_t *xreg_load(const char *path);
int xreg_save(const xreg_registry_t *reg, const char *path);

const xreg_key_t   *xreg_find_key(const xreg_registry_t *reg, const char *name);
xreg_value_t       *xreg_find_value_by_key(const xreg_registry_t *reg, const xreg_key_t *key);

int xreg_update_value(xreg_registry_t *reg,
                      const char *key_name,
                      int expected_type,
                      const void *new_data,
                      size_t new_len);

void xreg_free(xreg_registry_t *reg);

#ifdef __cplusplus
}
#endif

#endif // XREG_H