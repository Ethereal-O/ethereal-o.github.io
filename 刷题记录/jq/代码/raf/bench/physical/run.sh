#!/bin/bash
# Run the raf measurements on a physical machine and write one results file.
#
#   ./run.sh --mount /mnt/raf-bench --out ./results            everything
#   ./run.sh --mount /mnt/raf-bench --only write,slc           a subset
#   ./run.sh --mount /mnt/raf-bench --quick                    smaller, faster
#
# Sections, and the question each answers:
#
#   write   durable append throughput and latency, posix vs io_uring.
#           Settles whether io_uring's iou-wrk handoff matters on real
#           storage. On a QEMU SCSI disk the ratio was 1.00 because 1.4 ms
#           of device latency hid a 7.5 us cost; at 15 us it was 0.90.
#   read    cold random reads, and the bytes the device actually moved.
#           The open question: a 4 KiB record is 4128 bytes with its header,
#           so it spans two pages. On the VM that was exactly 2.00x, and
#           the device was saturated either way. Whether paying 2x matters
#           on a drive with real bandwidth is what this measures.
#   slc     sustained write past the drive's pseudo-SLC cache. A short run
#           measures the cache, not the drive.
#   fio     the same shapes through fio, as a reference floor.
#
# Needs: cmake + a compiler, and the repo this script ships in. liburing and
# fio are optional; their sections are skipped with a note if missing.
set -u

MOUNT="" ; OUT="$(pwd)/results" ; ONLY="" ; QUICK=0 ; REPS=${REPS:-3} ; SET_GIB_OVERRIDE=""
while [ $# -gt 0 ]; do
  case "$1" in
    --mount) MOUNT="$2"; shift 2 ;;
    --out)   OUT="$2"; shift 2 ;;
    --only)  ONLY="$2"; shift 2 ;;
    --quick) QUICK=1; shift ;;
    --reps)  REPS="$2"; shift 2 ;;
    --set-gib) SET_GIB_OVERRIDE="$2"; shift 2 ;;
    *) echo "usage: $0 --mount DIR [--out DIR] [--only a,b] [--quick] [--reps N] [--set-gib N]" >&2; exit 2 ;;
  esac
done
[ -n "$MOUNT" ] && [ -d "$MOUNT" ] || { echo "error: --mount DIR is required and must exist" >&2; exit 2; }

REPO=$(cd "$(dirname "$0")/../.." && pwd)
# A liburing built into a local prefix is found if PKG_CONFIG_PATH points at
# it, e.g. PKG_CONFIG_PATH=/opt/liburing/lib/pkgconfig ./run.sh ...
mkdir -p "$OUT" 2>/dev/null || true
RESULTS="$OUT/results.txt"
WORK="$MOUNT/raf-bench"
if ! : 2>/dev/null > "$RESULTS"; then
  echo "error: cannot write $RESULTS" >&2
  if [ -d "$OUT" ]; then
    echo "  $OUT is owned by $(stat -c %U "$OUT" 2>/dev/null); you are $(id -un)." >&2
    echo "  An older setup.sh run under sudo left it root-owned. Fix with:" >&2
    echo "    sudo chown -R $(id -un) $OUT" >&2
  fi
  exit 1
fi
if ! mkdir -p "$MOUNT/raf-bench-probe" 2>/dev/null; then
  echo "error: cannot create directories under $MOUNT -- check ownership" >&2
  exit 1
fi
rmdir "$MOUNT/raf-bench-probe"

part=$(df --output=source "$MOUNT" | tail -1)
DEV=$(lsblk -no PKNAME "$part" 2>/dev/null | head -1)
[ -n "$DEV" ] || DEV=$(basename "$part" | sed 's/p\?[0-9]*$//')
MEM_GIB=$(awk '/MemTotal/{printf "%.0f", $2/1048576}' /proc/meminfo)

want() { [ -z "$ONLY" ] && return 0; case ",$ONLY," in *,$1,*) return 0 ;; *) return 1 ;; esac; }
log()  { printf '%s\n' "$*" | tee -a "$RESULTS"; }
med()  { printf '%s\n' "$@" | sort -n | sed -n "$(( (REPS+1)/2 ))p"; }
drop() { sync; [ "$(id -u)" = 0 ] && echo 3 > /proc/sys/vm/drop_caches || sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'; }

# ------------------------------------------------------------------- building
BUILD="$REPO/build-bench"
URING=0
if pkg-config --exists liburing 2>/dev/null; then URING=1; fi
cmake -S "$REPO" -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRAF_BUILD_TESTS=OFF \
      -DRAF_URING=$([ $URING = 1 ] && echo ON || echo OFF) >/dev/null || {
  echo "error: cmake failed; see $BUILD" >&2; exit 1; }
cmake --build "$BUILD" -j"$(nproc)" --target raf_matrix >/dev/null || {
  echo "error: build failed" >&2; exit 1; }
BIN="$BUILD/raf_matrix"

log "# raf physical benchmark   $(date -Is)"
log "# device /dev/$DEV   mount $MOUNT   RAM ${MEM_GIB} GiB   reps $REPS"
log "# io_uring $([ $URING = 1 ] && echo 'built' || echo 'NOT BUILT (liburing missing)')"
log ""

# field 1 MiB/s, 3 p50 us, 4 p99 us, 7 amplification
field() { cut -d' ' -f"$2" <<<"$1"; }

# ---------------------------------------------------------------------- write
if want write; then
  log "## durable writes   MiB/s and append latency, median of $REPS"
  log ""
  printf '%-20s %9s %9s %9s | %9s %9s %9s | %6s\n' \
    workload "psync" "p50 us" "p99 us" "uring" "p50 us" "p99 us" "u/p" | tee -a "$RESULTS"

  if [ $QUICK = 1 ]; then
    ROWS=("4k w=1 b=1:4096:1:1:20000" "4k w=8 b=1:4096:8:1:10000" "1m w=4 b=1:1048576:4:1:600")
  else
    ROWS=(
      "4k w=1 b=1:4096:1:1:50000"      "4k w=4 b=1:4096:4:1:30000"
      "4k w=16 b=1:4096:16:1:15000"    "4k w=64 b=1:4096:64:1:5000"
      "4k w=1 b=32:4096:1:32:200000"   "4k w=16 b=32:4096:16:32:50000"
      "1m w=1 b=1:1048576:1:1:3000"    "1m w=4 b=1:1048576:4:1:2000"
      "1m w=16 b=1:1048576:16:1:800"
    )
  fi
  for spec in "${ROWS[@]}"; do
    IFS=: read -r lbl rec th ba cnt <<<"$spec"
    declare -A R
    for be in posix uring; do
      [ "$be" = uring ] && [ $URING = 0 ] && { R[$be]="- - - - - - -"; continue; }
      v=()
      for _ in $(seq "$REPS"); do
        rm -rf "$WORK"; mkdir -p "$WORK"
        v+=("$(RAF_BACKEND=$be "$BIN" write --dir "$WORK" --record "$rec" \
               --threads "$th" --batch "$ba" --count "$cnt")")
      done
      # Median by throughput, carrying that run's latencies with it.
      R[$be]=$(printf '%s\n' "${v[@]}" | sort -n -k1 | sed -n "$(( (REPS+1)/2 ))p")
    done
    rm -rf "$WORK"
    p=${R[posix]}; u=${R[uring]}
    ratio=$(awk -v a="$(field "$u" 1)" -v b="$(field "$p" 1)" \
            'BEGIN{printf (b>0 && a ~ /^[0-9.]+$/) ? "%.2f" : "%s", (b>0 && a ~ /^[0-9.]+$/) ? a/b : "-"}')
    printf '%-20s %9s %9s %9s | %9s %9s %9s | %6s\n' "$lbl" \
      "$(field "$p" 1)" "$(field "$p" 3)" "$(field "$p" 4)" \
      "$(field "$u" 1)" "$(field "$u" 3)" "$(field "$u" 4)" "$ratio" | tee -a "$RESULTS"
  done
  log ""
fi

# ----------------------------------------------------------------------- read
if want read; then
  # Larger than RAM, so a dropped cache is really cold.
  # Must exceed RAM or a dropped cache refills from the page cache on the
  # first pass and the run measures memory, not the drive. On a machine with
  # a lot of RAM that gets expensive -- --set-gib overrides it, but anything
  # at or below MEM_GIB makes the cold numbers meaningless.
  SET_GIB=${SET_GIB_OVERRIDE:-$([ $QUICK = 1 ] && echo $(( MEM_GIB + 4 )) || echo $(( MEM_GIB * 2 )))}
  if [ "$SET_GIB" -le "$MEM_GIB" ]; then
    log "## WARNING: working set ${SET_GIB} GiB <= RAM ${MEM_GIB} GiB -- reads will hit"
    log "##          the page cache and 'amp' will read below 1.0. Not a cold result."
  fi
  log "## cold random reads   working set ${SET_GIB} GiB vs ${MEM_GIB} GiB RAM"
  log "## 'amp' = bytes the device moved / bytes returned to the caller"
  log ""
  printf '%-18s %8s %10s %9s %9s %7s\n' record threads "MiB/s" "p50 us" "p99 us" amp | tee -a "$RESULTS"

  for rec in 4096 65536 1048576; do
    rm -rf "$WORK"; mkdir -p "$WORK"
    total=$(( SET_GIB * 1073741824 / rec ))
    per=$(( total / 4 )); [ "$per" -lt 1 ] && per=1
    "$BIN" write --dir "$WORK" --record "$rec" --threads 4 --batch 64 --count "$per" >/dev/null
    "$BIN" scan  --dir "$WORK" --record "$rec" >/dev/null || { log "  (scan failed for $rec)"; continue; }
    reads=$([ $QUICK = 1 ] && echo 5000 || echo 20000)
    for th in 1 8 32 128; do
      v=()
      for _ in $(seq "$REPS"); do
        drop
        v+=("$("$BIN" read --dir "$WORK" --record "$rec" --threads "$th" \
               --count "$reads" --device "$DEV")")
      done
      r=$(printf '%s\n' "${v[@]}" | sort -n -k1 | sed -n "$(( (REPS+1)/2 ))p")
      printf '%-18s %8s %10s %9s %9s %7s\n' "$rec" "$th" \
        "$(field "$r" 1)" "$(field "$r" 3)" "$(field "$r" 4)" "$(field "$r" 7)" | tee -a "$RESULTS"
    done
    rm -rf "$WORK" "$WORK.index"
  done
  log ""
fi

# ------------------------------------------------------------------------ slc
if want slc; then
  # Consumer drives absorb the first tens of GB into a pseudo-SLC cache at
  # several GB/s, then fall to native TLC speed. One number hides that; a
  # curve shows where the cliff is, and whether raf is still the bottleneck
  # after it.
  CHUNK_GIB=${CHUNK_GIB:-8}
  TOTAL_GIB=$([ $QUICK = 1 ] && echo 32 || echo 128)
  log "## sustained write   ${CHUNK_GIB} GiB per step to ${TOTAL_GIB} GiB, 1 MiB records, 4 writers"
  log "## a drop partway through is the SLC cache running out"
  log ""
  printf '%-12s %10s %10s %10s\n' "written GiB" "MiB/s" "p50 us" "p99 us" | tee -a "$RESULTS"
  rm -rf "$WORK"; mkdir -p "$WORK"
  per=$(( CHUNK_GIB * 1024 / 4 ))
  done_gib=0
  while [ "$done_gib" -lt "$TOTAL_GIB" ]; do
    r=$("$BIN" write --dir "$WORK" --record 1048576 --threads 4 --batch 1 --count "$per")
    done_gib=$(( done_gib + CHUNK_GIB ))
    printf '%-12s %10s %10s %10s\n' "$done_gib" \
      "$(field "$r" 1)" "$(field "$r" 3)" "$(field "$r" 4)" | tee -a "$RESULTS"
    avail=$(df --output=avail -BG "$MOUNT" | tail -1 | tr -dc 0-9)
    [ "${avail:-0}" -lt $(( CHUNK_GIB * 2 )) ] && { log "  (stopping: only ${avail}G free)"; break; }
  done
  rm -rf "$WORK"
  log ""
fi

# ------------------------------------------------------------------------ fio
if want fio; then
  if ! command -v fio >/dev/null; then
    log "## fio reference   SKIPPED (fio not installed)"; log ""
  else
    log "## fio reference   same shapes, no raf in the path"
    log ""
    printf '%-26s %10s %10s\n' workload "MiB/s" "p99 us" | tee -a "$RESULTS"
    mkdir -p "$WORK"
    fiorun() { # label rw bs jobs extra...
      local lbl=$1 rw=$2 bs=$3 jobs=$4; shift 4
      local o
      o=$(fio --name=r --directory="$WORK" --rw="$rw" --bs="$bs" --numjobs="$jobs" \
              --size=1G --runtime=20 --time_based --group_reporting --ioengine=psync \
              --output-format=json "$@" 2>/dev/null) || { log "  ($lbl failed)"; return; }
      printf '%-26s %10.1f %10.1f\n' "$lbl" \
        "$(jq -r 'if .jobs[0].write.bw>0 then .jobs[0].write.bw else .jobs[0].read.bw end/1024' <<<"$o")" \
        "$(jq -r 'if .jobs[0].write.bw>0 then .jobs[0].write.clat_ns.percentile."99.000000" else .jobs[0].read.clat_ns.percentile."99.000000" end/1000' <<<"$o")" \
        | tee -a "$RESULTS"
    }
    if command -v jq >/dev/null; then
      fiorun "4k durable write j=1"  write     4k 1   --fdatasync=1
      fiorun "4k durable write j=16" write     4k 16  --fdatasync=1
      fiorun "1m durable write j=4"  write     1m 4   --fdatasync=1
      fiorun "4k random read cold"   randread  4k 16  --invalidate=1
      fiorun "4k128 random read"     randread  4128 16 --invalidate=1
    else
      log "  (jq not installed; skipping fio parsing)"
    fi
    rm -rf "$WORK"
    log ""
  fi
fi

log "done. send $RESULTS and $OUT/env.txt"
