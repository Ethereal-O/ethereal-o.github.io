// Measurement harness for the benchmark scripts in bench/physical.
//
// Three modes, because a cold random-read measurement cannot afford to walk
// the archive first -- the walk would pull every record into the page cache
// and quietly turn a cold run into a warm one:
//
//   write  append records, report throughput and append latency percentiles
//   scan   iterate once and save every index to a side file
//   seqread  iterate with `--threads` readers at once, report throughput
//   odread   the same bytes through O_DIRECT, parsed here rather than by
//            RecordReader. Not a feature -- it answers whether raf's parse
//            and checksum path could keep up if the read path stopped going
//            through the page cache.
//   odrand   random reads through O_DIRECT, against `read`. Sequential said
//            O_DIRECT wins by 2.2x; random is where buffered might still
//            win, because a page cache hit costs nothing and O_DIRECT has
//            no hits to get.
//   read   random-read indexes from that side file, report throughput,
//          latency percentiles, and how many bytes the device actually moved
//
// Deterministic by construction: payload bytes come from the record number,
// threads start together on a std::latch, and the random-read order is a
// fixed coprime stride. Two runs of the same arguments read the same records
// in the same order, so a difference between two runs is a real difference.
#include <sys/resource.h>

#include <raf/raf.h>

#include <fcntl.h>
#include <unistd.h>

#include "store/format.h"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <latch>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxLatencySamples = 2'000'000;
// Coprime with any plausible record count, so the walk visits every record
// before repeating and never degenerates into a sequential scan.
constexpr std::size_t kStride = 7919;

struct Args {
    std::string mode, dir, device;
    std::size_t record = 4096;
    int threads = 1, batch = 1;
    long count = 10000;
    std::uint64_t prealloc = 0;
    int chunks = 1;
    int shards = 0;
    bool buffered = false;
};

[[noreturn]] void usage() {
    std::fprintf(stderr,
                 "usage: raf_matrix MODE --dir D [options]\n"
                 "  MODE      write | scan | read | seqread | odread | odrand\n"
                 "  --dir     archive directory\n"
                 "  --record  payload bytes (default 4096)\n"
                 "  --threads concurrent writers or readers (default 1)\n"
                 "  --batch   records per appendBatch (default 1)\n"
                 "  --count   records per thread (default 10000)\n"
                 "  --device  kernel device name, e.g. nvme0n1, to report the\n"
                 "            bytes it actually moved during a read run\n"
                 "  --prealloc bytes of zeros to keep ahead of the append point\n"
                 "            (Options::preallocateBytes; 0 disables)\n"
                 "  --buffered clear Options::directReads, which is on by default:\n"
                 "            the reader goes through the page cache instead\n"
                 "  --shards  give each reader its own archive: thread t opens\n"
                 "            <dir>.t. Without it every sequential reader walks\n"
                 "            the same bytes and the run measures the page cache\n"
                 "  --chunks  report this many successive slices of one run, all\n"
                 "            through one writer, so a cost that is paid once --\n"
                 "            preallocation above all -- lands inside the numbers\n"
                 "            instead of before them\n");
    std::exit(2);
}

Args parse(int argc, char** argv) {
    if (argc < 2) usage();
    Args a;
    a.mode = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&]() -> std::string {
            if (++i >= argc) usage();
            return argv[i];
        };
        if (k == "--dir") a.dir = next();
        else if (k == "--device") a.device = next();
        else if (k == "--record") a.record = std::stoul(next());
        else if (k == "--threads") a.threads = std::stoi(next());
        else if (k == "--batch") a.batch = std::stoi(next());
        else if (k == "--count") a.count = std::stol(next());
        else if (k == "--prealloc") a.prealloc = std::stoull(next());
        else if (k == "--chunks") a.chunks = std::stoi(next());
        else if (k == "--shards") a.shards = std::stoi(next());
        else if (k == "--buffered") a.buffered = true;
        else usage();
    }
    if (a.dir.empty() || a.threads < 1 || a.count < 1) usage();
    return a;
}

// Payload bytes derive from the record number, so a reader can tell which
// record it got without a lookup table, and two runs produce identical files.
std::string payload(std::size_t size, std::uint64_t seed) {
    std::string s(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        s[i] = static_cast<char>('a' + ((seed * 1103515245ull + i * 12345ull) >> 7) % 26);
    }
    return s;
}

// Sectors written to one device, /proc/diskstats field 10. The write side's
// amplification was never reported, so "raf writes one extra 512-byte block
// per record" stayed a prediction.
std::uint64_t sectorsWritten(const std::string& device) {
    if (device.empty()) return 0;
    std::ifstream in("/proc/diskstats");
    std::string line;
    while (std::getline(in, line)) {
        unsigned major = 0, minor = 0;
        char name[64] = {};
        unsigned long long r = 0, rm = 0, rs = 0, rt = 0, w = 0, wm = 0, ws = 0;
        if (std::sscanf(line.c_str(), "%u %u %63s %llu %llu %llu %llu %llu %llu %llu", &major,
                        &minor, name, &r, &rm, &rs, &rt, &w, &wm, &ws) == 10 &&
            device == name) {
            return ws;
        }
    }
    return 0;
}

// Sectors read from one device, straight out of /proc/diskstats field 3.
// Zero if the device was not named or not found -- the caller reports the
// amplification only when it has both numbers.
std::uint64_t sectorsRead(const std::string& device) {
    if (device.empty()) return 0;
    std::ifstream in("/proc/diskstats");
    std::string line;
    while (std::getline(in, line)) {
        unsigned major = 0, minor = 0;
        char name[64] = {};
        unsigned long long reads = 0, merged = 0, sectors = 0;
        if (std::sscanf(line.c_str(), "%u %u %63s %llu %llu %llu", &major, &minor, name, &reads,
                        &merged, &sectors) == 6 &&
            device == name) {
            return sectors;
        }
    }
    return 0;
}

struct Percentiles {
    double p50 = 0, p99 = 0, p999 = 0, max = 0;
};

// Samples are kept as tenths of a microsecond: a cached readNext takes a
// fraction of one, and a table of zeros says nothing. Printed in microseconds
// either way, so the columns mean what they always did.
Percentiles percentiles(std::vector<std::uint32_t>& us) {
    Percentiles p;
    if (us.empty()) return p;
    std::sort(us.begin(), us.end());
    auto at = [&](double q) {
        const std::size_t i = std::min(us.size() - 1, static_cast<std::size_t>(q * us.size()));
        return static_cast<double>(us[i]) / 10.0;
    };
    p.p50 = at(0.50);
    p.p99 = at(0.99);
    p.p999 = at(0.999);
    p.max = static_cast<double>(us.back()) / 10.0;
    return p;
}

// What the run cost besides time. The design doc asks for resource use
// beside throughput; the fields go on the end of the report line so a script
// that cuts the first seven keeps working.
struct Resources {
    double cpu_seconds;
    double peak_rss_mib;
};

Resources resources() {
    rusage ru{};
    ::getrusage(RUSAGE_SELF, &ru);
    const auto seconds = [](const timeval& t) {
        return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_usec) / 1e6;
    };
    return {seconds(ru.ru_utime) + seconds(ru.ru_stime),
            static_cast<double>(ru.ru_maxrss) / 1024.0};
}

// One line, space separated, so the shell can cut fields out of it without
// parsing anything.
//   mib_s ops_s p50_us p99_us p999_us max_us amp cpu_s peak_rss_mib
void report(double mib_s, double ops_s, const Percentiles& p, double amp) {
    const Resources r = resources();
    std::printf("%.1f %.0f %.1f %.1f %.1f %.1f %.3f %.2f %.1f\n", mib_s, ops_s, p.p50, p.p99,
                p.p999, p.max, amp, r.cpu_seconds, r.peak_rss_mib);
}

std::string indexPath(const std::string& dir) { return dir + ".index"; }

int doWrite(const Args& a) {
    const std::string value = payload(a.record, 7);
    const std::vector<std::string> batch_values(a.batch, value);

    std::latch opened(a.threads + 1);
    // Chunks run through one writer, so a one-off cost -- preallocating the
    // zero runway above all -- is paid inside a chunk rather than before the
    // clock starts. Measuring chunks as separate processes hides exactly the
    // cost the measurement exists to find.
    std::barrier sync_point(a.threads + 1);
    std::vector<std::vector<std::uint32_t>> latency(a.threads);
    std::atomic<long> written{0};
    std::vector<std::thread> pool;
    pool.reserve(a.threads);

    const long sample_every =
        std::max<long>(1, a.count / static_cast<long>(kMaxLatencySamples) + 1);

    for (int t = 0; t < a.threads; ++t) {
        pool.emplace_back([&, t] {
            raf::Options options;
            options.preallocateBytes = a.prealloc;
            raf::RecordWriter writer(a.dir, options);
            auto& mine = latency[t];
            // Every writer holds its segment before any of them starts, so
            // the measurement covers steady state and not segment handout.
            opened.arrive_and_wait();
            for (int c = 0; c < a.chunks; ++c) {
                sync_point.arrive_and_wait();
                long done = 0;
                for (long i = 0; i < a.count; i += a.batch) {
                    const int n = static_cast<int>(std::min<long>(a.batch, a.count - i));
                    const auto begin = Clock::now();
                    if (n == 1) {
                        writer.append(value);
                    } else {
                        writer.appendBatch(
                            std::vector<std::string>(batch_values.begin(), batch_values.begin() + n));
                    }
                    if ((i / std::max(1, a.batch)) % sample_every == 0) {
                        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            Clock::now() - begin)
                                            .count();
                        mine.push_back(static_cast<std::uint32_t>(ns / 100));
                    }
                    done += n;
                }
                written.fetch_add(done, std::memory_order_relaxed);
                sync_point.arrive_and_wait();
            }
            writer.close();
        });
    }

    opened.arrive_and_wait();
    for (int c = 0; c < a.chunks; ++c) {
        written.store(0, std::memory_order_relaxed);
        for (auto& v : latency) v.clear();
        sync_point.arrive_and_wait();
        const std::uint64_t sectors_before = sectorsWritten(a.device);
        const auto start = Clock::now();
        sync_point.arrive_and_wait();
        const double secs = std::chrono::duration<double>(Clock::now() - start).count();
        const std::uint64_t sectors_after = sectorsWritten(a.device);

        std::vector<std::uint32_t> all;
        for (auto& v : latency) all.insert(all.end(), v.begin(), v.end());
        const double records = static_cast<double>(written.load());
        const double payload = records * static_cast<double>(a.record);
        // Device bytes over payload bytes. Writers are still in their final
        // barrier here, so nothing but this chunk's appends is counted.
        const double amp = (sectors_after > sectors_before && payload > 0)
                               ? static_cast<double>(sectors_after - sectors_before) * 512.0 /
                                     payload
                               : 0.0;
        report(payload / (1024.0 * 1024.0) / secs, records / secs, percentiles(all), amp);
    }
    for (auto& th : pool) th.join();
    return 0;
}

int doScan(const Args& a) {
    std::vector<raf::INDEX_TYPE> index;
    raf::RecordReader reader(a.dir);
    while (reader.readNext()) index.push_back(reader.currentIndex());
    if (index.empty()) {
        std::fprintf(stderr, "raf_matrix: %s holds no records\n", a.dir.c_str());
        return 1;
    }
    std::ofstream out(indexPath(a.dir), std::ios::binary);
    out.write(reinterpret_cast<const char*>(index.data()),
              static_cast<std::streamsize>(index.size() * sizeof(raf::INDEX_TYPE)));
    if (!out) {
        std::fprintf(stderr, "raf_matrix: cannot write %s\n", indexPath(a.dir).c_str());
        return 1;
    }
    std::printf("%zu\n", index.size());
    return 0;
}

// Sequential read: `--threads` independent iterators over the same archive,
// each stopping after `--count` records. One reader per thread, because
// iteration state is per reader; they all cover the same bytes, which is what
// makes this comparable to fio's numjobs readers over one file.
int doSeqRead(const Args& a) {
    std::latch opened(a.threads + 1);
    std::vector<std::vector<std::uint32_t>> latency(a.threads);
    std::atomic<long> bytes_read{0};
    std::vector<std::thread> pool;
    pool.reserve(a.threads);
    const long sample_every =
        std::max<long>(1, a.count / static_cast<long>(kMaxLatencySamples) + 1);

    for (int t = 0; t < a.threads; ++t) {
        pool.emplace_back([&, t] {
            // One archive per reader when sharded, so W readers cover W
            // disjoint streams and the device, not the cache, is the limit.
            raf::Options options;
            options.directReads = !a.buffered;
            raf::RecordReader reader(a.shards > 0 ? a.dir + "." + std::to_string(t) : a.dir,
                                     options);
            auto& mine = latency[t];
            opened.arrive_and_wait();
            long local = 0;
            for (long i = 0; i < a.count; ++i) {
                const auto begin = Clock::now();
                const auto value = reader.readNext();
                if (i % sample_every == 0) {
                    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        Clock::now() - begin)
                                        .count();
                    mine.push_back(static_cast<std::uint32_t>(ns / 100));
                }
                if (!value) break;  // ran out of archive before running out of count
                local += static_cast<long>(value->size());
            }
            bytes_read.fetch_add(local, std::memory_order_relaxed);
        });
    }

    opened.arrive_and_wait();
    const std::uint64_t sectors_before = sectorsRead(a.device);
    const auto start = Clock::now();
    for (auto& th : pool) th.join();
    const double secs = std::chrono::duration<double>(Clock::now() - start).count();
    const std::uint64_t sectors_after = sectorsRead(a.device);

    std::vector<std::uint32_t> all;
    for (auto& v : latency) all.insert(all.end(), v.begin(), v.end());
    const double payload_bytes = static_cast<double>(bytes_read.load());
    const double amp = (sectors_after > sectors_before && payload_bytes > 0)
                           ? static_cast<double>(sectors_after - sectors_before) * 512.0 /
                                 payload_bytes
                           : 0.0;
    report(payload_bytes / (1024.0 * 1024.0) / secs,
           payload_bytes / static_cast<double>(a.record) / secs, percentiles(all), amp);
    return 0;
}

int doRead(const Args& a) {
    std::vector<raf::INDEX_TYPE> index;
    {
        std::ifstream in(indexPath(a.dir), std::ios::binary | std::ios::ate);
        if (!in) {
            std::fprintf(stderr, "raf_matrix: run mode 'scan' first\n");
            return 1;
        }
        const auto bytes = in.tellg();
        index.resize(static_cast<std::size_t>(bytes) / sizeof(raf::INDEX_TYPE));
        in.seekg(0);
        in.read(reinterpret_cast<char*>(index.data()), bytes);
    }

    std::latch opened(a.threads + 1);
    std::vector<std::vector<std::uint32_t>> latency(a.threads);
    std::atomic<long> bytes_read{0};
    std::vector<std::thread> pool;
    pool.reserve(a.threads);
    const long sample_every =
        std::max<long>(1, a.count / static_cast<long>(kMaxLatencySamples) + 1);

    for (int t = 0; t < a.threads; ++t) {
        pool.emplace_back([&, t] {
            raf::Options options;
            options.directReads = !a.buffered;
            raf::RecordReader reader(a.dir, options);
            auto& mine = latency[t];
            mine.reserve(static_cast<std::size_t>(a.count / sample_every) + 1);
            opened.arrive_and_wait();
            const std::size_t n = index.size();
            // Each thread starts a different place in the same fixed walk.
            std::size_t pos = (n / static_cast<std::size_t>(a.threads)) * static_cast<std::size_t>(t);
            long local = 0;
            for (long i = 0; i < a.count; ++i) {
                pos = (pos + kStride) % n;
                const auto start = Clock::now();
                const auto value = reader.read(index[pos]);
                if (i % sample_every == 0) {
                    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        Clock::now() - start)
                                        .count();
                    mine.push_back(static_cast<std::uint32_t>(ns / 100));
                }
                if (value) local += static_cast<long>(value->size());
            }
            bytes_read.fetch_add(local, std::memory_order_relaxed);
        });
    }

    opened.arrive_and_wait();
    const std::uint64_t sectors_before = sectorsRead(a.device);
    const auto start = Clock::now();
    for (auto& th : pool) th.join();
    const double secs = std::chrono::duration<double>(Clock::now() - start).count();
    const std::uint64_t sectors_after = sectorsRead(a.device);

    std::vector<std::uint32_t> all;
    for (auto& v : latency) all.insert(all.end(), v.begin(), v.end());
    const double payload_bytes = static_cast<double>(bytes_read.load());
    // Device bytes over payload bytes. The scan ran in a separate process, so
    // nothing but these reads is counted.
    const double amp = (sectors_after > sectors_before && payload_bytes > 0)
                           ? static_cast<double>(sectors_after - sectors_before) * 512.0 /
                                 payload_bytes
                           : 0.0;
    report(payload_bytes / (1024.0 * 1024.0) / secs,
           static_cast<double>(a.count) * a.threads / secs, percentiles(all), amp);
    return 0;
}

// Sequential read through O_DIRECT.
//
// ceiling.sh puts buffered reads on this machine at 733-936 MiB/s whatever
// the engine or queue depth, and O_DIRECT at 1775-1935. That is a property
// of the two paths, measured with fio and nothing of raf in it. This asks
// the other half: with the bytes arriving that fast, can raf's framing and
// checksums keep up, or does the win evaporate in the parse?
//
// No format change is involved and none is needed. The device's logical
// block is 512 bytes, so an aligned window costs at most 511 bytes at each
// end -- it is the page cache, not the drive, that rounds a 4128 byte record
// up to two 4096 byte pages.
//
// The payload is copied out of the window. That is not an extra copy: a
// buffered pread into the caller's string is the kernel copying from the
// page cache, so a miss costs one DMA plus one copy either way. O_DIRECT
// moves the copy out of the kernel rather than adding one. It could be
// removed outright -- see the note on alignment in doDirectRandom.
int doDirectRead(const Args& a) {
    // Slack so a straddling record can be carried into the next window.
    static constexpr std::size_t kMetaPad = 8192;
    static constexpr std::size_t kAlign = 4096;  // >= the logical block size
    // Big enough that a record always fits, with room for the one straddling
    // the end of the previous window. A fixed 1 MiB silently read nothing at
    // all for 1 MiB records -- every window was one record short of holding
    // one -- and reported 0.0 MiB/s rather than an error.
    const std::size_t window =
        std::max<std::size_t>(1u << 20, ((a.record + raf::format::kMetaSize) * 2 + kAlign - 1) /
                                            kAlign * kAlign);

    std::latch opened(a.threads + 1);
    std::vector<std::vector<std::uint32_t>> latency(a.threads);
    std::atomic<long> payload_bytes{0};
    std::atomic<long> records{0};
    std::atomic<int> failures{0};
    std::vector<std::thread> pool;
    pool.reserve(a.threads);

    for (int t = 0; t < a.threads; ++t) {
        pool.emplace_back([&, t] {
            const std::string dir = a.shards > 0 ? a.dir + "." + std::to_string(t) : a.dir;
            const std::string path = dir + "/seg-00000.raf";
            const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECT);
            if (fd < 0) {
                failures.fetch_add(1, std::memory_order_relaxed);
                opened.arrive_and_wait();
                return;
            }
            void* raw = nullptr;
            if (::posix_memalign(&raw, kAlign, window + a.record + kMetaPad) != 0) {
                ::close(fd);
                failures.fetch_add(1, std::memory_order_relaxed);
                opened.arrive_and_wait();
                return;
            }
            char* const buffer = static_cast<char*>(raw);
            std::string value;
            auto& mine = latency[t];
            long local_bytes = 0, local_records = 0;

            opened.arrive_and_wait();

            // Carry the tail of each window forward: a record straddles the
            // boundary roughly always, since 1 MiB is not a multiple of 4128.
            // kHeaderSize is 4096, so the first record already starts on an
            // aligned boundary and no window ever has to be trimmed at the
            // front.
            static_assert(raf::format::kHeaderSize % kAlign == 0);
            std::uint64_t offset = raf::format::kHeaderSize;
            std::size_t carried = 0;
            while (local_records < a.count) {
                const auto begin = Clock::now();
                const ssize_t got =
                    ::pread(fd, buffer + carried, window, static_cast<off_t>(offset));
                const auto ns =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin)
                        .count();
                mine.push_back(static_cast<std::uint32_t>(ns / 100));
                if (got <= 0) break;
                offset += static_cast<std::uint64_t>(got);

                std::size_t at = 0;
                const std::size_t end = carried + static_cast<std::size_t>(got);
                while (at + raf::format::kMetaSize <= end) {
                    raf::format::RecordMeta meta{};
                    std::memcpy(&meta, buffer + at, sizeof(meta));
                    if (!raf::format::metaIsValid(meta, raf::kMaxRecordSize)) break;
                    const std::size_t total = raf::format::kMetaSize + meta.length;
                    if (at + total > end) break;
                    value.assign(buffer + at + raf::format::kMetaSize, meta.length);
                    if (raf::crc32c(value.data(), value.size()) != meta.data_crc) {
                        failures.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                    local_bytes += static_cast<long>(meta.length);
                    at += total;
                    if (++local_records >= a.count) break;
                }
                carried = end - at;
                if (carried > 0) std::memmove(buffer, buffer + at, carried);
                if (carried >= window) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    break;  // a record the window cannot hold; see above
                }
            }

            payload_bytes.fetch_add(local_bytes, std::memory_order_relaxed);
            records.fetch_add(local_records, std::memory_order_relaxed);
            std::free(raw);
            ::close(fd);
        });
    }

    opened.arrive_and_wait();
    const std::uint64_t sectors_before = sectorsRead(a.device);
    const auto start = Clock::now();
    for (auto& th : pool) th.join();
    const double secs = std::chrono::duration<double>(Clock::now() - start).count();
    const std::uint64_t sectors_after = sectorsRead(a.device);

    if (failures.load() != 0) {
        std::fprintf(stderr, "raf_matrix: odread hit %d failures\n", failures.load());
    }
    std::vector<std::uint32_t> all;
    for (auto& v : latency) all.insert(all.end(), v.begin(), v.end());
    const double bytes = static_cast<double>(payload_bytes.load());
    const double amp = (sectors_after > sectors_before && bytes > 0)
                           ? static_cast<double>(sectors_after - sectors_before) * 512.0 / bytes
                           : 0.0;
    report(bytes / (1024.0 * 1024.0) / secs,
           static_cast<double>(records.load()) / secs, percentiles(all), amp);
    return 0;
}

// Random reads through O_DIRECT.
//
// The sequential answer does not carry over by itself. A buffered random
// read that hits the page cache costs nothing at all, and O_DIRECT has no
// hits to get; against that, O_DIRECT reads 512 byte sectors where the page
// cache reads 4096 byte pages, so a 4128 byte record costs it 4608 bytes
// instead of 8192. Which of those two wins is a measurement.
int doDirectRandom(const Args& a) {
    static constexpr std::size_t kSector = 512;  // the device's logical block
    static constexpr std::size_t kAlign = 4096;

    std::vector<raf::INDEX_TYPE> index;
    {
        std::ifstream in(indexPath(a.dir), std::ios::binary | std::ios::ate);
        if (!in) {
            std::fprintf(stderr, "raf_matrix: run mode 'scan' first\n");
            return 1;
        }
        const auto bytes = in.tellg();
        index.resize(static_cast<std::size_t>(bytes) / sizeof(raf::INDEX_TYPE));
        in.seekg(0);
        in.read(reinterpret_cast<char*>(index.data()), bytes);
    }

    const std::size_t span = raf::format::kMetaSize + a.record;
    const std::size_t buffer_bytes = (span + 2 * kSector + kAlign - 1) / kAlign * kAlign;

    std::latch opened(a.threads + 1);
    std::vector<std::vector<std::uint32_t>> latency(a.threads);
    std::atomic<long> payload_bytes{0};
    std::atomic<int> failures{0};
    std::vector<std::thread> pool;
    pool.reserve(a.threads);
    const long sample_every =
        std::max<long>(1, a.count / static_cast<long>(kMaxLatencySamples) + 1);

    for (int t = 0; t < a.threads; ++t) {
        pool.emplace_back([&, t] {
            std::vector<int> fds;  // by segment id; ids start at zero and are dense
            void* raw = nullptr;
            if (::posix_memalign(&raw, kAlign, buffer_bytes) != 0) {
                failures.fetch_add(1, std::memory_order_relaxed);
                opened.arrive_and_wait();
                return;
            }
            char* const buffer = static_cast<char*>(raw);
            auto& mine = latency[t];
            long local = 0;

            auto segmentFd = [&](std::uint32_t id) {
                if (id >= fds.size()) fds.resize(id + 1, -1);
                if (fds[id] < 0) {
                    char name[32];
                    std::snprintf(name, sizeof(name), "/seg-%05u.raf", id);
                    fds[id] = ::open((a.dir + name).c_str(), O_RDONLY | O_DIRECT);
                }
                return fds[id];
            };

            opened.arrive_and_wait();
            const std::size_t n = index.size();
            std::size_t pos = (n / static_cast<std::size_t>(a.threads)) * static_cast<std::size_t>(t);
            for (long i = 0; i < a.count; ++i) {
                pos = (pos + kStride) % n;
                const raf::INDEX_TYPE idx = index[pos];
                const int fd = segmentFd(raf::segmentOf(idx));
                if (fd < 0) { failures.fetch_add(1, std::memory_order_relaxed); break; }
                const std::uint64_t offset = raf::offsetOf(idx);
                // Round out to the device's sectors, not to pages: that is
                // the whole of O_DIRECT's advantage on a small record.
                const std::uint64_t start = offset / kSector * kSector;
                const std::size_t length =
                    ((offset + span) - start + kSector - 1) / kSector * kSector;

                const auto begin = Clock::now();
                const ssize_t got = ::pread(fd, buffer, length, static_cast<off_t>(start));
                if (i % sample_every == 0) {
                    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        Clock::now() - begin)
                                        .count();
                    mine.push_back(static_cast<std::uint32_t>(ns / 100));
                }
                if (got <= 0) { failures.fetch_add(1, std::memory_order_relaxed); continue; }

                const std::size_t at = static_cast<std::size_t>(offset - start);
                raf::format::RecordMeta meta{};
                std::memcpy(&meta, buffer + at, sizeof(meta));
                if (!raf::format::metaIsValid(meta, raf::kMaxRecordSize) ||
                    at + raf::format::kMetaSize + meta.length > static_cast<std::size_t>(got)) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                // Not an extra copy: buffered, the kernel does this same
                // copy out of the page cache. It is here rather than there.
                //
                // It could be no copy at all. O_DIRECT needs the destination
                // aligned and the offset and length multiples of 512, and a
                // payload sits at 32*(k+1) mod 512 -- aligned one record in
                // sixteen. Pad the meta to 512 and every payload is aligned
                // and 4096 long, so the drive could DMA straight into the
                // caller's buffer and beat the buffered path, which cannot
                // avoid its copy_to_user. That costs 512 bytes a record
                // instead of 32, and an API that returns an aligned buffer
                // rather than a std::string.
                std::string value(buffer + at + raf::format::kMetaSize, meta.length);
                if (raf::crc32c(value.data(), value.size()) != meta.data_crc) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                local += static_cast<long>(meta.length);
            }
            payload_bytes.fetch_add(local, std::memory_order_relaxed);
            for (const int fd : fds) {
                if (fd >= 0) ::close(fd);
            }
            std::free(raw);
        });
    }

    opened.arrive_and_wait();
    const std::uint64_t sectors_before = sectorsRead(a.device);
    const auto start = Clock::now();
    for (auto& th : pool) th.join();
    const double secs = std::chrono::duration<double>(Clock::now() - start).count();
    const std::uint64_t sectors_after = sectorsRead(a.device);

    if (failures.load() != 0) {
        std::fprintf(stderr, "raf_matrix: odrand hit %d failures\n", failures.load());
    }
    std::vector<std::uint32_t> all;
    for (auto& v : latency) all.insert(all.end(), v.begin(), v.end());
    const double bytes = static_cast<double>(payload_bytes.load());
    const double amp = (sectors_after > sectors_before && bytes > 0)
                           ? static_cast<double>(sectors_after - sectors_before) * 512.0 / bytes
                           : 0.0;
    report(bytes / (1024.0 * 1024.0) / secs,
           static_cast<double>(a.count) * a.threads / secs, percentiles(all), amp);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const Args a = parse(argc, argv);
    try {
        if (a.mode == "write") return doWrite(a);
        if (a.mode == "scan") return doScan(a);
        if (a.mode == "read") return doRead(a);
        if (a.mode == "seqread") return doSeqRead(a);
        if (a.mode == "odread") return doDirectRead(a);
        if (a.mode == "odrand") return doDirectRandom(a);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "raf_matrix: %s\n", e.what());
        return 1;
    }
    usage();
}
