#include "xreg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_SIZE_EXPECTED 0x40000u
#define AREA_SIZE 0x10000u
#define HEADER_SIZE 0x10u
#define KEYS_AREA_OFFSET 0x0u
#define VALUES_AREA_OFFSET AREA_SIZE

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

static int within_bounds(size_t off, 
                        size_t need, 
                        size_t total) {
    return (off + need) <= total;
}

// AA BB CC DD EE 00 00
static int is_end_marker(const uint8_t *p, size_t avail) {
    const uint8_t marker[7] = {0xAA,0xBB,0xCC,0xDD,0xEE,0x00,0x00};
    if (avail < 7) return 0;
    return memcmp(p, marker, 7) == 0;
}

static xreg_key_t *parse_keys(const uint8_t *file, 
                                size_t file_size, 
                                size_t *out_count) {
    size_t start = KEYS_AREA_OFFSET + HEADER_SIZE;
    size_t end   = KEYS_AREA_OFFSET + AREA_SIZE;
    if (end > file_size) end = file_size;

    size_t pos = start;
    size_t capacity = 64;
    size_t count = 0;
    xreg_key_t *keys = calloc(capacity, sizeof(xreg_key_t));
    if (!keys) return NULL;

    while (pos + 1 < end) {
        size_t avail = end - pos;
        const uint8_t *p = file + pos;

        if (is_end_marker(p, avail)) break;

        if (!within_bounds(pos, 5, end)) break;

        uint16_t unk_key = be16(p + 0);
        uint16_t key_length = be16(p + 2);
        uint8_t key_type = p[4];

        size_t string_pos = pos + 5;
        if (!within_bounds(string_pos, key_length + 1, end)) break;
        
        //copy and null terminatie
        char *str = malloc(key_length + 1);
        if (!str) break;
        memcpy(str, file + string_pos, key_length);
        str[key_length] = '\0';
        
        //dynamic allocation
        if (count >= capacity) {
            capacity *= 2;
            xreg_key_t *tmp = realloc(keys, capacity * sizeof(xreg_key_t));
            if (!tmp) break;
            keys = tmp;
        }

        keys[count].file_offset = (uint32_t)pos;
        keys[count].unk_key = unk_key;
        keys[count].key_length = key_length;
        keys[count].key_type = key_type;
        keys[count].key_string = str;
        ++count;

        //move header 5 + string + null term
        pos += 5 + key_length + 1;
    }

    *out_count = count;
    return keys;
}

static xreg_value_t *parse_values(const uint8_t *file, 
                                    size_t file_size, 
                                    size_t *out_count) {
    size_t start = VALUES_AREA_OFFSET;
    size_t end   = VALUES_AREA_OFFSET + AREA_SIZE;
    if (end > file_size) end = file_size;

    size_t pos = start;
    size_t capacity = 128;
    size_t count = 0;
    xreg_value_t *vals = calloc(capacity, sizeof(xreg_value_t));
    if (!vals) return NULL;

    while (pos + 1 < end) {
        size_t avail = end - pos;
        const uint8_t *p = file + pos;

        if (is_end_marker(p, avail)) break;

        if (!within_bounds(pos, 9, end)) break;

        uint16_t unk1 = be16(p + 0);
        uint16_t key_offset = be16(p + 2);
        uint16_t unk2 = be16(p + 4);
        uint16_t value_length = be16(p + 6);
        uint8_t value_type = p[8];

        size_t data_pos = pos + 9;
        if (!within_bounds(data_pos, value_length + 1, end)) break;

        uint8_t *data = malloc(value_length);
        if (!data) break;
        memcpy(data, file + data_pos, value_length);

        if (count >= capacity) { //grow vec
            capacity *= 2;
            xreg_value_t *tmp = realloc(vals, capacity * sizeof(xreg_value_t));
            if (!tmp) break;
            vals = tmp;
        }

        vals[count].file_offset = (uint32_t)pos;
        vals[count].unk_value_1 = unk1;
        vals[count].key_offset = key_offset;
        vals[count].unk_value_2 = unk2;
        vals[count].value_length = value_length;
        vals[count].value_type = value_type;
        vals[count].value_data = data;
        ++count;
        
        //header + data + null terminattor
        pos += 9 + value_length + 1;
    }

    *out_count = count;
    return vals;
}

const xreg_key_t *xreg_find_key(const xreg_registry_t *reg, const char *name) {
    if (!reg || !name) return NULL;
    for (size_t i = 0; i < reg->nkeys; ++i) {
        if (strcmp(reg->keys[i].key_string, name) == 0)
            return &reg->keys[i];
    }
    return NULL;
}

xreg_value_t *xreg_find_value_by_key(const xreg_registry_t *reg, const xreg_key_t *key) {
    if (!reg || !key) return NULL;
    for (size_t i = 0; i < reg->nvalues; ++i) {
        uint32_t abs_off = reg->values[i].key_offset + HEADER_SIZE;
        if (abs_off == key->file_offset)
            return (xreg_value_t *)&reg->values[i];
    }
    return NULL;
}

static int update_value_in_buffer(uint8_t *buf, size_t buf_size, xreg_value_t *val,
                                  const void *new_data, size_t new_len) {
    size_t pos = val->file_offset + 9; //skip header
    if (!within_bounds(pos, val->value_length + 1, buf_size)) return 0;
    if (new_len > val->value_length) return 0; //cant grow in place

    //overwrite buffer
    memcpy(buf + pos, new_data, new_len);
    if (new_len < val->value_length)
        memset(buf + pos + new_len, 0, val->value_length - new_len); //padding

    //mirror in copy
    memcpy(val->value_data, new_data, new_len);
    if (new_len < val->value_length)
        memset(val->value_data + new_len, 0, val->value_length - new_len);

    return 1;
}

int xreg_update_value(xreg_registry_t *reg, const char *key_name,
                      int expected_type, const void *new_data, size_t new_len) {
    if (!reg || !key_name) return 0;
    const xreg_key_t *key = xreg_find_key(reg, key_name);
    if (!key) return 0;
    xreg_value_t *val = xreg_find_value_by_key(reg, key);
    if (!val) return 0;
    if (val->value_type != expected_type) return 0;
    return update_value_in_buffer(reg->buffer, reg->size, val, new_data, new_len);
}

xreg_registry_t *xreg_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    if (sz <= 0) { fclose(f); return NULL; }

    xreg_registry_t *reg = calloc(1, sizeof(*reg));
    reg->size = (size_t)sz;
    reg->buffer = malloc(reg->size);
    if (!reg->buffer) { free(reg); fclose(f); return NULL; }

    //probably truncated
    if (fread(reg->buffer, 1, reg->size, f) != reg->size) {
        free(reg->buffer);
        free(reg);
        fclose(f);
        return NULL;
    }
    fclose(f);

    reg->keys = parse_keys(reg->buffer, reg->size, &reg->nkeys);
    reg->values = parse_values(reg->buffer, reg->size, &reg->nvalues);

    return reg;
}

int xreg_save(const xreg_registry_t *reg, const char *path) {
    if (!reg || !path) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t written = fwrite(reg->buffer, 1, reg->size, f);
    fclose(f);
    return written == reg->size;
}

void xreg_free(xreg_registry_t *reg) {
    if (!reg) return;
    for (size_t i = 0; i < reg->nkeys; ++i) free(reg->keys[i].key_string);
    for (size_t i = 0; i < reg->nvalues; ++i) free(reg->values[i].value_data);
    free(reg->keys);
    free(reg->values);
    free(reg->buffer);
    free(reg);
}
