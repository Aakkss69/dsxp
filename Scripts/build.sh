#!/bin/sh
# Release build of DSXP.app, signature check, zip + sha256 in dist/.
#   Scripts/build.sh            build and package
#   Scripts/build.sh --install  also replace /Applications/DSXP.app
set -eu
cd "$(dirname "$0")/.."

install=0
[ "${1:-}" = "--install" ] && install=1

command -v xcodegen >/dev/null 2>&1 || { echo "XcodeGen missing: brew install xcodegen"; exit 1; }
xcodegen generate --quiet

build_number=$(git rev-list --count HEAD 2>/dev/null || echo 1)
mkdir -p build
echo "Building DSXP (Release, build $build_number)..."
if ! xcodebuild -project DSXP.xcodeproj -scheme DSXP -configuration Release \
        -derivedDataPath build/DD CURRENT_PROJECT_VERSION="$build_number" build \
        > build/xcodebuild.log 2>&1; then
    grep -E "error:" build/xcodebuild.log | sort -u || tail -40 build/xcodebuild.log
    echo "Build failed - full log: build/xcodebuild.log"
    exit 1
fi

app=build/DD/Build/Products/Release/DSXP.app
codesign --verify --strict --verbose=2 "$app"
if codesign -dv "$app" 2>&1 | grep -q "Signature=adhoc"; then
    echo "WARNING: ad-hoc signature. macOS will ask for Input Monitoring again after"
    echo "         every rebuild. Run Scripts/bootstrap.sh once you have an Apple"
    echo "         Development certificate."
else
    codesign -dv "$app" 2>&1 | grep -E "^Authority=" | head -1
fi
echo "Designated requirement:"
codesign -d -r- "$app" 2>&1 | sed -n 's/^#* *designated => /  /p'

version=$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" "$app/Contents/Info.plist")
mkdir -p dist
zip="DSXP-$version.zip"
rm -f "dist/$zip"
ditto -c -k --keepParent "$app" "dist/$zip"
(cd dist && shasum -a 256 "$zip" > "$zip.sha256")
echo "Packaged dist/$zip"

if [ "$install" = 1 ]; then
    if pgrep -x DSXP >/dev/null 2>&1; then
        echo "Quitting the running DSXP (it hands X-Plane its controls back)..."
        osascript -e 'quit app "DSXP"' >/dev/null 2>&1 || true
        i=0
        while pgrep -x DSXP >/dev/null 2>&1 && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    fi
    rm -rf /Applications/DSXP.app
    ditto "$app" /Applications/DSXP.app
    echo "Installed /Applications/DSXP.app"
fi
