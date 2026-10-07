#!/bin/bash
# Why does raf's durable append stop scaling at sixteen writers?
#
#   ./appendscale.sh --mount /mnt/raf-bench
#   ./appendscale.sh --mount /mnt/raf-bench --workers "16" --reps 4
#
# vsfio.sh established the shape and ruled out two explanations:
#
#   workers   fio MiB/s   raf MiB/s   raf p50   fio p50
#   1               5.3         5.9    639 us    719 us
#   16             15.3         5.4  11266 us   1362 us
#   256           120.5        57.4  16205 us  11473 us
#
# raf gains nothing between one writer and sixteen while fio nearly triples,
# and raf's latency rises almost exactly 16x -- the signature of work that
# serialises. It is not measurement order (alternating the two sides changed
# nothing) and it is not page alignment (a 4064 byte payload, which makes the
# record exactly one page, moved 0.35 to 0.54 and left a 7x latency gap).
#
# So this script takes the append path apart instead of guessing at it again.
# Each variant removes one thing, and the one that restores scaling is the
# answer:
#
#   fio        the floor: the same bytes, no raf, and --fallocate=none so
#              that fio extends its file exactly as raf extends a segment
#   fio+falloc fio's own default, which preallocates with fallocate(2). It is
#              in the table because every fio write number in this repo before
#              now was this one, compared against a raf that was extending --
#              two different workloads wearing one label.
#   raf        as shipped
#   +prealloc  zeros kept ahead of the append point, so an append overwrites
#              instead of extending the file. Removes ext4's metadata commit
#              per append -- the journal table reads 0.002 transactions per
#              flush against 0.500 -- and pays twice the bytes for it. The
#              runway is sized to the run, so it is written once.
#   -frontier  RAF_PUBLISH_FRONTIER=0. Removes the second write per append,
#              the buffered one into the segment header.
#   -f+p       both, to catch the two interacting
#   batchN     one flush per N records, for any N. Sweeping it separates the
#              two things batching does: fewer journal commits per record,
#              and no partial page left for the next append to rewrite. If
#              the amplification falls like 1/N it is the journal; if it
#              drops to about 1.0 at small N and stays, it is the page. Its
#              p50 is per appendBatch call, so divide by N to compare it with
#              the other columns.
#
# Compare within a table, not across scripts: the fio column here runs a
# shorter job than vsfio.sh's, so its setup costs weigh more.
#
# A variant that lands near fio at sixteen writers names the cost. If none
# does, the cost is somewhere this script does not look.
#
# Needs: fio, python3, cmake. No root.
set -u

MOUNT=""; OUT="$(pwd)/results"; WORKERS="1 4 16 64"; MIB=8; REPS=2; RECORD=4096; PROBE=0
VARIANTS="fio,fio+falloc,raf,+prealloc,-frontier,-f+p,batch32"
while [ $# -gt 0 ]; do
  case "$1" in
    --mount)    MOUNT="$2"; shift 2 ;;
    --out)      OUT="$2"; shift 2 ;;
    --workers)  WORKERS="$2"; shift 2 ;;
    --mib)      MIB="$2"; shift 2 ;;
    --reps)     REPS="$2"; shift 2 ;;
    --record)   RECORD="$2"; shift 2 ;;
    --variants) VARIANTS="$2"; shift 2 ;;
    --probe)    PROBE="$2"; shift 2 ;;
    *) echo "usage: $0 --mount DIR [--out DIR] [--workers \"1 4 16 64\"] [--mib N]" \
            "[--reps N] [--record BYTES] [--variants a,b,c] [--probe WORKERS]" >&2; exit 2 ;;
  esac
done
[ -n "$MOUNT" ] && [ -d "$MOUNT" ] || { echo "error: --mount DIR is required and must exist" >&2; exit 2; }
command -v fio >/dev/null     || { echo "error: fio is not installed" >&2; exit 2; }
command -v python3 >/dev/null || { echo "error: python3 is needed to parse fio's json" >&2; exit 2; }

REPO=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$OUT" 2>/dev/null || true
RESULTS="$OUT/appendscale.txt"
if ! : 2>/dev/null > "$RESULTS"; then
  echo "error: cannot write $RESULTS" >&2; exit 1
fi
WORK="$MOUNT/raf-appendscale"
rm -rf "$WORK"; mkdir -p "$WORK" || { echo "error: cannot create $WORK" >&2; exit 1; }
trap 'rm -rf "$WORK"' EXIT

# raf keeps two descriptors per segment -- the O_DSYNC one it appends
# through and an unsynced one for the frontier -- so W writers want 2W, and
# the default soft limit of 1024 bites at 512. Raising it to the hard limit
# needs no privileges and is the difference between measuring raf and
# measuring RLIMIT_NOFILE.
ulimit -n "$(ulimit -Hn)" 2>/dev/null || true

log() { printf '%s\n' "$*" | tee -a "$RESULTS"; }

# Sectors this device has written, /proc/diskstats field 10. Bracketing a run
# with it gives device bytes over payload bytes -- the number that says
# whether "raf writes one extra 512-byte block per record" is true. It is
# machine wide, so a busy filesystem inflates it for every variant alike.
sectors_written() {
  awk -v d="$DEV" '$3 == d { print $10; exit }' /proc/diskstats
}
IFS=, read -r -a VARS <<<"$VARIANTS"
PER=$(( MIB * 1048576 / RECORD ))
# fio gets the operation count rather than a byte budget, so both sides do
# the same number of durable writes whatever --record is. Its block size is
# the payload rounded up to a page -- raf's extra 32 bytes of header are the
# handicap being measured, not something to hand to fio as well.
FIO_BS=$(( ((RECORD + 4095) / 4096) * 4096 ))
# The runway is the run, not a fixed 64 MiB.
#
# A fixed runway next to a small --mib is a trap: at 8 MiB per worker the old
# default wrote 64 MiB of zeros to append 8 MiB of payload, so the variant
# measured the runway and nothing else. Sized to the run, preallocation
# writes the bytes once and never refills, which is the fair version of what
# it costs: twice the bytes, no journal transactions.
PREALLOC=$(( PER * RECORD ))

# ------------------------------------------------------------------- building
BUILD="$REPO/build-bench"
cmake -S "$REPO" -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRAF_BUILD_TESTS=OFF \
      >/dev/null || { echo "error: cmake failed; see $BUILD" >&2; exit 1; }
cmake --build "$BUILD" -j"$(nproc)" --target raf_matrix >/dev/null || {
  echo "error: build failed" >&2; exit 1; }
BIN="$BUILD/raf_matrix"

part=$(df --output=source "$MOUNT" | tail -1)
DEV=$(lsblk -no PKNAME "$part" 2>/dev/null | head -1)
[ -n "$DEV" ] || DEV=$(basename "$part" | sed 's/p\?[0-9]*$//')

log "# raf append, taken apart   $(date -Is)"
log "# device /dev/$DEV   mount $MOUNT   record $RECORD B   $PER appends per worker"
log "# fio bs $FIO_BS   reps $REPS (best of)   workers: $WORKERS"

# ext4's journal transaction counter, which the probe says is where the time
# goes. It is machine wide, so anything else writing to this filesystem adds
# to it; the number worth reading is raf's against fio's over runs that do the
# same number of appends.
JBD2=$(ls -d /proc/fs/jbd2/"$(basename "$part")"-* 2>/dev/null | head -1)
[ -n "$JBD2" ] && log "# journal $JBD2" || log "# journal counters unavailable"
log ""

txns() {
  [ -n "$JBD2" ] && sed -n 's/^\([0-9]*\) transactions.*/\1/p' "$JBD2/info" 2>/dev/null \
                 || echo 0
}

# One run of one variant. Prints: mib_s p50_us.
run() { # variant workers
  local v="$1" w="$2"
  case "$v" in
    fio|fio+falloc)
      rm -rf "$WORK/f"; mkdir -p "$WORK/f"
      local falloc=none; [ "$v" = "fio+falloc" ] && falloc=native
      fio --name=w --rw=write --bs="$FIO_BS" --numjobs="$w" --ioengine=psync \
          --directory="$WORK/f" --size=$(( PER * FIO_BS )) --fdatasync=1 \
          --fallocate="$falloc" \
          --group_reporting=1 --thread --unlink=1 --output-format=json \
          --output="$WORK/fio.json" >/dev/null 2>&1 || { echo "- -"; return; }
      rm -rf "$WORK/f"
      python3 - "$WORK/fio.json" <<'PY'
import json, sys
job = json.load(open(sys.argv[1]))["jobs"][0]
s = job["write"]
p = s.get("clat_ns", {}).get("percentile", {}).get("50.000000", 0) / 1000.0
sync = job.get("sync", {}).get("lat_ns", {}).get("percentile", {})
p += sync.get("50.000000", 0) / 1000.0
print("%.1f %.1f" % (s["bw_bytes"] / 1048576.0, p))
PY
      ;;
    *)
      local env_frontier=1 prealloc=0 batch=1
      case "$v" in
        raf)        ;;
        +prealloc)  prealloc=$PREALLOC ;;
        -frontier)  env_frontier=0 ;;
        -f+p)       env_frontier=0; prealloc=$PREALLOC ;;
        # batchN for any N, so the batch size can be swept rather than
        # sampled at one value.
        batch[0-9]*) batch=${v#batch} ;;
        *) echo "- -"; return ;;
      esac
      rm -rf "$WORK/a"
      local line
      line=$(RAF_PUBLISH_FRONTIER=$env_frontier "$BIN" write --dir "$WORK/a" \
             --record "$RECORD" --threads "$w" --count "$PER" --batch "$batch" \
             --prealloc "$prealloc" 2>/dev/null) || line=""
      rm -rf "$WORK/a"
      [ -n "$line" ] && echo "$(cut -d' ' -f1 <<<"$line") $(cut -d' ' -f3 <<<"$line")" \
                     || echo "- -"
      ;;
  esac
}

# Best of REPS lines by throughput, not the median.
#
# With two repetitions the median was the lower of the two, so one unlucky
# run set the column. The machine this runs on is not idle -- kubelet and a
# VM image share the partition and the journal -- and interference can only
# make a run slower, never faster. Taking the best removes it symmetrically;
# both sides get the same treatment, and position is already balanced by the
# forward/reverse ordering.
med() { sort -n -k1 | tail -1; }

# Rotate the running order every repetition. This drive's durable-write cost
# steps up with cumulative bytes, so a variant pinned to last place loses on
# position alone.
rotate() { # rep
  local n=${#VARS[@]} i off=$(( (($1 - 1) / 2) % ${#VARS[@]} ))
  if [ $(( $1 % 2 )) -eq 1 ]; then
    for (( i = 0; i < n; i++ )); do printf '%s\n' "${VARS[$(( (i + off) % n ))]}"; done
  else
    # Reversed, so two repetitions already put every variant once in the first
    # half and once in the second. A plain rotation needs as many repetitions
    # as there are variants before the positions balance, and with six
    # variants and two reps it left -frontier last twice: at four writers it
    # read 3.1 MiB/s against raf's 11.9 while being identical to raf at one,
    # sixteen and sixty-four.
    for (( i = n - 1; i >= 0; i-- )); do printf '%s\n' "${VARS[$(( (i + off) % n ))]}"; done
  fi
}

# Where are the threads actually sleeping?
#
# Six variants have now failed to name the cost, so stop substituting pieces
# and look. /proc/<pid>/task/*/wchan is the kernel function each thread is
# blocked in, it needs no privileges, and a histogram of it over a run says
# whether raf is waiting on the journal, on writeback, on a lock, or on the
# device. jbd2_log_wait_commit at the top means ext4's journal; io_schedule
# means the drive.
if [ "$PROBE" != 0 ]; then
  log "## where sixteen writers sleep   wchan histogram, $PROBE workers"
  log ""
  rm -rf "$WORK/p"
  "$BIN" write --dir "$WORK/p" --record "$RECORD" --threads "$PROBE" \
         --count "$PER" >/dev/null 2>&1 &
  probe_pid=$!
  : > "$WORK/wchan"
  while kill -0 "$probe_pid" 2>/dev/null; do
    # awk, not cat: these files carry no trailing newline, so cat runs
    # sixteen answers into one line.
    awk 'FNR==1 && $0 != "" && $0 != "0" { print }' \
        /proc/"$probe_pid"/task/*/wchan 2>/dev/null >> "$WORK/wchan"
    sleep 0.02
  done
  wait "$probe_pid" 2>/dev/null
  total=$(wc -l < "$WORK/wchan")
  if [ "$total" -gt 0 ]; then
    sort "$WORK/wchan" | uniq -c | sort -rn | head -12 \
      | awk -v t="$total" '{ printf "%7.2f%%  %s\n", 100*$1/t, $2 }' | tee -a "$RESULTS"
  else
    log "  (no samples: threads were never caught asleep, or wchan is restricted)"
  fi
  rm -rf "$WORK/p"
  log ""
  log "written to $RESULTS"
  exit 0
fi

declare -A MIBS P50 TXN AMP
for w in $WORKERS; do
  for v in "${VARS[@]}"; do
    : > "$WORK/.$w.$(tr -d '+-' <<<"$v")"; : > "$WORK/.t.$w.$(tr -d '+-' <<<"$v")"
    : > "$WORK/.a.$w.$(tr -d '+-' <<<"$v")"
  done
  for rep in $(seq "$REPS"); do
    while read -r v; do
      case "$v" in batch[0-9]*) batch_of_v=${v#batch} ;; *) batch_of_v=1 ;; esac
      t0=$(txns); s0=$(sectors_written)
      run "$v" "$w" >> "$WORK/.$w.$(tr -d '+-' <<<"$v")"
      awk -v s="$(( $(sectors_written) - s0 ))" -v b="$(( PER * w * RECORD ))" \
          'BEGIN{ printf "%.2f\n", (b > 0) ? s * 512 / b : 0 }' \
          >> "$WORK/.a.$w.$(tr -d '+-' <<<"$v")"
      # Transactions this run saw, over the appends it did. One per append
      # means every append is its own commit; well under one means they are
      # being batched, by the kernel or by the caller.
      awk -v a="$(( $(txns) - t0 ))" -v n="$(( PER * w ))" -v b="$batch_of_v" \
          'BEGIN{ printf "%.3f\n", (n > 0) ? a / (n / b) : 0 }' \
          >> "$WORK/.t.$w.$(tr -d '+-' <<<"$v")"
    done < <(rotate "$rep")
  done
  for v in "${VARS[@]}"; do
    line=$(med < "$WORK/.$w.$(tr -d '+-' <<<"$v")")
    MIBS[$w,$v]=$(cut -d' ' -f1 <<<"$line"); P50[$w,$v]=$(cut -d' ' -f2 <<<"$line")
    TXN[$w,$v]=$(sort -n "$WORK/.t.$w.$(tr -d '+-' <<<"$v")" | head -1)
    AMP[$w,$v]=$(sort -n "$WORK/.a.$w.$(tr -d '+-' <<<"$v")" | head -1)
  done
  echo "  done workers=$w" >&2
done

table() { # name array-name
  log "## $1"
  log ""
  { printf '%-8s' workers; for v in "${VARS[@]}"; do printf '%11s' "$v"; done; printf '\n'; } \
    | tee -a "$RESULTS"
  for w in $WORKERS; do
    { printf '%-8s' "$w"
      for v in "${VARS[@]}"; do
        case "$2" in
          mibs) printf '%11s' "${MIBS[$w,$v]:--}" ;;
          p50)  printf '%11s' "${P50[$w,$v]:--}" ;;
          txn)  printf '%11s' "${TXN[$w,$v]:--}" ;;
          *)    printf '%11s' "${AMP[$w,$v]:--}" ;;
        esac
      done
      printf '\n'; } | tee -a "$RESULTS"
  done
  log ""
}

table "throughput, MiB/s" mibs
table "append latency p50, us" p50
table "ext4 journal transactions per flush, machine wide" txn
table "device bytes written per payload byte" amp

log "written to $RESULTS"
