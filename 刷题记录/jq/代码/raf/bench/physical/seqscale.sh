#!/bin/bash
# Sequential read and sequential write, raf against fio, swept until the
# drive is the limit.
#
#   ./seqscale.sh --mount /mnt/raf-bench --out ~/raf-results
#   ./seqscale.sh --mount /mnt/raf-bench --what read --records 1m
#
# The question is not whether raf is close to fio when nothing is loaded --
# vsfio.sh answers that -- but whether it is still close when the device is
# saturated. A single buffered reader at queue depth one gets nowhere near an
# NVMe's bandwidth, so saturation means many streams, and many streams only
# mean anything if they are reading different bytes.
#
# That is what this script fixes. Every worker gets its own stream on both
# sides: raf readers each open their own archive (raf_matrix --shards), fio
# jobs each get their own file. Pointing W readers at one archive -- which is
# all the public reader can do, since it cannot start partway through --
# leaves them walking the same prefix, and past a worker or two that is a
# memcpy benchmark. vsfio.sh's sequential rows have exactly that problem and
# say so; these do not.
#
#   read   cold sequential read, W disjoint streams. Three columns: fio,
#          raf buffered, and raf-od -- the same RecordReader with
#          Options::directReads off and on. It is on by default, so the
#          buffered column is the one that passes a flag.
#          ceiling.sh puts buffered reads on this machine at 733-936 MiB/s
#          whatever the engine or queue depth and O_DIRECT at 1775-1935; the
#          raf-od column asks whether raf's framing and checksums can keep up
#          once the bytes arrive that fast. It copies each payload out of the
#          window, which the buffered path does not, and that copy is what
#          the 2.5x would cost.
#   write  durable append, W writers, one segment each -- already disjoint,
#          since raf gives every writer its own segment
#   rand   cold random reads, the same three columns. Sequential said
#          O_DIRECT wins by 2.2x, but that does not carry over on its own:
#          a buffered random read that hits the page cache costs nothing and
#          O_DIRECT has no hits to get, while O_DIRECT reads 512 byte
#          sectors where the cache reads 4096 byte pages, so a 4128 byte
#          record costs it 4608 bytes instead of 8192. The archive is 1.5x
#          RAM by default so the hit rate is honest.
#
# Caches are dropped per read run with posix_fadvise(DONTNEED) on both sides.
# Needs: fio, python3, cmake. No root.
set -u

MOUNT=""; OUT="$(pwd)/results"; WORKERS="1 2 4 8 16 32"; STREAM_MIB=512
WRITE_MIB=64; REPS=2; RECORDS="4k 1m"; WHAT="read,write"; RAND_GIB=""; RAND_OPS=200000
# How fio is asked to make a write durable. raf opens O_DSYNC, so that is the
# default; fdatasync is the other way to spell the same contract and the two
# are not obliged to cost the same.
FIO_SYNC=dsync
# Record size for the random-read section. The sequential sections take
# --records; this one is separate because it needs its own archive.
RAND_RECORD=4096
while [ $# -gt 0 ]; do
  case "$1" in
    --mount)      MOUNT="$2"; shift 2 ;;
    --out)        OUT="$2"; shift 2 ;;
    --workers)    WORKERS="$2"; shift 2 ;;
    --stream-mib) STREAM_MIB="$2"; shift 2 ;;
    --write-mib)  WRITE_MIB="$2"; shift 2 ;;
    --reps)       REPS="$2"; shift 2 ;;
    --records)    RECORDS="$2"; shift 2 ;;
    --what)       WHAT="$2"; shift 2 ;;
    --rand-gib)   RAND_GIB="$2"; shift 2 ;;
    --rand-ops)   RAND_OPS="$2"; shift 2 ;;
    --rand-record) RAND_RECORD="$2"; shift 2 ;;
    --fio-sync)   FIO_SYNC="$2"; shift 2 ;;
    *) echo "usage: $0 --mount DIR [--out DIR] [--workers \"1 2 .. 32\"]" \
            "[--stream-mib N] [--write-mib N] [--reps N] [--records \"4k 1m\"]" \
            "[--what read,write,rand] [--rand-gib N] [--rand-ops N]" \
            "[--fio-sync dsync|fdatasync] [--rand-record BYTES]" >&2; exit 2 ;;
  esac
done
[ -n "$MOUNT" ] && [ -d "$MOUNT" ] || { echo "error: --mount DIR is required and must exist" >&2; exit 2; }
command -v fio >/dev/null     || { echo "error: fio is not installed" >&2; exit 2; }
command -v python3 >/dev/null || { echo "error: python3 is needed to parse fio's json" >&2; exit 2; }

REPO=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$OUT" 2>/dev/null || true
RESULTS="$OUT/seqscale.txt"
: 2>/dev/null > "$RESULTS" || { echo "error: cannot write $RESULTS" >&2; exit 1; }
WORK="$MOUNT/raf-seqscale"
rm -rf "$WORK"; mkdir -p "$WORK" || { echo "error: cannot create $WORK" >&2; exit 1; }
trap 'rm -rf "$WORK"' EXIT

part=$(df --output=source "$MOUNT" | tail -1)
DEV=$(lsblk -no PKNAME "$part" 2>/dev/null | head -1)
[ -n "$DEV" ] || DEV=$(basename "$part" | sed 's/p\?[0-9]*$//')
MEM_GIB=$(awk '/MemTotal/{printf "%.0f", $2/1048576}' /proc/meminfo)
MAXW=$(printf '%s\n' $WORKERS | sort -n | tail -1)

# raf keeps two descriptors per segment -- the O_DSYNC one it appends
# through and an unsynced one for the frontier -- so W writers want 2W, and
# the default soft limit of 1024 bites at 512. Raising it to the hard limit
# needs no privileges and is the difference between measuring raf and
# measuring RLIMIT_NOFILE.
ulimit -n "$(ulimit -Hn)" 2>/dev/null || true

log()  { printf '%s\n' "$*" | tee -a "$RESULTS"; }
want() { case ",$WHAT," in *,$1,*) return 0 ;; *) return 1 ;; esac; }
field(){ cut -d' ' -f"$2" <<<"$1"; }
# Best of REPS, not the median: interference on a machine that is not idle
# only ever makes a run slower. Both sides get the same treatment.
med()  { sort -n -k1 | tail -1; }

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

BUILD="$REPO/build-bench"
cmake -S "$REPO" -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRAF_BUILD_TESTS=OFF \
      >/dev/null || { echo "error: cmake failed; see $BUILD" >&2; exit 1; }
cmake --build "$BUILD" -j"$(nproc)" --target raf_matrix >/dev/null || {
  echo "error: build failed" >&2; exit 1; }
BIN="$BUILD/raf_matrix"

log "# raf against fio, swept to saturation   $(date -Is)"
log "# device /dev/$DEV   mount $MOUNT   RAM ${MEM_GIB} GiB"
log "# $(fio --version)   ioengine psync   workers: $WORKERS   reps $REPS (best of)"
[ -n "$RAND_GIB" ] || RAND_GIB=$(( MEM_GIB + MEM_GIB / 2 ))
case "$FIO_SYNC" in
  dsync)     fio_sync_arg="--sync=dsync" ;;
  fdatasync) fio_sync_arg="--fdatasync=1" ;;
  *) echo "error: --fio-sync takes dsync or fdatasync" >&2; exit 2 ;;
esac
log "# read ${STREAM_MIB} MiB per stream   write ${WRITE_MIB} MiB per worker"
want write && log "# fio makes writes durable with $fio_sync_arg; raf opens O_DSYNC"
want rand && log "# random ${RAND_GIB} GiB archive   $RAND_OPS operations per point"
log ""

fiorun() { # rw bs jobs rest...
  local rw="$1" bs="$2" jobs="$3"; shift 3
  # psync unless the caller says otherwise; a later --ioengine wins, which
  # is how the ceiling rows ask for io_uring.
  fio --name=w --rw="$rw" --bs="$bs" --numjobs="$jobs" --ioengine=psync \
      --group_reporting=1 --thread --output-format=json --output="$WORK/fio.json" \
      "$@" >/dev/null 2>&1 || { echo "- - - -"; return; }
  python3 - "$WORK/fio.json" "$rw" <<'PY'
import json, sys
job = json.load(open(sys.argv[1]))["jobs"][0]
s = job["read" if "read" in sys.argv[2] else "write"]
p = s.get("clat_ns", {}).get("percentile", {})
p50 = p.get("50.000000", 0) / 1000.0
p99 = p.get("99.000000", 0) / 1000.0
sync = job.get("sync", {}).get("lat_ns", {}).get("percentile", {})
p50 += sync.get("50.000000", 0) / 1000.0
p99 += sync.get("99.000000", 0) / 1000.0
print("%.1f %.0f %.1f %.1f" % (s["bw_bytes"] / 1048576.0, s["iops"], p50, p99))
PY
}

header() {
  log "## $1"
  log ""
  printf '%-8s %10s %10s %7s | %9s %9s | %9s %9s | %5s\n' \
    workers "fio MiB/s" "raf MiB/s" "raf/fio" "fio p50" "raf p50" "fio p99" "raf p99" "amp" \
    | tee -a "$RESULTS"
}

header_od() {
  log "## $1"
  log ""
  printf '%-8s | %9s %9s %7s | %9s %9s %7s | %5s %5s\n' \
    workers "fio buf" "raf buf" "ratio" "fio O_DIR" "raf O_DIR" "ratio" "amp" "amp-d" \
    | tee -a "$RESULTS"
}
ratio() { # numerator-line denominator-line
  awk -v a="$(field "$1" 1)" -v b="$(field "$2" 1)" \
      'BEGIN{ if (b+0 > 0 && a ~ /^[0-9.]+$/) printf "%.2f", a/b; else printf "-" }'
}
ampof() { awk -v v="$(field "$1" 7)" 'BEGIN{ if (v+0 > 0) printf "%.2f", v; else printf "-" }'; }

# workers fio-buffered raf-buffered fio-direct raf-direct
row_od() {
  printf '%-8s | %9s %9s %7s | %9s %9s %7s | %5s %5s\n' "$1" \
    "$(field "$2" 1)" "$(field "$3" 1)" "$(ratio "$3" "$2")" \
    "$(field "$4" 1)" "$(field "$5" 1)" "$(ratio "$5" "$4")" \
    "$(ampof "$3")" "$(ampof "$5")" | tee -a "$RESULTS"
}

# The drive, measured in this same run rather than quoted from another one.
# A ceiling taken an hour earlier on a drive that drifts is not a ceiling.
ceiling_line() { # label rw bs extra...
  local rw="$2" bs="$3"; local lbl="$1"; shift 3
  local one eight
  one=$(fiorun "$rw" "$bs" 1 --ioengine=io_uring --iodepth=64 --direct=1 "$@")
  eight=$(fiorun "$rw" "$bs" 8 --ioengine=io_uring --iodepth=64 --direct=1 "$@")
  log "   drive ceiling, O_DIRECT io_uring depth 64: $(field "$one" 1) MiB/s at 1 job," \
      "$(field "$eight" 1) at 8   ($lbl)"
}
row() { # workers fio-line raf-line
  local r a
  r=$(awk -v a="$(field "$3" 1)" -v b="$(field "$2" 1)" \
      'BEGIN{ if (b+0 > 0 && a ~ /^[0-9.]+$/) printf "%.2f", a/b; else printf "-" }')
  a=$(awk -v v="$(field "$3" 7)" 'BEGIN{ if (v+0 > 0) printf "%.2f", v; else printf "-" }')
  printf '%-8s %10s %10s %7s | %9s %9s | %9s %9s | %5s\n' "$1" \
    "$(field "$2" 1)" "$(field "$3" 1)" "$r" \
    "$(field "$2" 3)" "$(field "$3" 3)" "$(field "$2" 4)" "$(field "$3" 4)" "$a" \
    | tee -a "$RESULTS"
}

for rspec in $RECORDS; do
  case $rspec in
    4k) rec=4096;    bs=4k; prep_batch=256 ;;
    1m) rec=1048576; bs=1m; prep_batch=4 ;;
    *) echo "error: --records takes 4k and 1m" >&2; exit 2 ;;
  esac

  # ------------------------------------------------------------------ read
  if want read; then
    ARCH="$WORK/r$rspec"; FIODIR="$WORK/f$rspec"; mkdir -p "$FIODIR"
    per=$(( STREAM_MIB * 1048576 / rec ))
    log "preparing $MAXW streams of ${STREAM_MIB} MiB for $rspec reads..."
    t=0
    while [ $t -lt "$MAXW" ]; do
      "$BIN" write --dir "$ARCH.$t" --record "$rec" --batch "$prep_batch" \
             --count "$per" >/dev/null || exit 1
      t=$(( t + 1 ))
    done
    fio --name=prep --rw=write --bs=1m --numjobs="$MAXW" --directory="$FIODIR" \
        --size=$(( per * rec )) --end_fsync=1 --ioengine=psync --thread \
        >/dev/null 2>&1 || { echo "error: fio prep failed" >&2; exit 1; }
    log ""

    ceiling_line "$rspec sequential" read "$bs" --directory="$FIODIR" \
                 --size=$(( per * rec )) --invalidate=1
    header_od "$rspec sequential read   ${STREAM_MIB} MiB per stream, every worker its own"
    for w in $WORKERS; do
      : > "$WORK/.f"; : > "$WORK/.r"; : > "$WORK/.fd"; : > "$WORK/.rd"
      for i in $(seq "$REPS"); do
        # Four sides, and the order reverses every repetition so none of them
        # is always last on a drive whose state moves.
        order="fio raf fiod rafd"
        [ $(( i % 2 )) -eq 0 ] && order="rafd fiod raf fio"
        for side in $order; do
          case $side in
            fio)  cold "$FIODIR"
                  fiorun read "$bs" "$w" --directory="$FIODIR" --size=$(( per * rec )) \
                         --invalidate=0 >> "$WORK/.f" ;;
            fiod) cold "$FIODIR"
                  fiorun read "$bs" "$w" --directory="$FIODIR" --size=$(( per * rec )) \
                         --invalidate=0 --direct=1 >> "$WORK/.fd" ;;
            raf)  cold $(for ((t=0;t<w;t++)); do printf '%s ' "$ARCH.$t"; done)
                  { "$BIN" seqread --dir "$ARCH" --shards "$w" --record "$rec" \
                           --threads "$w" --count "$per" --device "$DEV" --buffered \
                      || echo "- - - -"; } >> "$WORK/.r" ;;
            rafd) cold $(for ((t=0;t<w;t++)); do printf '%s ' "$ARCH.$t"; done)
                  { "$BIN" seqread --dir "$ARCH" --shards "$w" --record "$rec" \
                           --threads "$w" --count "$per" --device "$DEV" \
                      || echo "- - - -"; } >> "$WORK/.rd" ;;
          esac
        done
      done
      row_od "$w" "$(med < "$WORK/.f")" "$(med < "$WORK/.r")" \
             "$(med < "$WORK/.fd")" "$(med < "$WORK/.rd")"
    done
    rm -rf "$ARCH".* "$FIODIR"
    log ""
  fi

  # ----------------------------------------------------------------- write
  # Already disjoint without help: raf hands every writer its own segment,
  # and fio every job its own file.
  if want write; then
    per=$(( WRITE_MIB * 1048576 / rec )); [ $per -lt 32 ] && per=32
    header "$rspec durable append   $(( per * rec / 1048576 )) MiB per worker, $per appends each"
    for w in $WORKERS; do
      : > "$WORK/.f"; : > "$WORK/.r"
      for i in $(seq "$REPS"); do
        for side in $(if [ $(( i % 2 )) -eq 1 ]; then echo "fio raf"; else echo "raf fio"; fi); do
          case $side in
            fio) rm -rf "$WORK/fw"; mkdir -p "$WORK/fw"
                 fiorun write "$bs" "$w" --directory="$WORK/fw" --size=$(( per * rec )) \
                        "$fio_sync_arg" --fallocate=none --unlink=1 >> "$WORK/.f"
                 rm -rf "$WORK/fw" ;;
            raf) rm -rf "$WORK/rw"
                 { "$BIN" write --dir "$WORK/rw" --record "$rec" --threads "$w" \
                          --count "$per" --device "$DEV" || echo "- - - -"; } >> "$WORK/.r"
                 rm -rf "$WORK/rw" ;;
          esac
        done
      done
      row "$w" "$(med < "$WORK/.f")" "$(med < "$WORK/.r")"
    done
    log ""
  fi
done

# ------------------------------------------------------------------ random
if want rand; then
  ARCH="$WORK/rand"; FIOFILE="$WORK/randfio.dat"
  rand_span=$(( RAND_RECORD + 32 ))
  rand_bs=$(( (RAND_RECORD + 4095) / 4096 * 4096 ))
  per_rec=$(( RAND_GIB * 1073741824 / rand_span ))
  rand_batch=256; [ "$RAND_RECORD" -gt 65536 ] && rand_batch=4
  log "preparing a ${RAND_GIB} GiB archive for random reads..."
  "$BIN" write --dir "$ARCH" --record "$RAND_RECORD" --batch "$rand_batch" \
         --count "$per_rec" >/dev/null || exit 1
  "$BIN" scan --dir "$ARCH" >/dev/null || exit 1
  fio --name=prep --rw=write --bs=1m --size="${RAND_GIB}g" --filename="$FIOFILE" \
      --end_fsync=1 --ioengine=psync >/dev/null 2>&1 || exit 1
  log ""

  ceiling_line "$RAND_RECORD B random" randread "$rand_bs" --filename="$FIOFILE" \
               --number_ios=$(( RAND_OPS / 8 )) --invalidate=1
  header_od "$RAND_RECORD B random read   cold, $RAND_OPS operations spread over the workers"
  for w in $WORKERS; do
    per=$(( RAND_OPS / w )); [ $per -lt 64 ] && per=64
    : > "$WORK/.f"; : > "$WORK/.r"; : > "$WORK/.fd"; : > "$WORK/.rd"
    for i in $(seq "$REPS"); do
      order="fio raf fiod rafd"
      [ $(( i % 2 )) -eq 0 ] && order="rafd fiod raf fio"
      for side in $order; do
        case $side in
          fio)  cold "$FIOFILE"
                fiorun randread "$rand_bs" "$w" --filename="$FIOFILE" --number_ios="$per" \
                       --invalidate=0 >> "$WORK/.f" ;;
          fiod) cold "$FIOFILE"
                fiorun randread "$rand_bs" "$w" --filename="$FIOFILE" --number_ios="$per" \
                       --invalidate=0 --direct=1 >> "$WORK/.fd" ;;
          raf)  cold "$ARCH"
                { "$BIN" read --dir "$ARCH" --record "$RAND_RECORD" --threads "$w" \
                         --count "$per" --device "$DEV" --buffered \
                    || echo "- - - -"; } >> "$WORK/.r" ;;
          rafd) cold "$ARCH"
                { "$BIN" read --dir "$ARCH" --record "$RAND_RECORD" --threads "$w" \
                         --count "$per" --device "$DEV" \
                    || echo "- - - -"; } >> "$WORK/.rd" ;;
        esac
      done
    done
    row_od "$w" "$(med < "$WORK/.f")" "$(med < "$WORK/.r")" \
           "$(med < "$WORK/.fd")" "$(med < "$WORK/.rd")"
  done
  rm -rf "$ARCH" "$ARCH.index" "$FIOFILE"
  log ""
fi

log "written to $RESULTS"
