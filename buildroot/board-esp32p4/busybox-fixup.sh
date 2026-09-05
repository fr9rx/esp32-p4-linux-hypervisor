#!/bin/sh
# Apply a list of kconfig assignments to a .config, replacing any existing value
# for each symbol instead of appending.
#
# Appending does not work: this kconfig keeps the FIRST assignment of a symbol
# and ignores later duplicates, so a fixups file cat'd onto the end has no
# effect at all. Buildroot's own KCONFIG_ENABLE_OPT has the same problem and
# solves it the same way -- delete every line mentioning the symbol, then
# append the one we want.
set -e
CONFIG="$1"
FIXUPS="$2"
test -f "$CONFIG"
test -f "$FIXUPS"

applied=0
while IFS= read -r line; do
    case "$line" in
        CONFIG_*=*)
            symbol="${line%%=*}"
            ;;
        "# CONFIG_"*" is not set")
            symbol="${line#\# }"
            symbol="${symbol% is not set}"
            ;;
        *)
            continue
            ;;
    esac
    sed -i "/^# ${symbol} is not set\$/d; /^${symbol}=/d" "$CONFIG"
    printf "%s
" "$line" >> "$CONFIG"
    applied=$((applied + 1))
done < "$FIXUPS"

echo "busybox-fixup: applied $applied assignments to $CONFIG"
