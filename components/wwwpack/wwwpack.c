#include "wwwpack.h"

#include <stdlib.h>
#include <string.h>

#define MAGIC "JRWWW1\0\0"
#define MAGIC_LEN 8

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

bool wwwpack_name_ok(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n > 64 || name[0] == '/' || name[n - 1] == '/' || strstr(name, "..") || strstr(name, "//")) {
        return false;
    }
    for (const char *c = name; *c; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '.' ||
              *c == '_' || *c == '-' || *c == '/')) {
            return false;
        }
    }
    return true;
}

static bool str_field(const uint8_t *buf, size_t len, size_t *off, char *out)
{
    if (*off + 1 > len) {
        return false;
    }
    size_t n = buf[*off];
    if (n == 0 || n > 64 || *off + 1 + n > len) {
        return false;
    }
    memcpy(out, buf + *off + 1, n);
    out[n] = 0;
    *off += 1 + n;
    return strlen(out) == n;  // no NUL inside
}

bool wwwpack_next(const uint8_t *buf, size_t len, size_t *off, wwwpack_entry_t *e)
{
    if (*off == 0) {
        if (len < MAGIC_LEN + 4 || memcmp(buf, MAGIC, MAGIC_LEN) != 0) {
            return false;
        }
        *off = MAGIC_LEN + 4;
    }
    size_t o = *off;
    if (!str_field(buf, len, &o, e->name) || !str_field(buf, len, &o, e->plain) || o + 64 + 4 > len) {
        return false;
    }
    memcpy(e->sha, buf + o, 64);
    e->sha[64] = 0;
    for (int i = 0; i < 64; i++) {
        char c = e->sha[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    o += 64;
    e->size = le32(buf + o);
    o += 4;
    if (e->size > len - o || !wwwpack_name_ok(e->name) || !wwwpack_name_ok(e->plain)) {
        return false;
    }
    e->data = buf + o;
    *off = o + e->size;
    return true;
}

int wwwpack_check(const uint8_t *buf, size_t len)
{
    if (len < MAGIC_LEN + 4 || memcmp(buf, MAGIC, MAGIC_LEN) != 0) {
        return -1;
    }
    uint32_t count = le32(buf + MAGIC_LEN);
    size_t off = 0;
    wwwpack_entry_t e;
    for (uint32_t i = 0; i < count; i++) {
        if (!wwwpack_next(buf, len, &off, &e)) {
            return -1;
        }
    }
    return off == len && count <= 1000 ? (int)count : -1;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static bool put(char *out, size_t cap, size_t *n, const char *s, size_t len)
{
    if (*n + len + 1 >= cap) {
        return false;
    }
    memcpy(out + *n, s, len);
    *n += len;
    out[(*n)++] = '\n';
    out[*n] = 0;
    return true;
}

size_t wwwpack_manifest(const char *old, char **new_lines, int n_new, char *out, size_t cap)
{
    // All lines in one list (the old manifest's vendor/ lines copied), then one sort
    int n_old = 0;
    for (const char *p = old ? old : ""; (p = strchr(p, '\n')); p++) {
        n_old++;
    }
    n_old++;  // a last line without '\n'
    char **all = malloc(sizeof(*all) * (size_t)(n_old + n_new));
    char **copies = malloc(sizeof(*copies) * (size_t)n_old);
    int n_copies = 0;
    if (!all || !copies || cap == 0) {
        free(all);
        free(copies);
        return 0;
    }
    int n_all = 0;
    for (const char *p = old ? old : ""; *p;) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        // "<64 hex>  <path>", a path the install didn't write
        bool replaced = false;
        for (int i = 0; i < n_new && !replaced && len > 66; i++) {
            const char *path = strlen(new_lines[i]) > 66 ? new_lines[i] + 66 : "";
            replaced = strlen(path) == len - 66 && strncmp(path, p + 66, len - 66) == 0;
        }
        if (!replaced && len > 66 && len < 200 && p[0] != '#' && p[64] == ' ' && p[65] == ' ') {
            char *line = malloc(len + 1);
            if (line) {
                memcpy(line, p, len);
                line[len] = 0;
                all[n_all++] = copies[n_copies++] = line;
            }
        }
        p += len + (end ? 1 : 0);
    }
    for (int i = 0; i < n_new; i++) {
        all[n_all++] = new_lines[i];
    }
    qsort(all, (size_t)n_all, sizeof(*all), cmp_str);
    size_t n = 0;
    bool ok = true;
    out[0] = 0;
    for (int i = 0; i < n_all; i++) {
        ok = ok && put(out, cap, &n, all[i], strlen(all[i]));
    }
    for (int i = 0; i < n_copies; i++) {
        free(copies[i]);
    }
    free(copies);
    free(all);
    return ok ? n : 0;
}
