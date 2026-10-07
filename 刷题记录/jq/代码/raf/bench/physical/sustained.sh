#!/bin/bash
# A consumer SSD absorbs the first hundred megabytes of durable appends into
# cache and then falls off a cliff. This finds out what the cliff is.
#
#   ./sustained.sh --mount /mnt/raf-bench [--ops 32000] [--idle 60]
#
# Four curves, each over the same number of durable writes:
#
#   A overwrite, blocks already written   no allocation, no metadata commit.
#                                         Whatever happens here is the drive.
#   B extend, batch 1                     raf's shape. A over plus ext4
#                                         committing a size change per append.
#   C extend, batch 32                    one flush per 32 records. The
#                                         practical question: does batching
#                                         survive the cliff, or just delay it?
#   D after idle                          a short burst once the drive has had
#                                         time to catch up. Recovery means
#                                         garbage collection or a write cache;
#                                         no recovery means steady state.
#   E raf, preallocate off                every append extends the file.
#   F raf, preallocate --prealloc         zeros kept ahead of the append point,
#                                         so appends overwrite. E against F is
#                                         the whole question: on this drive an
#                                         overwrite is row A and an extend is
#                                         row B, so F should hold where E falls.
#
# The runway is topped up once it is half gone, so the default 64 MiB refills
# every 32 MiB. Setting --prealloc larger than the whole run preallocates once
# and never refills, which is how to tell a refill cost from something that
# simply degrades with bytes written.
#
# Drive temperature is sampled around each curve: thermal throttling looks
# exactly like cache exhaustion from the outside. Needs no privileges --
# nothing in this script does.
set -u
MOUNT=""; OPS=32000; IDLE=60; CURVES=ABCD; PREALLOC=$(( 64 * 1024 * 1024 ))
while [ $# -gt 0 ]; do
  case "$1" in
    --mount)  MOUNT="$2"; shift 2 ;;
    --ops)    OPS="$2"; shift 2 ;;
    --idle)   IDLE="$2"; shift 2 ;;
    --curves) CURVES="$2"; shift 2 ;;
    --prealloc) PREALLOC="$2"; shift 2 ;;
    *) echo "usage: $0 --mount DIR [--ops N] [--idle SECONDS] [--curves ABCDEF]" \
            "[--prealloc BYTES]" >&2; exit 2 ;;
  esac
done
# --curves B re-measures just the one that collapses, for comparing a mount
# option or a journal size without paying for the other three.
has() { case "$CURVES" in *$1*) return 0 ;; *) return 1 ;; esac; }
[ -n "$MOUNT" ] && [ -d "$MOUNT" ] || { echo "usage: $0 --mount DIR" >&2; exit 2; }

part=$(df --output=source "$MOUNT" | tail -1)
DEV=$(lsblk -no PKNAME "$part" 2>/dev/null | head -1)
[ -n "$DEV" ] || DEV=$(basename "$part" | sed 's/p\?[0-9]*$//')
D="$MOUNT/.sustained.$$"; mkdir -p "$D"
trap 'rm -rf "$D"' EXIT
CHUNKS=8
PER=$(( OPS / CHUNKS ))

# Controller temperature straight out of sysfs, in millidegrees. nvme-cli
# would need root for the same number, and nothing else here does.
temp() {
    local f
    for f in /sys/block/"$DEV"/device/hwmon*/temp1_input; do
        if [ -r "$f" ]; then
            awk '{printf "%dC", $1 / 1000}' "$f"
            return
        fi
    done
    printf 'n/a'
}

ddcurve() { # label bs setup extra
  local lbl=$1 bs=$2 setup=$3 extra=${4:-}
  echo; echo "## $lbl   ($PER writes per chunk, temp $(temp))"
  rm -f "$D/f"; eval "$setup"; sync
  local off=0 i r
  for i in $(seq $CHUNKS); do
    # shellcheck disable=SC2086
    r=$( { TIMEFORMAT=%R; time dd if=/dev/zero of="$D/f" bs=$bs count=$PER \
           seek=$off oflag=dsync status=none $extra; } 2>&1 )
    awk -v a=$((off)) -v b=$((off+PER)) -v r="$r" -v n="$PER" -v s="$bs" \
      'BEGIN{printf "  %7d..%7d  %7.1f MiB/s  %8.0f us/op\n", a, b, n*s/1048576/r, r*1e6/n}'
    off=$(( off + PER ))
  done
  echo "  (temp now $(temp))"
}

echo "# device /dev/$DEV   $OPS durable writes per curve"
PRE=$(( OPS * 4128 + 1048576 ))

has A && ddcurve "A overwrite 4096, blocks already written -- the drive alone" 4096 \
        "dd if=/dev/zero of=$D/f bs=1M count=$(( PRE / 1048576 )) status=none" "conv=notrunc"
has B && ddcurve "B extend 4128 -- raf's shape, drive + ext4 metadata" 4128 "true"

# The mount options that matter for this cliff, printed next to the curve so
# a result is never read without them.
echo
echo "# filesystem"
findmnt -T "$MOUNT" -no FSTYPE,OPTIONS | sed 's/^/  /'


REPO=$(cd "$(dirname "$0")/../.." && pwd); BUILD="$REPO/build-bench"
if { has C || has D || has E || has F; } &&
   cmake -S "$REPO" -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRAF_BUILD_TESTS=OFF >/dev/null 2>&1 &&
   cmake --build "$BUILD" -j"$(nproc)" --target raf_matrix >/dev/null 2>&1; then
  rm -rf "$D/a"; mkdir -p "$D/a"
  has C && echo && echo "## C raf, batch 32 -- one flush per 32 records   (temp $(temp))"
  has C &&
  for i in $(seq $CHUNKS); do
    r=$("$BUILD/raf_matrix" write --dir "$D/a" --record 4096 --threads 1 --batch 32 --count "$PER")
    awk -v a=$(( (i-1)*PER )) -v b=$(( i*PER )) -v m="$(cut -d' ' -f1 <<<"$r")" \
        -v p="$(cut -d' ' -f3 <<<"$r")" \
      'BEGIN{printf "  %7d..%7d  %7.1f MiB/s  %8.0f us/batch\n", a, b, m, p}'
  done
  echo "  (temp now $(temp))"

  # E and F are the preallocation A/B: identical workloads, one letting every
  # append extend the file, the other keeping zeros ahead of the append point
  # so appends overwrite instead. Both sustained, because a short burst cannot
  # see the cliff that makes the question interesting.
  rafcurve() { # label prealloc-bytes
    local lbl=$1 pa=$2
    echo; echo "## $lbl   (temp $(temp))"
    rm -rf "$D/a"; mkdir -p "$D/a"
    # One process for the whole curve: a separate process per chunk would
    # reopen the segment and preallocate before the clock started, which hides
    # the cost this comparison exists to measure.
    "$BUILD/raf_matrix" write --dir "$D/a" --record 4096 --threads 1 --batch 1 \
        --count "$PER" --chunks "$CHUNKS" --prealloc "$pa" 2>/dev/null |
    awk -v per="$PER" '{printf "  %7d..%7d  %7.1f MiB/s  p50 %7.0f us  p99 %8.0f us\n",
                               (NR-1)*per, NR*per, $1, $3, $4}'
    echo "  (temp now $(temp))"
    rm -rf "$D/a"
  }
  has E && rafcurve "E raf batch 1, preallocateBytes=0 -- appends extend" 0
  has F && rafcurve "F raf batch 1, preallocateBytes=$PREALLOC -- appends overwrite" "$PREALLOC"

  has D || { rm -rf "$D/a"; exit 0; }
  echo; echo "## D recovery: idle ${IDLE}s, then a short burst"
  sync; sleep "$IDLE"
  r=$("$BUILD/raf_matrix" write --dir "$D/a" --record 4096 --threads 1 --batch 1 --count 2000)
  awk -v m="$(cut -d' ' -f1 <<<"$r")" -v p="$(cut -d' ' -f3 <<<"$r")" \
    'BEGIN{printf "  batch 1, 2000 records: %7.1f MiB/s  %8.0f us/op\n", m, p}'
  echo "  (temp now $(temp))"
  rm -rf "$D/a"
fi

echo
echo "# reading it"
echo "#   A falls too        the drive. Nothing above it can help."
echo "#   only B falls       ext4's journal, not the drive. Try a bigger journal,"
echo "#                      data=writeback, or xfs."
echo "#   C holds up         batching survives the cliff: the fix is at the call"
echo "#                      site, and raf's appendBatch already does it."
echo "#   C falls too        the cliff is bytes written, not flushes issued."
echo "#   D recovers         garbage collection or a write cache catching up."
echo "#   D does not         the drive has reached its steady state; that is the"
echo "#                      number to design against."
echo "#   F flat, E falls    preallocation works here: turn preallocateBytes on."
echo "#   F no better        this drive does not prefer overwrites; leave it off."
echo "#   F worse than E     it costs double the bytes written for nothing."
