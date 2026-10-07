#!/bin/bash
# What can this drive actually do, and how much of it can a synchronous
# buffered reader reach?
#
#   ./ceiling.sh --mount /mnt/raf-bench
#
# seqscale.sh put thirty-two raf streams at 757 MiB/s and thirty-two fio jobs
# at 883. raf tracking fio is the answer to one question and leaves another
# open: 883 MiB/s is far under what an NVMe of this class advertises, so the
# sweep may never have saturated anything. Without the ceiling, "close to fio
# under load" cannot be told from "close to fio, neither of them loaded".
#
# The rows vary two things independently, because the first run confounded
# them: O_DIRECT at depth 64 read 1900 MiB/s and buffered psync across
# thirty-two threads read 759, which could be the page cache's fault or the
# queue depth's, and raf's answer is different in each case.
#
#   buffered-uring-qd64  buffered, deep queue. This is the row that decides
#                        it. If it reaches O_DIRECT's number, then what costs
#                        raf 2.5x is having one request in flight per thread,
#                        and a batched read API fixes it without giving up
#                        the page cache. If it stays near 759, the page cache
#                        itself is the limit and only O_DIRECT crosses it.
#   odirect-uring-qd64   the drive with nothing in the way: the ceiling.
#   odirect-psync-j32    O_DIRECT without a deep queue.
#   buffered-psync-j32   what raf does today.
#   the -j8 and -bs4m rows                 whether thread count or request
#                        size moves the buffered number at all.
#
# The write rows ask the same question on the other side, plus one that only
# applies to writing: what does durability cost? raf's contract is that a
# returning append is on the drive, so the row that bounds it is not the
# drive's streaming ceiling but the durable one.
#
#   dsync-*     O_DSYNC writes submitted through io_uring at a queue depth,
#               from one thread. Every completion is durable, so this is the
#               durable ceiling with the thread count taken out of it -- the
#               number raf's one-durable-write-per-thread design is really
#               being compared against. A sweep to a thousand writers never
#               flattened; depth costs nothing per unit, so this is where the
#               flattening should be if there is any. The -buf rows drop
#               O_DIRECT, because raf's writes are buffered and the rest of
#               these are not.
#   stream-*    write, flush once at the end. The drive's write ceiling.
#   durable-*   a buffered write followed by fdatasync.
#   odsync-*    the same bytes through O_DSYNC, which is what raf opens its
#               segments with: one call that carries the durability rather
#               than two. The pair exists because every raf-against-fio write
#               comparison in this repo gave fio fdatasync and raf O_DSYNC,
#               and nothing here said whether that mattered.
#
# The gap from either to the streaming row is what the contract costs, and no
# design that keeps the contract can close it.
#
# Writing wears flash, so the write rows move --write-gib (8 by default)
# rather than the read file's 1.5x RAM. Needs fio and python3, no root.
set -u

MOUNT=""; OUT="$(pwd)/results"; SIZE_GIB=""; SECS=20; WHAT="read,rand,write,dsync"; WRITE_GIB=8
while [ $# -gt 0 ]; do
  case "$1" in
    --mount) MOUNT="$2"; shift 2 ;;
    --out)   OUT="$2"; shift 2 ;;
    --gib)   SIZE_GIB="$2"; shift 2 ;;
    --secs)  SECS="$2"; shift 2 ;;   # kept for compatibility; unused
    --what)  WHAT="$2"; shift 2 ;;
    --write-gib) WRITE_GIB="$2"; shift 2 ;;
    *) echo "usage: $0 --mount DIR [--out DIR] [--gib N] [--what read,rand,write]" \
            "[--write-gib N]" >&2; exit 2 ;;
  esac
done
[ -n "$MOUNT" ] && [ -d "$MOUNT" ] || { echo "error: --mount DIR is required and must exist" >&2; exit 2; }
command -v fio >/dev/null && command -v python3 >/dev/null || {
  echo "error: needs fio and python3" >&2; exit 2; }

# One pass, over a file larger than RAM, with every job on its own slice.
# All three are there to keep the buffered row off the page cache; the
# O_DIRECT rows never cared.
MEM_GIB=$(awk '/MemTotal/{printf "%.0f", $2/1048576}' /proc/meminfo)
[ -n "$SIZE_GIB" ] || SIZE_GIB=$(( MEM_GIB + MEM_GIB / 2 ))

mkdir -p "$OUT" 2>/dev/null || true
RESULTS="$OUT/ceiling.txt"
: 2>/dev/null > "$RESULTS" || { echo "error: cannot write $RESULTS" >&2; exit 1; }
WORK="$MOUNT/raf-ceiling"; rm -rf "$WORK"; mkdir -p "$WORK" || exit 1
trap 'rm -rf "$WORK"' EXIT

# One parser for both halves of the script. A file rather than a heredoc
# inside each function: two levels of heredoc in one shell script is a way to
# lose an afternoon.
HELPER="$WORK/parse.py"
cat > "$HELPER" <<'PARSER'
import json, sys

job = json.load(open(sys.argv[1]))["jobs"][0]
side = job[sys.argv[2]]
lat = side.get("clat_ns", {}).get("percentile", {}).get("50.000000", 0) / 1000.0
# A durable write is not finished until the flush returns, and fio reports
# that separately.
lat += job.get("sync", {}).get("lat_ns", {}).get("percentile", {}).get("50.000000", 0) / 1000.0
print("%.0f %.1f" % (side["bw_bytes"] / 1048576.0, lat))
PARSER

log() { printf '%s\n' "$*" | tee -a "$RESULTS"; }

log "# device read ceiling   $(date -Is)"
log "# mount $MOUNT   file ${SIZE_GIB} GiB   RAM ${MEM_GIB} GiB   one pass per row   $(fio --version)"
[ "$SIZE_GIB" -le "$MEM_GIB" ] && log "# WARNING: file <= RAM; the buffered row is memory, not the drive"
log ""

want() { case ",$WHAT," in *,$1,*) return 0 ;; *) return 1 ;; esac; }

if want read || want rand; then
  fio --name=prep --rw=write --bs=1m --size="${SIZE_GIB}g" --filename="$WORK/f" \
      --ioengine=psync --end_fsync=1 >/dev/null 2>&1 || {
    echo "error: could not lay down the file" >&2; exit 1; }
fi

one() { # engine jobs iodepth direct bs
  # Each job takes its own slice. Pointed at one file without
  # offset_increment, thirty-two buffered jobs all start at zero, the first
  # one pulls the file into memory and the other thirty-one read it from
  # there -- which is how this row reported 5704 MiB/s against O_DIRECT's
  # 1870 and made the shape look free.
  local slice=$(( SIZE_GIB * 1024 / $2 ))
  fio --name=r --rw=read --bs="$5" --filename="$WORK/f" --ioengine="$1" \
      --numjobs="$2" --iodepth="$3" --direct="$4" --thread --invalidate=1 \
      --size="${slice}m" --offset_increment="${slice}m" \
      --loops=1 --group_reporting=1 \
      --output-format=json --output="$WORK/j" >/dev/null 2>&1 || { echo "- -"; return; }
  python3 "$HELPER" "$WORK/j" read
}

if want read; then
printf '%-24s %10s %10s\n' shape "MiB/s" "p50 us" | tee -a "$RESULTS"
for spec in "buffered-uring-qd64:io_uring:1:64:0:1m" \
            "buffered-uring-qd64-j4:io_uring:4:64:0:1m" \
            "odirect-uring-qd64:io_uring:1:64:1:1m" \
            "odirect-psync-j32:psync:32:1:1:1m" \
            "odirect-psync-j8:psync:8:1:1:1m" \
            "buffered-psync-j32:psync:32:1:0:1m" \
            "buffered-psync-j8:psync:8:1:0:1m" \
            "buffered-psync-j32-bs4m:psync:32:1:0:4m"; do
  IFS=: read -r lbl eng jobs qd dio bs <<<"$spec"
  line=$(one "$eng" "$jobs" "$qd" "$dio" "$bs")
  printf '%-24s %10s %10s\n' "$lbl" "$(cut -d' ' -f1 <<<"$line")" \
         "$(cut -d' ' -f2 <<<"$line")" | tee -a "$RESULTS"
done
log ""
fi

# ----------------------------------------------------------------------- rand
# Sequential reads are bound by bandwidth and random 4 KiB ones by operations,
# so the sequential ceiling says nothing about where a random sweep should
# stop. seqscale put fio at 791 MiB/s and raf at 623 across sixty-four
# workers; without these rows there is no telling whether that is the drive or
# the shape.
if want rand; then
  log "## random 4 KiB read"
  log ""
  printf '%-24s %10s %10s\n' shape "MiB/s" "p50 us" | tee -a "$RESULTS"
  randone() { # engine jobs iodepth direct
    fio --name=r --rw=randread --bs=4k --filename="$WORK/f" --ioengine="$1" \
        --numjobs="$2" --iodepth="$3" --direct="$4" --thread --invalidate=1 \
        --number_ios=$(( 200000 / $2 )) --group_reporting=1 \
        --output-format=json --output="$WORK/j" >/dev/null 2>&1 || { echo "- -"; return; }
    python3 "$HELPER" "$WORK/j" read
  }
  for spec in "odirect-uring-qd64:io_uring:1:64:1" \
              "odirect-uring-qd64-j8:io_uring:8:64:1" \
              "odirect-psync-j64:psync:64:1:1" \
              "buffered-psync-j64:psync:64:1:0"; do
    IFS=: read -r lbl eng jobs qd dio <<<"$spec"
    line=$(randone "$eng" "$jobs" "$qd" "$dio")
    printf '%-24s %10s %10s\n' "$lbl" "$(cut -d' ' -f1 <<<"$line")" \
           "$(cut -d' ' -f2 <<<"$line")" | tee -a "$RESULTS"
  done
  log ""
fi

# --------------------------------------------------------------------- dsync
# How much durable write can one thread have outstanding?
#
# The sweep in seqscale buys concurrency in threads, because a synchronous
# append holds one. io_uring does not: a thread can have two hundred and
# fifty-six O_DSYNC writes in flight and each completion is a durable record.
# If these rows flatten where the thread sweep did not, the limit was never
# the drive.
if want dsync; then
  log "## durable writes in flight, one thread, io_uring + O_DIRECT + O_DSYNC"
  log ""
  printf '%-22s %10s %10s %12s\n' shape "MiB/s" "p50 us" "ops/s" | tee -a "$RESULTS"
  dsyncone() { # bs depth total_mib jobs direct
    rm -rf "$WORK/d"; mkdir -p "$WORK/d"
    fio --name=d --rw=write --bs="$1" --directory="$WORK/d" --ioengine=io_uring \
        --iodepth="$2" --numjobs="${4:-1}" --direct="${5:-1}" --sync=dsync --thread \
        --size="$3m" --fallocate=none --group_reporting=1 --unlink=1 \
        --output-format=json --output="$WORK/j" >/dev/null 2>&1 || { echo "- - -"; return; }
    rm -rf "$WORK/d"
    python3 - "$WORK/j" <<'OPS'
import json, sys
w = json.load(open(sys.argv[1]))["jobs"][0]["write"]
p = w.get("clat_ns", {}).get("percentile", {}).get("50.000000", 0) / 1000.0
print("%.0f %.1f %.0f" % (w["bw_bytes"] / 1048576.0, p, w["iops"]))
OPS
  }
  # One job means one file, and ext4 serialises appends to an inode however
  # many requests are outstanding -- so the single-job rows measure a file,
  # not the drive. raf gives every writer its own segment, so the rows that
  # match its shape are the multi-job ones: the same number of requests in
  # flight, spread over that many files.
  for spec in "4k:1:256:1" "4k:4:256:1" "4k:16:512:1" "4k:64:512:1" \
              "4k:1:512:64" "4k:4:512:16" "4k:8:512:8" "4k:16:512:4" \
              "4k:4:1024:64" "4k:16:1024:16" \
              "1m:1:2048:1" "1m:16:4096:1" "1m:4:4096:8" "1m:1:4096:32" \
              "4k:1:512:64:0" "4k:4:1024:64:0" "4k:16:1024:16:0" "4k:64:1024:8:0"; do
    IFS=: read -r bs depth mib jobs direct <<<"$spec"
    line=$(dsyncone "$bs" "$depth" "$mib" "$jobs" "${direct:-1}")
    lbl="dsync-$bs-j$jobs-qd$depth"
    [ "${direct:-1}" = 0 ] && lbl="$lbl-buf"
    printf '%-22s %10s %10s %12s\n' "$lbl" "$(cut -d' ' -f1 <<<"$line")" \
           "$(cut -d' ' -f2 <<<"$line")" "$(cut -d' ' -f3 <<<"$line")" | tee -a "$RESULTS"
  done
  log ""
fi

# ---------------------------------------------------------------------- write
if want write; then
  rm -f "$WORK/f"
  log "## write, ${WRITE_GIB} GiB per row"
  log ""
  printf '%-28s %10s %10s\n' shape "MiB/s" "p50 us" | tee -a "$RESULTS"
  writeone() { # engine jobs iodepth direct bs sync total_mib
    local slice=$(( $7 / $2 ))
    local sync_args="--end_fsync=1"
    [ "$6" = fdatasync ] && sync_args="--fdatasync=1"
    # O_DSYNC, which is what raf opens its segments with: the write call
    # carries the durability rather than a second call forcing it.
    [ "$6" = dsync ] && sync_args="--sync=dsync"
    rm -rf "$WORK/w"; mkdir -p "$WORK/w"
    fio --name=w --rw=write --bs="$5" --directory="$WORK/w" --ioengine="$1" \
        --numjobs="$2" --iodepth="$3" --direct="$4" --thread --size="${slice}m" \
        --fallocate=none $sync_args --group_reporting=1 --unlink=1 \
        --output-format=json --output="$WORK/j" >/dev/null 2>&1 || { echo "- -"; return; }
    rm -rf "$WORK/w"
    python3 "$HELPER" "$WORK/j" write
  }
  # The 4 KiB rows are bound by operations, not bytes, so they move a
  # sixteenth of the budget. At the full one they would be a flush per 4 KiB
  # across eight gigabytes -- two million of them, hours, and no more
  # informative for it.
  small=$(( WRITE_GIB * 1024 / 16 ))
  big=$(( WRITE_GIB * 1024 ))
  for spec in "stream-odirect-uring-qd64:io_uring:1:64:1:1m:end:$big" \
              "stream-odirect-psync-j8:psync:8:1:1:1m:end:$big" \
              "stream-buffered-psync-j8:psync:8:1:0:1m:end:$big" \
              "durable-buffered-psync-j8-1m:psync:8:1:0:1m:fdatasync:$big" \
              "durable-odirect-psync-j8-1m:psync:8:1:1:1m:fdatasync:$big" \
              "durable-buffered-psync-j32-4k:psync:32:1:0:4k:fdatasync:$small" \
              "durable-buffered-psync-j8-4k:psync:8:1:0:4k:fdatasync:$small" \
              "odsync-buffered-psync-j8-1m:psync:8:1:0:1m:dsync:$big" \
              "odsync-buffered-psync-j32-4k:psync:32:1:0:4k:dsync:$small" \
              "odsync-buffered-psync-j8-4k:psync:8:1:0:4k:dsync:$small"; do
    IFS=: read -r lbl eng jobs qd dio bs syn mib <<<"$spec"
    line=$(writeone "$eng" "$jobs" "$qd" "$dio" "$bs" "$syn" "$mib")
    printf '%-28s %10s %10s\n' "$lbl" "$(cut -d' ' -f1 <<<"$line")" \
           "$(cut -d' ' -f2 <<<"$line")" | tee -a "$RESULTS"
  done
  log ""
fi

log "written to $RESULTS"
