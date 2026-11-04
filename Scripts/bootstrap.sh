#!/bin/sh
# One-time setup after cloning: XcodeGen, signing settings, the Xcode project.
set -eu
cd "$(dirname "$0")/.."

if ! command -v xcodegen >/dev/null 2>&1; then
    echo "XcodeGen is needed to generate DSXP.xcodeproj: brew install xcodegen"
    exit 1
fi

# Older checkouts kept local signing settings in Config/ (renamed BuildSettings/).
if [ -f Config/Local.xcconfig ] && [ ! -f BuildSettings/Local.xcconfig ]; then
    mv Config/Local.xcconfig BuildSettings/Local.xcconfig
    rmdir Config 2>/dev/null || true
    echo "Moved Config/Local.xcconfig to BuildSettings/Local.xcconfig."
fi

# Signing: use your Apple Development certificate if you have one, so macOS
# keeps the Input Monitoring permission across rebuilds.
if [ ! -f BuildSettings/Local.xcconfig ]; then
    team=$(security find-certificate -c "Apple Development" -p 2>/dev/null \
           | openssl x509 -noout -subject 2>/dev/null \
           | sed -n 's/.*OU *= *\([A-Z0-9]\{10\}\).*/\1/p' | head -1)
    if [ -n "$team" ]; then
        printf 'CODE_SIGN_IDENTITY = Apple Development\nDEVELOPMENT_TEAM = %s\n' "$team" > BuildSettings/Local.xcconfig
        echo "BuildSettings/Local.xcconfig: signing with your Apple Development certificate (team $team)."
    else
        echo "No Apple Development certificate found - builds will be ad-hoc signed."
        echo "Create one in Xcode > Settings > Accounts > Manage Certificates, then run this again."
    fi
fi

xcodegen generate
echo "Done. Open DSXP.xcodeproj, or run Scripts/build.sh --install."
