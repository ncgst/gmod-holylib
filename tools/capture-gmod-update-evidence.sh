#!/usr/bin/env bash
# Collect file identities only. Never start, stop, update or reconfigure a server.
set -euo pipefail

if [[ $# != 2 ]]; then
    printf 'Usage: %s SERVER_ROOT OUTPUT_DIRECTORY\n' "$0" >&2
    exit 2
fi

root=$(cd -- "$1" && pwd -P)
output=$(realpath -m -- "$2")
if [[ "$output" == "$root" || "$output" == "$root/"* ]]; then
    printf 'Output must be outside the server installation.\n' >&2
    exit 2
fi
if [[ ! -d "$root/garrysmod" ]]; then
    printf 'SERVER_ROOT must contain garrysmod/.\n' >&2
    exit 2
fi
mkdir -p -- "$output"

manifest="$output/native-sha256.txt"
links="$output/native-symlinks.txt"
(
    cd -- "$root"
    for directory in bin garrysmod/bin garrysmod/lua/bin; do
        if [[ -d "$directory" ]]; then
            find "$directory" -type f \( -name '*.so' -o -name '*.so.*' -o -name '*.dll' \) -print0
        fi
    done
    for file in ghostinj.dll srcds_run srcds_linux srcds_linux64; do
        if [[ -f "$file" && ! -L "$file" ]]; then printf '%s\0' "$file"; fi
    done
) | sort -zu | (
    cd -- "$root"
    xargs -0 -r sha256sum --
) > "$manifest"

(
    cd -- "$root"
    for directory in bin garrysmod/bin garrysmod/lua/bin; do
        if [[ -d "$directory" ]]; then
            find "$directory" -type l -printf '%p -> %l\n'
        fi
    done
) > "$links"

{
    printf 'captured_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'scope=files on disk; loaded process identity and runtime gates are separate\n'
    printf 'symlinks=listed separately; resolve and verify their loaded targets before promotion\n'
    if [[ -f "$root/garrysmod/garrysmod.ver" ]]; then
        printf '\n[garrysmod.ver]\n'
        cat -- "$root/garrysmod/garrysmod.ver"
        printf '\n'
    elif [[ -f "$root/garrysmod.ver" ]]; then
        printf '\n[garrysmod.ver]\n'
        cat -- "$root/garrysmod.ver"
        printf '\n'
    fi
    for app in "$root/steamapps/appmanifest_4020.acf" "$root/steamapps/appmanifest_4000.acf"; do
        if [[ -f "$app" ]]; then
            printf '\n[%s: buildid only]\n' "${app##*/}"
            awk '$1 == "\"buildid\"" { print $0 }' "$app"
        fi
    done
} > "$output/engine-identity.txt"

printf 'Evidence saved in %s\n' "$output"
