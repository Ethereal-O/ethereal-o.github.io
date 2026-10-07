#!/bin/bash
# Find out why a drive is slower than it should be, before trusting any
# benchmark run on it. Takes well under a minute and writes ~100 MiB.
#
#   ./diagnose.sh --mount /mnt/raf-bench
#
# The question it answers: of the microseconds in one durable 4 KiB append,
# how many belong to the flash, how many to the flush, how many to the
# filesystem, and how many to something else entirely.
set -u

MOUNT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --mount) MOUNT="$2"; shift 2 ;;
    *) echo "usage: $0 --mount DIR" >&2; exit 2 ;;
  esac
done
[ -n "$MOUNT" ] && [ -d "$MOUNT" ] || { echo "usage: $0 --mount DIR" >&2; exit 2; }

part=$(df --output=source "$MOUNT" | tail -1)
DEV=$(lsblk -no PKNAME "$part" 2>/dev/null | head -1)
[ -n "$DEV" ] || DEV=$(basename "$part" | sed 's/p\?[0-9]*$//')
F="$MOUNT/.diag.$$"
trap 'rm -f "$F"' EXIT

echo "# layers"
echo "mount       $MOUNT"
echo "source      $part   -> /dev/$DEV"
findmnt -T "$MOUNT" -no FSTYPE,OPTIONS | sed 's/^/fs          /'
# A stack of device-mapper targets -- LUKS, LVM, RAID, bcache -- each adds
# latency and none of them show up in the device name.
echo "stack       $(lsblk -s -no NAME,TYPE "$part" 2>/dev/null | tr '\n' ' ')"
echo "virt        $(systemd-detect-virt 2>/dev/null || echo unknown)"
echo

echo "# device"
echo "model       $(cat /sys/block/$DEV/device/model 2>/dev/null | sed 's/ *$//')"
echo "rotational  $(cat /sys/block/$DEV/queue/rotational 2>/dev/null)"
echo "scheduler   $(cat /sys/block/$DEV/queue/scheduler 2>/dev/null)"
echo "write_cache $(cat /sys/block/$DEV/queue/write_cache 2>/dev/null)"
echo "hw queues   $(ls -d /sys/block/$DEV/mq/* 2>/dev/null | wc -l)"
# Temperature needs no privileges: sysfs publishes it in millidegrees.
for f in /sys/block/"$DEV"/device/hwmon*/temp1_input; do
  [ -r "$f" ] && { awk '{printf "temperature %dC\n", $1 / 1000}' "$f"; break; }
done
# The rest of the NVMe detail does want root, but it is detail -- never
# prompt for it. sudo -n fails immediately when there is no passwordless
# rule, and running the whole script under sudo fills these in.
if command -v nvme >/dev/null 2>&1; then
  idctrl=$(nvme id-ctrl "/dev/$DEV" 2>/dev/null || sudo -n nvme id-ctrl "/dev/$DEV" 2>/dev/null)
  smart=$(nvme smart-log "/dev/$DEV" 2>/dev/null || sudo -n nvme smart-log "/dev/$DEV" 2>/dev/null)
  if [ -n "$idctrl$smart" ]; then
    echo "nvme model  $(awk -F: '/^mn /{print $2}' <<<"$idctrl" | sed 's/^ *//')"
    echo "nvme vwc    $(awk -F: '/^vwc /{print $2}' <<<"$idctrl" | sed 's/^ *//') (volatile write cache)"
    echo "nvme used   $(awk -F: '/percentage_used/{print $2}' <<<"$smart" | sed 's/^ *//')"
    echo "nvme crit   $(awk -F: '/critical_warning/{print $2}' <<<"$smart" | sed 's/^ *//')"
  else
    echo "nvme detail (run under sudo for model, write cache, wear)"
  fi
fi
# Autonomous power state transitions: the drive parks itself between I/Os and
# pays milliseconds to wake. A classic cause of a fast drive measuring slow.
echo "apst limit  $(cat /sys/module/nvme_core/parameters/default_ps_max_latency_us 2>/dev/null || echo n/a) us"
echo "pcie link   $(cat /sys/block/$DEV/device/../current_link_speed 2>/dev/null) $(cat /sys/block/$DEV/device/../current_link_width 2>/dev/null)"
echo "governor    $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
echo "fs used     $(df -h --output=pcent "$MOUNT" | tail -1 | tr -d ' ')"
echo

# Pre-create, so no run pays for extending the file or journalling a new size.
dd if=/dev/zero of="$F" bs=1M count=64 status=none; sync

t() { # label dd-flags count
  local lbl=$1 flags=$2 n=$3 r
  r=$( { TIMEFORMAT=%R; time dd if=/dev/zero of="$F" bs=4096 count=$n \
         conv=notrunc oflag=$flags status=none; } 2>&1 )
  awk -v l="$lbl" -v r="$r" -v n="$n" \
    'BEGIN{printf "%-34s %8.0f us/op  %7.1f MiB/s\n", l, r*1e6/n, n*4096/1048576/r}'
}

echo "# where the time goes, 4 KiB writes"
t "O_DIRECT  (device, no flush)"   direct       2000
t "O_DSYNC   (device + flush)"     direct,dsync 500
t "buffered + fdatasync each"      dsync        500
echo
echo "# interpreting:"
echo "#   O_DIRECT slow too        -> the drive, the link, or a dm layer"
echo "#   only O_DSYNC slow        -> flush cost: write cache off, or no power-loss"
echo "#                               protection, or APST waking the drive"
echo "#   only buffered slow       -> the filesystem's journal, not the drive"
echo "#   all three ~10-50 us/op   -> the drive is fine; raf is the problem"
