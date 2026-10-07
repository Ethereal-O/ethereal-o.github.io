#!/bin/bash
# Decompose one durable append. raf measured 12x the cost of dd doing
# nominally the same thing on a healthy NVMe; this says where that goes.
#
#   ./appendcost.sh --mount /mnt/raf-bench
#
# Three things differ between `dd oflag=dsync` over an existing file and what
# raf actually does, and each is isolated below:
#
#   - raf extends the file on every append, so ext4 commits a metadata
#     transaction alongside the data. On the VM this project was developed
#     on, that doubles the cost of an append: 422 us overwriting blocks that
#     already exist against 808 us extending. Preallocating does not avoid
#     it -- row 5 measures that, and on ext4 it came out no better.
#   - a raf record is 4128 bytes, not 4096: a 32-byte header plus a 4 KiB
#     payload. It therefore straddles two pages, always.
#   - raf writes through its own code.
set -u
MOUNT=""
SUSTAINED=0
while [ $# -gt 0 ]; do
  case "$1" in
    --mount) MOUNT="$2"; shift 2 ;;
    --sustained) SUSTAINED=1; shift ;;
    *) echo "usage: $0 --mount DIR [--sustained]" >&2; exit 2 ;;
  esac
done
[ -n "$MOUNT" ] && [ -d "$MOUNT" ] || { echo "usage: $0 --mount DIR" >&2; exit 2; }

D="$MOUNT/.append.$$"
mkdir -p "$D"
trap 'rm -rf "$D"' EXIT
N=500

t() { # label setup-cmd bs extra-dd-flags
  local lbl=$1 setup=$2 bs=$3 extra=${4:-}
  rm -f "$D/f"; eval "$setup"; sync
  local r
  # shellcheck disable=SC2086
  r=$( { TIMEFORMAT=%R; time dd if=/dev/zero of="$D/f" bs=$bs count=$N \
         oflag=dsync status=none $extra; } 2>&1 )
  awk -v l="$lbl" -v r="$r" -v n="$N" -v b="$bs" \
    'BEGIN{printf "%-44s %8.0f us/op   %7.1f MiB/s\n", l, r*1e6/n, n*b/1048576/r}'
}

echo "# one durable write, decomposed   ($N ops each)"
echo
t "1 overwrite 4096, blocks already written"  "dd if=/dev/zero of=$D/f bs=4096 count=$N status=none" 4096 "conv=notrunc"
t "2 overwrite 4128, blocks already written"  "dd if=/dev/zero of=$D/f bs=4128 count=$N status=none" 4128 "conv=notrunc"
t "3 extend  4096, fresh file"                "true" 4096
t "4 extend  4128, fresh file  <- raf's shape" "true" 4128
t "5 extend  4128, fallocate'd first"         "fallocate -l $((N*4128+1048576)) $D/f" 4128 "conv=notrunc"

# raf itself, same shape, for the comparison the dd rows exist to frame.
REPO=$(cd "$(dirname "$0")/../.." && pwd)
BUILD="$REPO/build-bench"
if cmake -S "$REPO" -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
         -DRAF_BUILD_TESTS=OFF >/dev/null 2>&1 &&
   cmake --build "$BUILD" -j"$(nproc)" --target raf_matrix >/dev/null 2>&1; then
  for b in 1 32; do
    rm -rf "$D/a"; mkdir -p "$D/a"
    r=$("$BUILD/raf_matrix" write --dir "$D/a" --record 4096 --threads 1 \
        --batch $b --count $(( N * b )))
    awk -v b="$b" -v m="$(cut -d" " -f1 <<<"$r")" -v p="$(cut -d" " -f3 <<<"$r")" \
      'BEGIN{printf "%-44s %8.0f us/op   %7.1f MiB/s\n", "raf append, batch=" b, p, m}'
    rm -rf "$D/a"
  done
  # Options::preallocateBytes: zeros written ahead of the append point so an
  # append overwrites instead of extending. Whether this helps is the whole
  # question -- it turns row 4 into row 1 if the drive likes overwrites, and
  # costs double the writes if it does not. On the VM this was developed on
  # it measured 8x worse; on a drive where row 1 beats row 4 it should win.
  rm -rf "$D/a"; mkdir -p "$D/a"
  r=$("$BUILD/raf_matrix" write --dir "$D/a" --record 4096 --threads 1 \
      --batch 1 --count $N --prealloc $(( 64 * 1024 * 1024 )))
  awk -v m="$(cut -d" " -f1 <<<"$r")" -v p="$(cut -d" " -f3 <<<"$r")" \
    'BEGIN{printf "%-44s %8.0f us/op   %7.1f MiB/s\n", "raf append, batch=1, prealloc 64M", p, m}'
  rm -rf "$D/a"
else
  echo "(could not build raf_matrix; skipping the raf rows)"
fi


# A short burst and a long run can disagree by a lot: the drive absorbs the
# first megabytes into cache, and a journal reaches steady state only after
# a while. This appends into one archive repeatedly and prints the curve, so
# a slow sustained number can be told from a slow start.
if [ "$SUSTAINED" = 1 ] && [ -x "$BUILD/raf_matrix" ]; then
  echo
  echo "# sustained: successive 4000-record chunks into one archive"
  rm -rf "$D/a"; mkdir -p "$D/a"
  for i in $(seq 8); do
    r=$("$BUILD/raf_matrix" write --dir "$D/a" --record 4096 --threads 1 \
        --batch 1 --count 4000)
    awk -v i="$i" -v m="$(cut -d' ' -f1 <<<"$r")" -v p="$(cut -d' ' -f3 <<<"$r")" \
        -v q="$(cut -d' ' -f4 <<<"$r")" \
      'BEGIN{printf "  records %6d..%6d  %7.1f MiB/s   p50 %6.0f us   p99 %8.0f us\n", (i-1)*4000, i*4000, m, p, q}'
  done
  rm -rf "$D/a"
  echo "#   flat          the short and long numbers disagree for another reason"
  echo "#   falling       the drive or the journal degrades under sustained flushes;"
  echo "#                 the sustained figure is the real one"
fi

echo
echo "# reading it"
echo "#   2 >> 1          the 32-byte header costs a second page per record"
echo "#   3 >> 1          extending the file costs a metadata commit per append"
echo "#   5 vs 4          on ext4 these came out equal here: fallocate does not"
echo "#                    help, because converting an unwritten extent to a"
echo "#                    written one is itself a metadata commit"
echo "#   raf b=1 >> 4    the remainder is raf's own code"
echo "#   raf b=32 ~ 4/32  batching amortises the flush; if it does not, the"
echo "#                    cost is per-append and not per-flush"
echo "#   prealloc ~ 1     preallocation worked: appends became overwrites"
echo "#   prealloc >> 4    this drive dislikes overwrites; leave the option off"
