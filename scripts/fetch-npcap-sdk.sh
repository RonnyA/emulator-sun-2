#!/bin/sh
# fetch-npcap-sdk.sh — download the Npcap SDK into external/npcap-sdk/
#
# The Npcap *runtime* (wpcap.dll, deployed to C:\Windows\System32\Npcap by
# the Npcap installer) is enough for an already-built program to run, but
# it doesn't ship pcap.h or the import library.  Building net_pcap.c
# against libpcap on Windows therefore needs the SDK.
#
# Run from the repo root:
#   make fetch-npcap-sdk                          # default version
#   NPCAP_SDK_VERSION=1.13 make fetch-npcap-sdk   # specific version

set -eu

VERSION="${NPCAP_SDK_VERSION:-1.13}"
URL="https://npcap.com/dist/npcap-sdk-${VERSION}.zip"

if [ ! -d sim ] || [ ! -d m68k ]; then
    echo "Error: run this via 'make fetch-npcap-sdk' from the repository root." >&2
    exit 1
fi

EXTERNAL="external"
TARGET="$EXTERNAL/npcap-sdk"
ZIP="$EXTERNAL/npcap-sdk-${VERSION}.zip"

mkdir -p "$EXTERNAL"

if [ -d "$TARGET" ]; then
    echo "external/npcap-sdk already exists."
    echo "Delete it first if you want to re-fetch:  rm -rf external/npcap-sdk"
    exit 0
fi

echo "Downloading Npcap SDK ${VERSION} from:"
echo "  $URL"

if command -v curl >/dev/null 2>&1; then
    curl -L --fail -o "$ZIP" "$URL"
elif command -v wget >/dev/null 2>&1; then
    wget -O "$ZIP" "$URL"
else
    echo "Error: neither curl nor wget is available." >&2
    exit 1
fi

echo "Extracting..."
mkdir -p "$TARGET"
(cd "$TARGET" && unzip -q "../npcap-sdk-${VERSION}.zip")
rm "$ZIP"

echo
echo "Npcap SDK ${VERSION} ready at: $TARGET"
ls "$TARGET" 2>/dev/null | head -20 || true

cat <<'EOF'

You can now build with libpcap support:
  make NET_BACKEND=pcap

(The Makefile auto-detects external/npcap-sdk/ — no need to set
 NPCAP_SDK explicitly.)
EOF
