#!/bin/bash
# How close does raf get to the raw device?
#
#   ./vsfio.sh --mount /mnt/raf-bench --out ./results
#   ./vsfio.sh --mount /mnt/raf-bench --workloads rr4k,sw4k --workers "1 16 256"
#
# fio doing the same shape of I/O is the floor: it is the same system calls
# against a plain file with no format, no checksum and no index. Whatever gap
# is left is what raf costs. Six workloads, each swept from 1 to 256 workers:
#
#   rr4k  4 KiB random read        raf's random read against fio randread
#   rr4ka 4064 B random read       the control for rr4k: a payload of 4064
#                                  plus a 32 byte header is exactly one page,
#                                  so raf and fio touch the same bytes. The
#                                  gap that survives this is not alignment.
#   rr1m  1 MiB random read        the same at a size where bandwidth, not
#                                  IOPS, is the limit
#   sr4k  4 KiB sequential read    raf iteration against fio sequential read
#   sr1m  1 MiB sequential read
#   sw4k  4 KiB durable append     raf append against fio write + fdatasync
#   sw4ka 4064 B durable append    the same control on the write path. raf
#                                  writes 4096 bytes per record here, exactly
#                                  what fio writes, and never rewrites a page
#                                  tail the next append has to write again.
#   sw1m  1 MiB durable append
#
# Comparability, and where it is imperfect:
#
#   * fio runs ioengine=psync because that is what raf's posix io layer does.
#     An io_uring fio number would be measuring a different program.
#   * writes are durable on both sides: raf opens O_DSYNC, fio gets
#     --fdatasync=1. fio's clat stops before the flush, so the p50 column adds
#     fio's sync latency back in; without that fio looks ten times faster than
#     it is.
#   * raf writes 4128 bytes per 4 KiB record -- the payload plus its header --
#     and fio writes 4096. A few percent of the write gap is that, by
#     construction, and no tuning will remove it. The 4064 B rows exist to
#     take it out: there raf's record is exactly one page and the two sides
#     move the same bytes. raf's MiB/s still counts payload only, so it
#     carries a 0.8% handicap in those rows -- far below what they are
#     looking for.
#   * raf keeps one writer per segment, so W workers means W files on both
#     sides.
#   * fio gets --fallocate=none. Its default is native, which reserves the
#     whole file with fallocate(2) before the first write, so every write
#     lands in an existing extent and never costs a size change. raf extends
#     a segment as it appends. Left at the default, the two columns are
#     different workloads with the same heading.
#   * fio and raf run back to back at each point, and which of them goes
#     first alternates between repetitions. On a drive whose durable-write
#     cost steps up with cumulative bytes -- and this one's does -- whoever
#     runs second runs on a worse drive. A fixed order turned raf/fio from
#     1.15 into 0.12 at 16 writers between two runs of the same command, so
#     --reps 1 is not enough for the write sweep whatever else it is enough
#     for.
#
# Caches are dropped per read run with posix_fadvise(DONTNEED), which needs no
# privileges. That empties the page cache for these files only; if the dataset
# is smaller than RAM the drive may still have it in its own cache, so --gib
# should exceed RAM for the read numbers to mean what they say.
#
# fio's own --invalidate is turned off and the same drop is used on both
# sides. Left on, each of W jobs invalidates the shared file as it starts, so
# a late job evicts what an early one has already read -- which costs fio
# throughput for a reason that has nothing to do with raf.
#
# Needs: fio, python3, cmake + a compiler. No root.
set -u

MOUNT=""; OUT="$(pwd)/results"; WORKERS="1 2 4 8 16 32 64 128 256"
WORKLOADS="rr4k,rr4ka,rr1m,sr4k,sr1m,sw4k,sw4ka,sw1m"; GIB=""; WRITE_MIB=16
OPS4K=200000; OPS1M=4000; REPS=2
while [ $# -gt 0 ]; do
  case "$1" in
    --mount)     MOUNT="$2"; shift 2 ;;
    --out)       OUT="$2"; shift 2 ;;
    --workers)   WORKERS="$2"; shift 2 ;;
    --workloads) WORKLOADS="$2"; shift 2 ;;
    --gib)       GIB="$2"; shift 2 ;;
    --write-mib) WRITE_MIB="$2"; shift 2 ;;
    --ops4k)     OPS4K="$2"; shift 2 ;;
    --ops1m)     OPS1M="$2"; shift 2 ;;
    --reps)      REPS="$2"; shift 2 ;;
    *) echo "usage: $0 --mount DIR [--out DIR] [--workers \"1 2 .. 256\"]" \
            "[--workloads rr4k,rr4ka,rr1m,sr4k,sr1m,sw4k,sw4ka,sw1m] [--gib N]" \
            "[--write-mib N] [--ops4k N] [--ops1m N] [--reps N]" >&2; exit 2 ;;
  esac
done
[ -n "$MOUNT" ] && [ -d "$MOUNT" ] || { echo "error: --mount DIR is required and must exist" >&2; exit 2; }
command -v fio >/dev/null     || { echo "error: fio is not installed" >&2; exit 2; }
command -v python3 >/dev/null || { echo "error: python3 is needed to parse fio's json" >&2; exit 2; }

REPO=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$OUT" 2>/dev/null || true
RESULTS="$OUT/vsfio.txt"
if ! : 2>/dev/null > "$RESULTS"; then
  echo "error: cannot write $RESULTS" >&2
  [ -d "$OUT" ] && echo "  $OUT is owned by $(stat -c %U "$OUT" 2>/dev/null); you are $(id -un)." >&2
  exit 1
fi
WORK="$MOUNT/raf-vsfio"
rm -rf "$WORK"; mkdir -p "$WORK" || { echo "error: cannot create $WORK" >&2; exit 1; }
trap 'rm -rf "$WORK"' EXIT

part=$(df --output=source "$MOUNT" | tail -1)
DEV=$(lsblk -no PKNAME "$part" 2>/dev/null | head -1)
[ -n "$DEV" ] || DEV=$(basename "$part" | sed 's/p\?[0-9]*$//')
MEM_GIB=$(awk '/MemTotal/{printf "%.0f", $2/1048576}' /proc/meminfo)
# Bigger than RAM by default, so a dropped page cache is really a cold read.
[ -n "$GIB" ] || GIB=$(( MEM_GIB + MEM_GIB / 2 ))

log()  { printf '%s\n' "$*" | tee -a "$RESULTS"; }
has()  { case ",$WORKLOADS," in *,$1,*) return 0 ;; *) return 1 ;; esac; }
field(){ cut -d' ' -f"$2" <<<"$1"; }

# Drop the page cache for named files without being root. The drive's own
# cache is out of reach, which is why the dataset wants to be large.
cold() {
  sync
  find "$@" -type f 2>/dev/null | python3 -c '
import os, sys
for path in sys.stdin.read().split("\n"):
    if not path: continue
    try:
        fd = os.open(path, os.O_RDONLY)
    except OSError:
        continue
    try:
        os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
    finally:
        os.close(fd)
'
}

# ------------------------------------------------------------------- building
BUILD="$REPO/build-bench"
cmake -S "$REPO" -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRAF_BUILD_TESTS=OFF \
      >/dev/null || { echo "error: cmake failed; see $BUILD" >&2; exit 1; }
cmake --build "$BUILD" -j"$(nproc)" --target raf_matrix >/dev/null || {
  echo "error: build failed" >&2; exit 1; }
BIN="$BUILD/raf_matrix"

log "# raf against fio   $(date -Is)"
log "# device /dev/$DEV   mount $MOUNT   RAM ${MEM_GIB} GiB   dataset ${GIB} GiB"
log "# fio $(fio --version)   ioengine psync   workers: $WORKERS   reps $REPS"
[ "$GIB" -le "$MEM_GIB" ] && log "# WARNING: dataset <= RAM; read numbers are partly memory"
[ $(( REPS % 2 )) -eq 1 ] && log "# WARNING: odd --reps; fio goes first more often than raf does"
log ""

# fio, one line out: mib_s ops_s p50_us p99_us.
#
# The percentiles are completion latency, plus fdatasync latency when there is
# one, because a durable write is not finished until the flush returns.
fiorun() { # rw bs jobs rest...
  local rw="$1" bs="$2" jobs="$3"; shift 3
  local json="$WORK/fio.json"
  fio --name=w --rw="$rw" --bs="$bs" --numjobs="$jobs" --ioengine=psync \
      --group_reporting=1 --randrepeat=1 --thread --output-format=json \
      --output="$json" "$@" >/dev/null 2>&1 || { echo "- - - -"; return; }
  python3 - "$json" "$rw" <<'PY'
import json, sys
j = json.load(open(sys.argv[1]))
job = j["jobs"][0]
side = "read" if "read" in sys.argv[2] else "write"
s = job[side]
def pct(block, q):
    p = block.get("clat_ns", block.get("lat_ns", {})).get("percentile", {})
    return p.get(q, 0) / 1000.0
p50 = pct(s, "50.000000")
p99 = pct(s, "99.000000")
sync = job.get("sync", {})
if sync.get("lat_ns", {}).get("percentile"):
    p = sync["lat_ns"]["percentile"]
    p50 += p.get("50.000000", 0) / 1000.0
    p99 += p.get("99.000000", 0) / 1000.0
print("%.1f %.0f %.1f %.1f" % (s["bw_bytes"] / 1048576.0, s["iops"], p50, p99))
PY
}

# Best of REPS lines by throughput, carrying that run's latencies with it.
#
# It used to be the median, which at two repetitions is the lower of the two,
# so a single interfered run set the column. That is how this script reported
# 4 KiB durable append at 0.25 of fio at sixteen writers: best of three on the
# same machine puts it at 0.97. The benchmark machine is not idle, and
# interference only ever makes a run slower, so the best run is the one with
# the least of something that is not being measured. Both sides get it, and
# position is already balanced by alternating who goes first.
med() { sort -n -k1 | tail -1; }
FLINES="$WORK/.fio.lines"; RLINES="$WORK/.raf.lines"

# Which side goes first in repetition `i`.
#
# This drive's durable-write cost steps up permanently with cumulative bytes
# written, so whoever runs second runs on a worse drive. Fixed order turned a
# 1.15 into a 0.12 at 16 writers between two runs of the same command. The
# order alternates, and --reps 1 keeps the bias -- an even --reps is the
# honest minimum for the write sweep.
order() { if [ $(( $1 % 2 )) -eq 1 ]; then echo "fio raf"; else echo "raf fio"; fi; }

row() { # label fio-line raf-line
  local r a; r=$(awk -v a="$(field "$3" 1)" -v b="$(field "$2" 1)" \
      'BEGIN{ if (b+0 > 0 && a ~ /^[0-9.]+$/) printf "%.2f", a/b; else printf "-" }')
  # Device bytes over payload bytes, raf's side only. A read ratio near 0.5
  # next to an amp near 2.0 is one fact, not two: the record spans a page it
  # did not need to.
  a=$(awk -v v="$(field "$3" 7)" 'BEGIN{ if (v+0 > 0) printf "%.2f", v; else printf "-" }')
  printf '%-8s %10s %10s %7s | %9s %9s | %9s %9s | %5s\n' "$1" \
    "$(field "$2" 1)" "$(field "$3" 1)" "$r" \
    "$(field "$2" 3)" "$(field "$3" 3)" "$(field "$2" 4)" "$(field "$3" 4)" "$a" \
    | tee -a "$RESULTS"
}

header() {
  log "## $1"
  log ""
  printf '%-8s %10s %10s %7s | %9s %9s | %9s %9s | %5s\n' \
    workers "fio MiB/s" "raf MiB/s" "raf/fio" "fio p50" "raf p50" "fio p99" "raf p99" "amp" \
    | tee -a "$RESULTS"
}

# --------------------------------------------------------------------- set-up
# One archive per record size, laid down with a big batch because prep time is
# not the measurement; and one fio file of the same size beside it.
ARCH4K="$WORK/arch4k"; ARCH1M="$WORK/arch1m"; ARCH4KA="$WORK/arch4ka"
FIOFILE="$WORK/fio.dat"
need_reads=0
for w in rr4k rr4ka rr1m sr4k sr1m; do has $w && need_reads=1; done
if [ $need_reads = 1 ]; then
  log "preparing ${GIB} GiB of data (a few minutes)..."
  fio --name=prep --rw=write --bs=1m --size="${GIB}g" --filename="$FIOFILE" \
      --end_fsync=1 --ioengine=psync >/dev/null 2>&1 || {
    echo "error: could not lay down $FIOFILE" >&2; exit 1; }
  if has rr4k || has sr4k; then
    "$BIN" write --dir "$ARCH4K" --record 4096 --batch 256 \
           --count $(( GIB * 262144 )) >/dev/null || exit 1
    has rr4k && "$BIN" scan --dir "$ARCH4K" >/dev/null
  fi
  if has rr4ka; then
    "$BIN" write --dir "$ARCH4KA" --record 4064 --batch 256 \
           --count $(( GIB * 264241 )) >/dev/null || exit 1
    "$BIN" scan --dir "$ARCH4KA" >/dev/null
  fi
  if has rr1m || has sr1m; then
    "$BIN" write --dir "$ARCH1M" --record 1048576 --batch 4 \
           --count $(( GIB * 1024 )) >/dev/null || exit 1
    has rr1m && "$BIN" scan --dir "$ARCH1M" >/dev/null
  fi
  log ""
fi

# ---------------------------------------------------------------------- reads
for wl in rr4k rr4ka rr1m sr4k sr1m; do
  has $wl || continue
  case $wl in
    rr4k) rec=4096;    bs=4k; mode=read;    arch=$ARCH4K; ops=$OPS4K; rw=randread; name="4 KiB random read" ;;
    rr4ka) rec=4064;   bs=4k; mode=read;    arch=$ARCH4KA; ops=$OPS4K; rw=randread; name="4064 B random read, page aligned" ;;
    rr1m) rec=1048576; bs=1m; mode=read;    arch=$ARCH1M; ops=$OPS1M; rw=randread; name="1 MiB random read" ;;
    sr4k) rec=4096;    bs=4k; mode=seqread; arch=$ARCH4K; ops=$OPS4K; rw=read;     name="4 KiB sequential read" ;;
    sr1m) rec=1048576; bs=1m; mode=seqread; arch=$ARCH1M; ops=$OPS1M; rw=read;     name="1 MiB sequential read" ;;
  esac
  # Random reads split the work: W workers share one walk, so the device sees
  # the same number of lookups however many threads ask for them. Sequential
  # readers cannot be split -- the public reader has no way to start partway
  # through an archive -- so each one streams the whole run, which is also
  # what fio's numjobs do over one file. Beyond a worker or two that measures
  # the page cache on both sides; the ratio still means something, the
  # absolute MiB/s does not.
  case $mode in
    read)    per_note="$ops operations spread over the workers" ;;
    seqread) per_note="$ops records streamed by every worker, cache shared" ;;
  esac
  header "$name   cold, $per_note"
  for w in $WORKERS; do
    case $mode in read) per=$(( ops / w )) ;; *) per=$ops ;; esac
    [ $per -lt 64 ] && per=64
    : > "$FLINES"; : > "$RLINES"
    for i in $(seq "$REPS"); do
      for side in $(order "$i"); do
        case $side in
          fio) cold "$FIOFILE"
               fiorun "$rw" "$bs" "$w" --filename="$FIOFILE" --number_ios="$per" \
                      --invalidate=0 >> "$FLINES" ;;
          raf) cold "$arch"
               { "$BIN" "$mode" --dir "$arch" --record "$rec" --threads "$w" \
                        --count "$per" --device "$DEV" || echo "- - - -"; } >> "$RLINES" ;;
        esac
      done
    done
    row "$w" "$(med < "$FLINES")" "$(med < "$RLINES")"
  done
  log ""
done

# --------------------------------------------------------------------- writes
# A fresh archive and fresh fio files per point: an append that extends a file
# is a different cost from one that overwrites, and reusing files would turn
# half the sweep into the other measurement.
for wl in sw4k sw4ka sw1m; do
  has $wl || continue
  case $wl in
    sw4k)  rec=4096;    bs=4k; bsb=4096;    name="4 KiB durable append" ;;
    sw4ka) rec=4064;    bs=4k; bsb=4096;    name="4064 B durable append, page aligned" ;;
    sw1m)  rec=1048576; bs=1m; bsb=1048576; name="1 MiB durable append" ;;
  esac
  # At 1 MiB a 16 MiB budget is sixteen operations, which is a sample size
  # rather than a measurement; the floor keeps every point at 32 or more.
  per=$(( WRITE_MIB * 1048576 / rec )); [ $per -lt 32 ] && per=32
  # fio is given the operation count, not a round size: rounding its file to
  # whole megabytes while raf writes `per` records had fio moving a quarter
  # less data than raf in the 4064 B rows.
  fio_bytes=$(( per * bsb ))
  header "$name   $(( fio_bytes / 1048576 )) MiB per worker, $per operations each"
  for w in $WORKERS; do
    : > "$FLINES"; : > "$RLINES"
    for i in $(seq "$REPS"); do
      for side in $(order "$i"); do
        case $side in
          fio) rm -rf "$WORK/fiow"; mkdir -p "$WORK/fiow"
               fiorun write "$bs" "$w" --directory="$WORK/fiow" --size="$fio_bytes" \
                      --fdatasync=1 --fallocate=none --unlink=1 >> "$FLINES"
               rm -rf "$WORK/fiow" ;;
          raf) rm -rf "$WORK/warch"
               { "$BIN" write --dir "$WORK/warch" --record "$rec" --threads "$w" \
                        --count "$per" || echo "- - - -"; } >> "$RLINES"
               rm -rf "$WORK/warch" ;;
        esac
      done
    done
    row "$w" "$(med < "$FLINES")" "$(med < "$RLINES")"
  done
  log ""
done

log "written to $RESULTS"
