#!/usr/bin/env bash
# Package a release: dist/MagickaPoolFix-<version>.zip, containing the plugin, its
# default .conf, the docs and the full source, plus a SHA256SUMS covering every file.
#
# Run ./build.sh first. Any previous zip in dist/ is removed, so dist/ only ever
# holds the current release.
set -euo pipefail
cd "$(dirname "$0")/.."

ver=$(sed -n 's/^#define MPF_PLUGIN_VERSION "\(.*\)"/\1/p' src/PCH.h)
[ -n "$ver" ] || { echo "cannot read MPF_PLUGIN_VERSION from src/PCH.h"; exit 1; }
for f in build/MagickaPoolFix.asi build/MagickaPoolFix.conf; do
    [ -f "$f" ] || { echo "missing $f: run ./build.sh first"; exit 1; }
done

name="MagickaPoolFix-$ver"
stage="dist/$name"
rm -rf dist
mkdir -p "$stage"

cp build/MagickaPoolFix.asi build/MagickaPoolFix.conf "$stage/"
cp README.md SPEC.md FIELD-JOURNAL.md LICENSE upstream-issue-1846.md \
   CMakeLists.txt build.sh "$stage/"
cp -r src tests cmake tools "$stage/"

(cd "$stage" && find . -type f ! -name SHA256SUMS | LC_ALL=C sort | xargs sha256sum > SHA256SUMS)
(cd dist && zip -qr "$name.zip" "$name")
rm -rf "$stage"

echo "dist/$name.zip"
unzip -l "dist/$name.zip" | tail -1
