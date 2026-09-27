#!/bin/sh
set -e
cd "$(dirname "$0")"

MODTOOL=${MODTOOL:-./RecompModTool}
make
"$MODTOOL" mod.toml build

version=$(grep -m1 '^version = ' mod.toml | cut -d'"' -f2)
manifest_version=$(grep '"version_number"' thunderstore/manifest.json | cut -d'"' -f4)
if [ "$version" != "$manifest_version" ]; then
    echo "version mismatch: mod.toml is $version, thunderstore/manifest.json is $manifest_version" >&2
    exit 1
fi

mkdir -p dist
rm -f "dist/3D_Grottos-$version.zip"
zip -qj "dist/3D_Grottos-$version.zip" build/mm_recomp_3d_grottos.nrm thunderstore/manifest.json \
    thunderstore/icon.png README.md CHANGELOG.md
echo "dist/3D_Grottos-$version.zip"
