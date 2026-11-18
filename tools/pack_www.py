#!/usr/bin/env python3
"""Packs the web app into the firmware (components/web, www_install.c installs it on the SD card).

  tools/pack_www.py <repo> <out.pack>

The same files as tools/web_deploy.sh, without DuckDB (7.5 MB gzipped, only for the SQL console,
changes only with its pinned version: web_deploy.sh / sd_deploy.sh put it there) and without version.txt (the
firmware writes its own version). html/js/mjs/css/wasm are gzipped like on the SD card.

Format (little endian, parsed by components/wwwpack):
  "JRWWW1\\0\\0", u32 count, then per file:
  u8 len + served name (e.g. app.js.gz), u8 len + plain name (app.js, as in deploy-manifest.txt),
  64 hex chars sha256 of the plain file, u32 size, data
"""
import gzip
import hashlib
import os
import struct
import sys

GZ = ('.html', '.js', '.mjs', '.css', '.wasm')


def files(repo):
    www = os.path.join(repo, 'www', 'stats')
    for d, dirs, names in os.walk(www):
        rel_d = os.path.relpath(d, www)
        if rel_d.replace(os.sep, '/').startswith('vendor/duckdb'):
            continue
        for n in names:
            rel = os.path.normpath(os.path.join(rel_d, n)).replace(os.sep, '/')
            if n.startswith('.') or n.startswith('._') or rel == 'version.txt':
                continue
            yield rel, os.path.join(d, n)
    yield 'manual/index.html', os.path.join(repo, 'docs', 'manual.html')
    img = os.path.join(repo, 'docs', 'img')
    for n in sorted(os.listdir(img)):
        if n.endswith('.png') and not n.startswith('.'):
            yield 'manual/img/' + n, os.path.join(img, n)


def main():
    repo, out = sys.argv[1], sys.argv[2]
    entries = []
    for plain, path in sorted(files(repo)):
        with open(path, 'rb') as f:
            data = f.read()
        sha = hashlib.sha256(data).hexdigest().encode()
        served = plain
        if plain.endswith(GZ):
            data, served = gzip.compress(data, 9, mtime=0), plain + '.gz'
        if len(served) > 64:
            sys.exit(f'{served}: name longer than 64 characters (the device refuses it)')
        entries.append(struct.pack('<B', len(served)) + served.encode() + struct.pack('<B', len(plain)) +
                       plain.encode() + sha + struct.pack('<I', len(data)) + data)
    blob = b'JRWWW1\0\0' + struct.pack('<I', len(entries)) + b''.join(entries)
    tmp = out + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(blob)
    os.replace(tmp, out)
    print(f'web app pack: {len(entries)} files, {len(blob) // 1024} KiB')


if __name__ == '__main__':
    main()
