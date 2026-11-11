#!/usr/bin/env bash
# Installs keelc, keelb and the kl package on Linux, replacing any earlier install there:
#
#     <prefix>/bin/keelc, <prefix>/bin/keelb
#     <prefix>/lib/keel/<version>/kl/
#
# and puts <prefix>/bin on PATH through ~/.profile when it is not there already. Run from anywhere:
#
#     scripts/install.sh [--build debug] [--link] [--prefix <dir>]
#
# --build picks build/release (the default) or build/debug. --link links the installed files to the
# build and to keel_stl/src instead of copying them, so a rebuild needs no reinstall. With no keelc
# built there, kl is still installed and keelc left as it was.

set -euo pipefail

build=release
link=0
prefix="$HOME/.local"
while (( $# ))
do
    case "$1" in
        --build)  build="$2"; shift 2 ;;
        --link)   link=1; shift ;;
        --prefix) prefix="$2"; shift 2 ;;
        *) echo "usage: $0 [--build release|debug] [--link] [--prefix <dir>]" >&2; exit 2 ;;
    esac
done
[[ $build == release || $build == debug ]] || { echo "--build is release or debug" >&2; exit 2; }

root="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"

# keelc looks for kl under its version, which comes from the top-level project().
version="$( sed -nE 's/^project\(\s*keel\s+VERSION\s+([0-9.]+).*/\1/p' "$root/CMakeLists.txt" )"
[[ -n $version ]] || { echo "could not read the version from CMakeLists.txt" >&2; exit 1; }

mkdir -p "$prefix"
prefix="$( cd "$prefix" && pwd )"
bin="$prefix/bin"
kl="$prefix/lib/keel/$version/kl"
mkdir -p "$bin" "$( dirname "$kl" )"

# Removed first, so a copy never writes through an earlier install's link into the build.
put()
{
    rm -rf "$2"
    if (( link )); then ln -s "$1" "$2"; else cp -R "$1" "$2"; fi
}

built="$root/build/$build/bin"
if [[ -x $built/keelc ]]
then
    for tool in keelc keelb
    do
        if [[ -x $built/$tool ]]
        then
            put "$built/$tool" "$bin/$tool"
            echo "installed $built/$tool"
        fi
    done
else
    echo "warning: no keelc at $built/keelc; build the $build preset first. Installing kl only." >&2
fi

put "$root/keel_stl/src" "$kl"
echo "installed kl $version to $kl"

if [[ ":$PATH:" == *":$bin:"* ]]
then
    echo "$bin is already on PATH"
elif grep -qsF "$bin" "$HOME/.profile" || grep -qsF "${bin/#$HOME/\$HOME}" "$HOME/.profile"
then
    echo "$bin is on PATH in ~/.profile; log in again to pick it up"
else
    printf '\n# keel\nexport PATH="%s:$PATH"\n' "${bin/#$HOME/\$HOME}" >> "$HOME/.profile"
    echo "added $bin to PATH in ~/.profile; log in again, or run: export PATH=\"$bin:\$PATH\""
fi
