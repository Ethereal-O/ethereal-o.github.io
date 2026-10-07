#!/bin/bash
# Prepare a physical machine for raf measurements, and record what it was.
#
#   sudo ./setup.sh --mount /mnt/raf-bench            check and tune only
#   sudo ./setup.sh --mount /mnt/raf-bench --tune     also change kernel knobs
#   sudo ./setup.sh --mount /mnt/raf-bench --restore  put the knobs back
#
# Writes env.txt next to the results. Send that file along with the numbers --
# without it a result cannot be interpreted, and a drive that turns out to
# have been near-full or thermally throttled explains more than any table.
set -u

MOUNT=""
TUNE=0
RESTORE=0
OUT="${OUT:-$(pwd)/results}"

while [ $# -gt 0 ]; do
  case "$1" in
    --mount)   MOUNT="$2"; shift 2 ;;
    --tune)    TUNE=1; shift ;;
    --restore) RESTORE=1; shift ;;
    --out)     OUT="$2"; shift 2 ;;
    *) echo "usage: $0 --mount DIR [--tune|--restore] [--out DIR]" >&2; exit 2 ;;
  esac
done
[ -n "$MOUNT" ] || { echo "error: --mount is required" >&2; exit 2; }
[ -d "$MOUNT" ] || { echo "error: $MOUNT is not a directory" >&2; exit 2; }

mkdir -p "$OUT"
# This script is normally run under sudo and run.sh is not, so anything
# created here must end up owned by the person who invoked sudo -- otherwise
# run.sh cannot write its results into the same directory.
if [ -n "${SUDO_USER:-}" ]; then
  chown -R "$SUDO_USER" "$OUT" 2>/dev/null || true
fi
SAVED="$OUT/tuning.saved"

# The kernel device behind the mount: /dev/nvme0n1p2 -> nvme0n1, sda3 -> sda.
part=$(df --output=source "$MOUNT" | tail -1)
DEV=$(lsblk -no PKNAME "$part" 2>/dev/null | head -1)
[ -n "$DEV" ] || DEV=$(basename "$part" | sed 's/p\?[0-9]*$//')
Q=/sys/block/$DEV/queue
[ -d "$Q" ] || { echo "error: cannot find /sys/block/$DEV/queue for $part" >&2; exit 2; }

say() { printf '%s\n' "$*"; }
need_root() { [ "$(id -u)" = 0 ] || { echo "error: --tune and --restore need root" >&2; exit 2; }; }

if [ "$RESTORE" = 1 ]; then
  need_root
  [ -f "$SAVED" ] || { echo "error: no $SAVED to restore from" >&2; exit 2; }
  # shellcheck disable=SC1090
  . "$SAVED"
  echo "$OLD_SCHED"     > "$Q/scheduler"      2>/dev/null || true
  echo "$OLD_READAHEAD" > "$Q/read_ahead_kb"  2>/dev/null || true
  echo "$OLD_ROTATIONAL" > "$Q/rotational"    2>/dev/null || true
  for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -w "$g" ] && echo "$OLD_GOVERNOR" > "$g" 2>/dev/null || true
  done
  say "restored scheduler=$OLD_SCHED read_ahead_kb=$OLD_READAHEAD governor=$OLD_GOVERNOR"
  exit 0
fi

# ---------------------------------------------------------------- environment
{
  say "# raf benchmark environment"
  say "date            $(date -Is)"
  say "host            $(hostname)"
  say "kernel          $(uname -sr)"
  say "cpu             $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
  say "cores           $(nproc) online, $(lscpu | awk -F: '/^Socket/{print $2}' | tr -d ' ') socket(s)"
  say "virt            $(systemd-detect-virt 2>/dev/null || echo unknown)"
  say "memory          $(awk '/MemTotal/{printf "%.1f GiB", $2/1048576}' /proc/meminfo)"
  say "mount           $MOUNT  ($part)"
  say "device          /dev/$DEV"
  say "fs              $(findmnt -T "$MOUNT" -no FSTYPE,OPTIONS 2>/dev/null)"
  say "fs free         $(df -h --output=avail "$MOUNT" | tail -1 | tr -d ' ')"
  say ""
  say "# device"
  say "model           $(cat "/sys/block/$DEV/device/model" 2>/dev/null | sed 's/ *$//')"
  say "size            $(lsblk -dno SIZE "/dev/$DEV" 2>/dev/null | tr -d ' ')"
  say "rotational      $(cat "$Q/rotational")"
  say "scheduler       $(cat "$Q/scheduler")"
  say "nr_requests     $(cat "$Q/nr_requests")"
  say "read_ahead_kb   $(cat "$Q/read_ahead_kb")"
  say "max_sectors_kb  $(cat "$Q/max_sectors_kb")"
  say "write_cache     $(cat "$Q/write_cache" 2>/dev/null)"
  say "queues          $(ls -d /sys/block/$DEV/mq/* 2>/dev/null | wc -l) hardware"
  if command -v nvme >/dev/null && [ -e "/dev/$DEV" ]; then
    say "nvme model      $(nvme id-ctrl "/dev/$DEV" 2>/dev/null | awk -F: '/^mn /{print $2}' | sed 's/^ *//')"
    say "nvme firmware   $(nvme id-ctrl "/dev/$DEV" 2>/dev/null | awk -F: '/^fr /{print $2}' | sed 's/^ *//')"
    say "nvme used%      $(nvme smart-log "/dev/$DEV" 2>/dev/null | awk -F: '/percentage_used/{print $2}' | sed 's/^ *//')"
    say "nvme temp       $(nvme smart-log "/dev/$DEV" 2>/dev/null | awk -F: '/^temperature/{print $2}' | sed 's/^ *//')"
  fi
  d=$(readlink -f "/sys/block/$DEV/device" 2>/dev/null)
  if [ -n "$d" ]; then
    say "pcie link       $(cat "$d/../current_link_speed" 2>/dev/null) $(cat "$d/../current_link_width" 2>/dev/null)"
  fi
  say ""
  say "# cpu state"
  say "governor        $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
  say "turbo/boost     $(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null && echo '(0 = turbo on)' || echo n/a)"
  say "smt             $(cat /sys/devices/system/cpu/smt/control 2>/dev/null || echo n/a)"
  say ""
  say "# toolchain"
  say "compiler        $(${CXX:-c++} --version 2>/dev/null | head -1)"
  say "liburing        $(pkg-config --modversion liburing 2>/dev/null || echo 'not found -- io_uring rows will be skipped')"
  say "fio             $(fio --version 2>/dev/null || echo 'not found -- reference rows will be skipped')"
} | tee "$OUT/env.txt"

# -------------------------------------------------------------------- tuning
if [ "$TUNE" = 1 ]; then
  need_root
  cat > "$SAVED" <<SAVE
OLD_SCHED=$(sed 's/.*\[\(.*\)\].*/\1/' "$Q/scheduler")
OLD_READAHEAD=$(cat "$Q/read_ahead_kb")
OLD_ROTATIONAL=$(cat "$Q/rotational")
OLD_GOVERNOR=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)
SAVE
  # none: on NVMe the scheduler only adds latency; the device reorders better.
  grep -q none "$Q/scheduler" && echo none > "$Q/scheduler"
  # Readahead inflates a random-read measurement by fetching what was not
  # asked for. raf reads exactly what a record needs.
  echo 0 > "$Q/read_ahead_kb"
  for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -w "$g" ] && echo performance > "$g" 2>/dev/null
  done
  say ""
  say "tuned: scheduler=$(cat "$Q/scheduler") read_ahead_kb=$(cat "$Q/read_ahead_kb") governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)"
  say "previous values saved to $SAVED; undo with --restore"
fi

# ----------------------------------------------------------------- the checks
say ""
say "# checks"
warn=0
flag() { say "  WARN  $*"; warn=1; }
ok()   { say "  ok    $*"; }

[ "$(cat "$Q/rotational")" = 0 ] && ok "rotational=0" \
  || flag "rotational=1 -- the kernel is applying spinning-disk heuristics"
grep -q '\[none\]' "$Q/scheduler" && ok "scheduler=none" \
  || flag "scheduler=$(sed 's/.*\[\(.*\)\].*/\1/' "$Q/scheduler") -- merging will distort small writes; --tune sets none"
[ "$(cat "$Q/read_ahead_kb")" = 0 ] && ok "read_ahead_kb=0" \
  || flag "read_ahead_kb=$(cat "$Q/read_ahead_kb") -- random reads will fetch more than asked; --tune sets 0"
[ "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)" = performance ] \
  && ok "governor=performance" \
  || flag "governor is not performance -- microsecond latencies will be noisy"
[ "$(systemd-detect-virt 2>/dev/null)" = none ] && ok "bare metal" \
  || flag "virtualised ($(systemd-detect-virt 2>/dev/null)) -- device latency is not the drive's"

mem_gib=$(awk '/MemTotal/{printf "%.0f", $2/1048576}' /proc/meminfo)
free_gib=$(df --output=avail -BG "$MOUNT" | tail -1 | tr -dc 0-9)
want=$((mem_gib * 2 + 16))
[ "$free_gib" -ge "$want" ] && ok "free space ${free_gib}G covers a cold working set" \
  || flag "only ${free_gib}G free; a cold random-read run wants >= ${want}G (2x RAM + headroom)"

used_pct=$(df --output=pcent "$MOUNT" | tail -1 | tr -dc 0-9)
[ "${used_pct:-0}" -le 80 ] && ok "filesystem ${used_pct}% used" \
  || flag "filesystem ${used_pct}% used -- an SSD near full garbage-collects during the run"

say ""
[ "$warn" = 0 ] && say "ready. next: ./run.sh --mount $MOUNT --out $OUT" \
  || say "fix the warnings above, or re-run with --tune, then: ./run.sh --mount $MOUNT --out $OUT"
