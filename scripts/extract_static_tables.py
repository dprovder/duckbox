#!/usr/bin/env python3
# Extract the track-independent pdb tables (columns/menu, colors, unknown 17/18,
# history header) from a real rekordbox export and emit them as a C++ include.
# These tables are byte-identical across exports; the CDJ firmware needs them to
# build its browse UI (an export.pdb without them shows nothing / no response).
#
#   python3 scripts/extract_static_tables.py [reference.pdb] > src/pdb_static_tables.inc
#
# Uses the same page/row layout as EmitPage() in pdb_writer.cpp (PAGE=4096,
# HEAP=0x28, num_rows = u24@24 & 0x1fff, backward row-index groups of 16).
import sys

PAGE = 4096
HEAP = 0x28
ref = sys.argv[1] if len(sys.argv) > 1 else "test/reference/export.pdb"
d = open(ref, "rb").read()


def table_ptrs():
    nt = int.from_bytes(d[8:12], "little")
    po, t = 0x1c, {}
    for _ in range(nt):
        typ = int.from_bytes(d[po:po + 4], "little")
        f = int.from_bytes(d[po + 8:po + 12], "little")
        l = int.from_bytes(d[po + 12:po + 16], "little")
        t[typ] = (f, l)
        po += 16
    return t


def extract(typ):
    f, l = table_ptrs()[typ]
    rows = []
    for pi in range(f, l + 1):
        b = d[pi * PAGE:(pi + 1) * PAGE]
        nrows = int.from_bytes(b[24:27], "little") & 0x1fff
        used = int.from_bytes(b[30:32], "little")
        present = []
        for r in range(nrows):
            g, j = divmod(r, 16)
            base = PAGE - g * 0x24
            if (int.from_bytes(b[base - 4:base - 2], "little") >> j) & 1:
                present.append(int.from_bytes(b[base - 6 - 2 * j:base - 4 - 2 * j], "little"))
        present.sort()
        for k, o in enumerate(present):
            end = present[k + 1] if k + 1 < len(present) else used
            rows.append(bytes(b[HEAP + o:HEAP + end]))
    return rows


TABLES = [(6, "C"), (16, "COL"), (17, "U17"), (18, "U18"), (19, "HIST"), ]
print("// Auto-extracted, track-independent pdb tables from a real rekordbox export")
print("// (test/reference/export.pdb). Byte-identical across exports. Do not edit by hand.")
print("// Regenerate: python3 scripts/extract_static_tables.py > src/pdb_static_tables.inc")
print("namespace {")
for typ, name in TABLES:
    rows = extract(typ)
    flat = b"".join(rows)
    lens = [len(r) for r in rows]
    print(f"const unsigned char ST_{name}_DATA[] = {{{','.join(str(x) for x in flat)}}};")
    print(f"const int ST_{name}_LENS[] = {{{','.join(str(x) for x in lens)}}};")
print("struct StaticTab { uint32_t type; const unsigned char *data; const int *lens; int n; };")
print("const StaticTab STATIC_TABS[] = {")
for typ, name in TABLES:
    print(f"  {{{typ}, ST_{name}_DATA, ST_{name}_LENS, (int)(sizeof(ST_{name}_LENS)/sizeof(int))}},")
print("};")
print("}")
