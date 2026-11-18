// The web app packed into the firmware by tools/pack_www.py (format there). Pure C.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char name[65];   // as served from the SD card, e.g. "app.js.gz"
    char plain[65];  // as listed in deploy-manifest.txt, e.g. "app.js"
    char sha[65];    // sha256 of the plain file, hex
    const uint8_t *data;
    uint32_t size;
} wwwpack_entry_t;

// Number of files, or -1 if the pack is damaged anywhere (checked to the end, names included)
int wwwpack_check(const uint8_t *buf, size_t len);
// Next file from *off (start at 0); false at the end or on damage
bool wwwpack_next(const uint8_t *buf, size_t len, size_t *off, wwwpack_entry_t *e);
// Letters, digits, '._-/', 1..64, no "..", no leading or doubled '/'
bool wwwpack_name_ok(const char *name);

// deploy-manifest.txt after an install: the old manifest's lines for paths not in new_lines
// (DuckDB stays on the card; '#' lines dropped) plus new_lines ("<sha>  <path>"), sorted bytewise like tools/web_deploy.sh (LC_ALL=C sort).
// Returns the length written (0 if it doesn't fit).
size_t wwwpack_manifest(const char *old, char **new_lines, int n_new, char *out, size_t cap);
