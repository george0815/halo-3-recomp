#!/usr/bin/env bash
# Runs H3EK's tool.exe under Wine just long enough for its static
# initializers to build the tag definitions, then dumps its .rdata and .data
# (with .bss) for tagdefs.py. Many struct definitions are only complete
# after initialization (fields and sizes are filled in by constructors), so
# the file alone isn't enough.
#
# Usage: tools/tagdefs/dump_tool_memory.sh <H3EK folder> <work dir>
#
# Copies tool.exe, libfbxsdk.dll and bin/ into <work dir> (the H3EK install
# is never run or written to in place), uses its own Wine prefix there, and
# writes <work dir>/tool_rdata.bin and tool_data.bin (~420 MB). Needs wine
# and gdb. tool.exe prints its usage and exits; gdb stops it at exit_group.
set -euo pipefail

[[ $# -eq 2 ]] || { sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
H3EK="$1"
WORK="$(mkdir -p "$2" && realpath "$2")"

cp "$H3EK/tool.exe" "$H3EK/libfbxsdk.dll" "$WORK/"
cp -r "$H3EK/bin" "$WORK/"

# Section ranges from the PE header (tool.exe is not relocated by Wine: its
# preferred base is free).
read -r RDATA_START RDATA_END DATA_START DATA_END < <(python3 -I - "$WORK/tool.exe" <<'EOF'
import struct, sys
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]
count = struct.unpack_from('<H', d, pe + 6)[0]
optional = struct.unpack_from('<H', d, pe + 20)[0]
base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]
ranges = {}
for i in range(count):
    o = pe + 24 + optional + i * 40
    name = d[o:o + 8].rstrip(b'\0').decode()
    size, address = struct.unpack_from('<2I', d, o + 8)
    ranges[name] = (base + address, base + address + size)
print(*(f'{x:#x}' for x in ranges['.rdata'] + ranges['.data']))
EOF
)

export WINEPREFIX="$WORK/wineprefix" WINEDLLOVERRIDES="mscoree,mshtml=" WINEDEBUG=-all
[[ -d "$WINEPREFIX" ]] || wineboot -i > "$WORK/wineboot.log" 2>&1

cd "$WORK"
# Wine uses these signals itself; let them through.
signals=(SIGSEGV SIGUSR1 SIGUSR2 SIGILL SIGBUS SIGPIPE)
gdb_args=(-batch -ex "set pagination off")
for s in "${signals[@]}"; do gdb_args+=(-ex "handle $s nostop noprint pass"); done
gdb_args+=(-ex "catch syscall exit_group" -ex run
    -ex "dump binary memory tool_rdata.bin $RDATA_START $RDATA_END"
    -ex "dump binary memory tool_data.bin $DATA_START $DATA_END" -ex kill)
timeout 300 gdb "${gdb_args[@]}" --args wine tool.exe > gdb.log 2>&1 || true
ls -la "$WORK/tool_rdata.bin" "$WORK/tool_data.bin"
