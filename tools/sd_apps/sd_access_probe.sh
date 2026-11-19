#!/system/bin/sh
# One-shot bench probe. Reads the SD root; writes only to the caller's log.
# Run in a detached session with stdout/stderr redirected to internal storage.
set -u
samples=${1:-60}
case "$samples" in
    ''|*[!0-9]*) echo 'samples must be an integer from 1 to 60' >&2; exit 2 ;;
esac
if [ "$samples" -lt 1 ] || [ "$samples" -gt 60 ]; then
    echo 'samples must be an integer from 1 to 60' >&2
    exit 2
fi

card=/mnt/media_rw/sdcard0
echo "PROBE_START pid=$$ samples=$samples card=$card"
id
getprop ro.build.version.incremental
trap 'echo PROBE_INTERRUPTED; exit 143' TERM

i=1
while [ "$i" -le "$samples" ]; do
    echo "SAMPLE=$i UTC=$(date -u +%Y-%m-%dT%H:%M:%SZ) USB_CONFIG=$(getprop sys.usb.config)"
    # The subshell releases its cwd after each attempt. No card handles stay open.
    # Discard names: only access success/failure and error text belong in the log.
    if (cd "$card" && /system/bin/ls >/dev/null); then
        echo CARD_ACCESS=ok
    else
        echo CARD_ACCESS=unavailable
    fi
    [ "$i" -eq "$samples" ] || sleep 2
    i=$((i + 1))
done
echo PROBE_FINISHED
