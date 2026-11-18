#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "test.h"
#include "wwwpack.h"

#define SHA_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define SHA_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define SHA_0 "0000000000000000000000000000000000000000000000000000000000000000"

static uint8_t pack[512];

static size_t add(size_t o, const char *name, const char *plain, const char *sha, const char *data)
{
    pack[o++] = (uint8_t)strlen(name);
    memcpy(pack + o, name, strlen(name));
    o += strlen(name);
    pack[o++] = (uint8_t)strlen(plain);
    memcpy(pack + o, plain, strlen(plain));
    o += strlen(plain);
    memcpy(pack + o, sha, 64);
    o += 64;
    uint32_t n = (uint32_t)strlen(data);
    pack[o++] = n & 0xff;
    pack[o++] = n >> 8 & 0xff;
    pack[o++] = n >> 16 & 0xff;
    pack[o++] = n >> 24;
    memcpy(pack + o, data, n);
    return o + n;
}

static size_t sample(void)
{
    memcpy(pack, "JRWWW1\0\0\2\0\0\0", 12);
    size_t o = add(12, "app.js.gz", "app.js", SHA_A, "xyz");
    return add(o, "manual/img/home.png", "manual/img/home.png", SHA_B, "");
}

static void test_parse(void)
{
    size_t len = sample();
    CHECK_EQ(wwwpack_check(pack, len), 2);
    size_t off = 0;
    wwwpack_entry_t e;
    CHECK(wwwpack_next(pack, len, &off, &e));
    CHECK(strcmp(e.name, "app.js.gz") == 0 && strcmp(e.plain, "app.js") == 0 && strcmp(e.sha, SHA_A) == 0);
    CHECK(e.size == 3 && memcmp(e.data, "xyz", 3) == 0);
    CHECK(wwwpack_next(pack, len, &off, &e));
    CHECK(strcmp(e.name, "manual/img/home.png") == 0 && e.size == 0);
    CHECK(!wwwpack_next(pack, len, &off, &e));

    // Damage anywhere is refused: truncated, trailing bytes, wrong count, magic
    CHECK_EQ(wwwpack_check(pack, len - 1), -1);
    CHECK_EQ(wwwpack_check(pack, len + 1), -1);
    pack[8] = 3;
    CHECK_EQ(wwwpack_check(pack, len), -1);
    pack[8] = 2;
    pack[0] = 'X';
    CHECK_EQ(wwwpack_check(pack, len), -1);
    CHECK_EQ(wwwpack_check(pack, 4), -1);

    // A size past the end
    len = sample();
    pack[12 + 1 + 9 + 1 + 6 + 64 + 3] = 0x7f;
    CHECK_EQ(wwwpack_check(pack, len), -1);
    // Bad names never get to the SD card
    memcpy(pack, "JRWWW1\0\0\1\0\0\0", 12);
    CHECK_EQ(wwwpack_check(pack, add(12, "../config/settings.json", "x", SHA_A, "")), -1);
    CHECK_EQ(wwwpack_check(pack, add(12, "ok.js", "ok.js", "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", "")), -1);
}

static void test_names(void)
{
    CHECK(wwwpack_name_ok("index.html.gz"));
    CHECK(wwwpack_name_ok("manual/img/a-b_c.png"));
    CHECK(!wwwpack_name_ok(""));
    CHECK(!wwwpack_name_ok("/abs"));
    CHECK(!wwwpack_name_ok("a/../b"));
    CHECK(!wwwpack_name_ok("a//b"));
    CHECK(!wwwpack_name_ok("dir/"));
    CHECK(!wwwpack_name_ok("sp ace"));
    CHECK(wwwpack_name_ok("vendor/uplot-1.6.32/uPlot.esm.js.gz"));
    CHECK(!wwwpack_name_ok("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));  // 65
}

static void test_manifest(void)
{
    // Old manifest: lines for paths the install didn't write are kept (DuckDB, a file the pack
    // doesn't have); app.js gets its new hash; '#' lines go
    const char *old = SHA_B "  vendor/uplot.js\n" SHA_A "  app.js\n# deploy in progress\n" SHA_0 "  vendor/duckdb.wasm";
    char l1[] = SHA_B "  app.js", l2[] = SHA_A "  index.html";
    char *lines[] = {l1, l2};
    char out[1024];
    size_t n = wwwpack_manifest(old, lines, 2, out, sizeof(out));
    const char *want = SHA_0 "  vendor/duckdb.wasm\n" SHA_A "  index.html\n" SHA_B "  app.js\n" SHA_B "  vendor/uplot.js\n";
    CHECK(strcmp(out, want) == 0);
    CHECK_EQ(n, strlen(want));
    CHECK_EQ(wwwpack_manifest(NULL, lines, 2, out, sizeof(out)), (64 + 2 + 6 + 1) + (64 + 2 + 10 + 1));
    CHECK_EQ(wwwpack_manifest(old, lines, 2, out, 40), 0);  // doesn't fit
}

int main(void)
{
    test_parse();
    test_names();
    test_manifest();
    printf("wwwpack: %d failures\n", test_failures);
    return test_failures != 0;
}
