#!/usr/bin/env bash
# Build one firmware image per flash size, into dist/<version>/<size>/.
#
# WHY BY FLASH SIZE AND NOT BY MODULE NAME
#
# Boards are sold as N8R2, N16R8 and so on, where N is flash and R is PSRAM.
# Only the flash number matters to us: CONFIG_SPIRAM is not set and nothing in
# this firmware allocates from PSRAM, so a module's R-number changes nothing
# about the image it needs. An N8R2 and an N8R8 take the identical binary.
#
# Naming releases after module SKUs would therefore invent a choice users do
# not have to make, and get it wrong the moment somebody turns up with an N8R0
# or an N16R2. The release notes list which modules each size covers.
#
#   bash tools/build-variants.sh
#
# Restores sdkconfig afterwards, so the working tree is left as it was found.
set -euo pipefail
cd "$(dirname "$0")/.."

if ! command -v idf.py >/dev/null 2>&1; then
    cat <<'MSG'
idf.py not found.

Run this from a shell that has already exported the ESP-IDF environment. On
Windows that means PowerShell, NOT the bash that ships with WSL - they are
different environments and the WSL one does not inherit the export:

    & "C:\esp\v6.0.1\esp-idf\export.ps1"

then run the equivalent loop from PowerShell, or use an IDF-aware bash.
MSG
    exit 1
fi

VER=$(grep -oE '"[0-9]+\.[0-9]+\.[0-9]+"' main/hr_http.h | tr -d '"' | head -1)
[ -z "$VER" ] && { echo "could not read version from main/hr_http.h"; exit 1; }
echo "building Free Harvest $VER"

# size:partition csv
VARIANTS="4mb:partitions-4mb.csv 8mb:partitions.csv 16mb:partitions-16mb.csv"

BACKUP=$(mktemp)
cp sdkconfig "$BACKUP"
restore() { cp "$BACKUP" sdkconfig; rm -f "$BACKUP"; }
trap restore EXIT

for v in $VARIANTS; do
    size="${v%%:*}"
    csv="${v##*:}"
    out="dist/$VER/$size"
    echo
    echo "=== $size ($csv) ==="

    cp "$BACKUP" sdkconfig
    sed -i "s/^CONFIG_ESPTOOLPY_FLASHSIZE=\".*\"/CONFIG_ESPTOOLPY_FLASHSIZE=\"${size^^}\"/" sdkconfig
    sed -i "s/^CONFIG_ESPTOOLPY_FLASHSIZE_[0-9]*MB=y/CONFIG_ESPTOOLPY_FLASHSIZE_${size^^}=y/" sdkconfig
    sed -i "s|^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\".*\"|CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"$csv\"|" sdkconfig

    idf.py build > "/tmp/build-$size.log" 2>&1 || {
        echo "  BUILD FAILED - see /tmp/build-$size.log"
        tail -20 "/tmp/build-$size.log"
        exit 1
    }
    grep -E "Smallest app partition" "/tmp/build-$size.log" | sed 's/^/  /'

    mkdir -p "$out"
    cp build/hr_wifi_adapter.bin \
       build/bootloader/bootloader.bin \
       build/partition_table/partition-table.bin \
       build/ota_data_initial.bin "$out/"
    echo "  -> $out"
done

echo
echo "done. sdkconfig restored; rebuild once more for your own board."
