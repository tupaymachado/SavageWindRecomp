#!/usr/bin/env bash
# savewind save sync: builds the in-title module + BearSSL into ONE relocatable object that
# exports only sw_title_before_mount / sw_title_after_commit (everything else is local, so
# nothing can clash with the title's other libraries). Prints the object's path.
#   runtime/savesync/build-ps5.sh <payload sdk dir>
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
sdk=$1
out="$root/build/ps5/savesync"
bearssl="$out/bearssl"
bearssl_rev=7bea48e5e850ab4cafbe68d3765cdaba13a86d6f   # BearSSL master, 2026-04-06 (MIT)
cc() { PS5_PAYLOAD_SDK="$sdk" sh "$root/ps5/tooling/prospero-clang18" "$@"; }
tool() { local t; for t in "$sdk/bin/$1" "$(command -v "$1-18" || true)" "$(command -v "$1" || true)"; do
    [[ -n $t && -x $t ]] && { printf '%s' "$t"; return; }; done; echo "missing $1" >&2; exit 2; }

mkdir -p "$out/obj/bearssl"
if [[ ! -f $bearssl/.rev || $(<"$bearssl/.rev") != "$bearssl_rev" ]]; then
    [[ -d $bearssl/.git ]] || { git init -q "$bearssl"; git -C "$bearssl" remote add origin https://www.bearssl.org/git/BearSSL; }
    git -C "$bearssl" fetch -q --depth 1 origin "$bearssl_rev"
    git -C "$bearssl" checkout -q --detach --force "$bearssl_rev"
    find "$out/obj/bearssl" -name '*.o' -delete
    printf '%s\n' "$bearssl_rev" > "$bearssl/.rev"
fi

flags=(-std=gnu11 -O2 -g1 -fno-omit-frame-pointer -ffunction-sections -fdata-sections -Wall -Wextra)
objects=()
for src in $(cd "$bearssl" && find src -name '*.c' | LC_ALL=C sort); do
    obj="$out/obj/bearssl/$(tr / _ <<<"$src").o"
    [[ -f $obj ]] || cc "${flags[@]}" -w -I"$bearssl/src" -I"$bearssl/inc" -c "$bearssl/$src" -o "$obj"
    objects+=("$obj")
done
for name in sw_core sw_sync sw_http sw_net sw_tls sw_hooks sw_ps5 sha256 json; do
    obj="$out/obj/$name.o"
    cc "${flags[@]}" -Werror -DSW_TLS -I"$bearssl/inc" -I"$root/runtime" \
        -c "$root/runtime/savesync/$name.c" -o "$obj"
    objects+=("$obj")
done

"$(tool ld.lld)" -r -o "$out/savesync-all.o" "${objects[@]}"
"$(tool llvm-objcopy)" --keep-global-symbol=sw_title_before_mount --keep-global-symbol=sw_title_after_commit \
    "$out/savesync-all.o" "$out/savesync.o"
printf '%s\n' "$out/savesync.o"
