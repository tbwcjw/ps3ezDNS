#ifndef CSV_H
#define CSV_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIELDS_CAPACITY 3  //max number of fields
#define ROW_CAPACITY    20 //max number of rows

typedef struct {
    char **fields;
    size_t count;
} CSVRow;

typedef struct {
    CSVRow *rows;
    size_t count;
} CSVTable;

static inline char *csv_strdup(const char *s, size_t len) {
    char *out = malloc(len + 1);
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static inline ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
    if (!lineptr || !n || !stream) return -1;
    int c;
    size_t i = 0;

    if (*lineptr == NULL || *n == 0) {
        *n = 128;
        *lineptr = malloc(*n);
        if (!*lineptr) return -1;
    }

    while ((c = fgetc(stream)) != EOF) {
        if (i + 1 >= *n) {
            *n *= 2;
            *lineptr = realloc(*lineptr, *n);
            if (!*lineptr) return -1;
        }
        (*lineptr)[i++] = c;
        if (c == '\n') break;
    }

    if (i == 0) return -1; // EOF
    (*lineptr)[i] = '\0';
    return i;
}

static inline CSVRow csv_parse_line(const char *line, char delimiter) {
    CSVRow row = {NULL, 0};
    size_t capacity = FIELDS_CAPACITY;
    row.fields = malloc(capacity * sizeof(char *));

    const char *p = line;
    while (*p) {
        if (row.count >= capacity) {
            capacity *= 2;
            row.fields = realloc(row.fields, capacity * sizeof(char *));
        }

        char *field;
        if (*p == '"') {
            p++;
            const char *start = p;
            while (*p && !(*p == '"' && (*(p + 1) == delimiter || *(p + 1) == '\0'))) {
                if (*p == '"' && *(p + 1) == '"') p++; // skip escaped quote
                p++;
            }
            field = csv_strdup(start, p - start);
            if (*p == '"') p++;
        } else {
            const char *start = p;
            while (*p && *p != delimiter) p++;
            field = csv_strdup(start, p - start);
        }

        row.fields[row.count++] = field;
        if (*p == delimiter) p++;
    }

    return row;
}

/* Free a single row */
static inline void csv_free_row(CSVRow *row) {
    for (size_t i = 0; i < row->count; i++) free(row->fields[i]);
    free(row->fields);
}

static inline int csv_create(const char *filename, char **header, size_t count, char delimiter) {
    FILE *fp = fopen(filename, "r");
    if (fp) {
        fclose(fp);
        return 1; // file exists
    }

    fp = fopen(filename, "w"); // Create new
    if (!fp) return 0;
    
    if (header && count > 0) {
        for (size_t i = 0; i < count; i++) {
            size_t len = strcspn(header[i], "\r\n");
            fputs(header[i], fp);
            if (i < count - 1) fputc(delimiter, fp);
        }
        fputc('\n', fp);
    }
    fclose(fp);
    return 1;
}

static inline int csv_append_row(const char *filename, char **fields, size_t count, char delimiter) {
    FILE *fp = fopen(filename, "a");
    if (!fp) return 0;
    for (size_t i = 0; i < count; i++) {
        const char *field = fields[i];
        int needs_quotes = strchr(field, delimiter) || strchr(field, '"') || strchr(field, '\n');
        if (needs_quotes) fputc('"', fp);
        for (const char *c = field; *c; c++) {
            if (*c == '"') fputc('"', fp); // escape quotes
            fputc(*c, fp);
        }
        if (needs_quotes) fputc('"', fp);
        if (i < count - 1) fputc(delimiter, fp);
    }
    fputc('\n', fp);
    fclose(fp);
    return 1;
}

static inline CSVTable csv_read_all(const char *filename, char delimiter) {
    CSVTable table = {NULL, 0};
    FILE *fp = fopen(filename, "r");
    if (!fp) return table;
    size_t capacity = ROW_CAPACITY;
    table.rows = malloc(capacity * sizeof(CSVRow));
    char *line = NULL;
    size_t len = 0;
    ssize_t read;

    while ((read = getline(&line, &len, fp)) != -1) {
        if (table.count >= capacity) {
            capacity *= 2;
            table.rows = realloc(table.rows, capacity * sizeof(CSVRow));
        }
        // trim newline
        if (read > 0 && (line[read - 1] == '\n' || line[read - 1] == '\r'))
            line[--read] = '\0';
        table.rows[table.count++] = csv_parse_line(line, delimiter);
    }

    free(line);
    fclose(fp);
    return table;
}

static inline void csv_free_table(CSVTable *table) {
    for (size_t i = 0; i < table->count; i++)
        csv_free_row(&table->rows[i]);
    free(table->rows);
    table->rows = NULL;
    table->count = 0;
}

static inline int csv_remove_row(const char *filename, const int column, const char *target,char delimiter) {
    CSVTable table = csv_read_all(filename, delimiter);
    if(!table.rows || table.count == 0) return 0;

    CSVTable filtered = { malloc(table.count * sizeof(CSVRow)), 0};
    int removed = 0;

    for(size_t i = 0; i < table.count; i++) {
        CSVRow *row = &table.rows[i];

        if(i == 0) { //preserve header
            filtered.rows[filtered.count++] = *row;
            continue;
        }

        if(column < (int)row->count && strcmp(row->fields[column], target) == 0) {
            removed = 1; //found
            continue;
        }

        filtered.rows[filtered.count++] = *row;
    }

    FILE *fp = fopen(filename, "w"); //truncate
    if(!fp) {
        free(filtered.rows);
        return 0;
    }
    fclose(fp);

    for (size_t i = 0; i < filtered.count; i++) {
        csv_append_row(filename, filtered.rows[i].fields, filtered.rows[i].count, delimiter);
    }

    free(filtered.rows);
    free(table.rows);  // free if dynamically allocated

    return removed;
}

#endif // CSV_H
