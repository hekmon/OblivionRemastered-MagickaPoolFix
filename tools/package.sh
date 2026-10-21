#!/usr/bin/env bash
# Package a release: dist/MagickaPoolFix-<version>.zip, containing the plugin, its
# default .conf, the docs and the full source, plus a SHA256SUMS covering every file.
#
# Run ./build.sh first, from a git checkout. Any previous zip in dist/ is removed,
# so dist/ only ever holds the current release.
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
# Source and docs: every file git tracks, as it is in the working tree, minus the
# repository plumbing (.github/, .gitignore, .gitattributes). Untracked files, such
# as drafts kept out of the repository, never reach a release.
git ls-files -z -- . ':!:.*' | xargs -0 cp --parents -t "$stage/"

(cd "$stage" && find . -type f ! -name SHA256SUMS | LC_ALL=C sort | xargs sha256sum > SHA256SUMS)
(cd dist && zip -qr "$name.zip" "$name")
rm -rf "$stage"

echo "dist/$name.zip"
unzip -l "dist/$name.zip" | tail -1
