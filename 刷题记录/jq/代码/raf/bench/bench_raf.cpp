// raf benchmark.
//
// Covers what the design doc sets out to measure: append throughput at 4 KiB
// and 1 MiB, how it scales from one writer to several, and what sequential and
// random reads cost. Durable appends are the headline number, since that is
// what the API promises; the buffered row beside it shows what the layers above
// the disk are capable of.
//
//   raf_bench [directory] [scale]
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <latch>
#include <string>
#include <thread>
#include <vector>

#include <sys/resource.h>

#include <raf/raf.h>

namespace {

using Clock = std::chrono::steady_clock;

// What a run costs besides time. The design doc asks for resource use
// alongside throughput, and for a storage library the useful form is CPU per
// byte moved: a number that stays flat as the drive gets faster is the
// library's own cost, and one that does not is the kernel's.
struct Usage {
    double cpu_seconds = 0;
    long peak_rss_kib = 0;
};

Usage usageNow() {
    rusage ru{};
    ::getrusage(RUSAGE_SELF, &ru);
    const auto seconds = [](const timeval& t) {
        return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_usec) / 1e6;
    };
    return {seconds(ru.ru_utime) + seconds(ru.ru_stime), ru.ru_maxrss};
}

double cpuSince(const Usage& before) { return usageNow().cpu_seconds - before.cpu_seconds; }

double secondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

std::string makeValue(std::size_t size, std::uint32_t writer) {
    std::string value(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        value[i] = static_cast<char>((i * 31u) ^ (writer * 7u) ^ 0x5au);
    }
    return value;
}

void removeTree(const std::string& path) {
    const std::string command = "rm -rf '" + path + "'";
    if (std::system(command.c_str()) != 0) {
        std::fprintf(stderr, "warning: could not clean up %s\n", path.c_str());
    }
}

void header(const char* title) {
    std::printf("\n%s\n", title);
    std::printf("%-10s %8s %10s %12s %12s %10s\n", "record", "writers", "records", "records/s",
                "MiB/s", "CPU s/GiB");
}

void row(const char* label, unsigned workers, std::size_t records, double bytes, double seconds,
         double cpu_seconds) {
    const double gib = bytes / (1 << 30);
    std::printf("%-10s %8u %10zu %12.0f %12.1f %10.2f\n", label, workers, records,
                static_cast<double>(records) / seconds, bytes / seconds / (1 << 20),
                gib > 0 ? cpu_seconds / gib : 0.0);
}

// Runs `writers` threads, each appending `per_writer` records of `size`, and
// returns how long the whole thing took. The threads open their writers before
// the latch drops, so the measurement covers appends and nothing else.
double runAppend(const std::string& dir, unsigned writers, std::size_t per_writer, std::size_t size,
                 bool sync, bool batched) {
    std::latch start(1);
    std::vector<std::thread> threads;
    std::atomic<double> slowest{0};
    for (unsigned w = 0; w < writers; ++w) {
        threads.emplace_back([&, w] {
            raf::Options options;
            options.sync = sync;
            raf::RecordWriter writer(dir, options);
            const std::string value = makeValue(size, w);
            std::vector<std::string> batch;
            if (batched) {
                batch.assign(32, value);
            }
            start.wait();
            const Clock::time_point began = Clock::now();
            if (batched) {
                for (std::size_t i = 0; i < per_writer; i += batch.size()) {
                    writer.appendBatch(batch);
                }
            } else {
                for (std::size_t i = 0; i < per_writer; ++i) {
                    writer.append(value);
                }
            }
            const double elapsed = secondsSince(began);
            double previous = slowest.load();
            while (previous < elapsed && !slowest.compare_exchange_weak(previous, elapsed)) {
            }
            writer.close();
        });
    }
    start.count_down();
    for (std::thread& thread : threads) {
        thread.join();
    }
    return slowest.load();
}

struct Prepared {
    std::string dir;
    std::vector<raf::INDEX_TYPE> indexes;
    std::size_t size;
};

Prepared prepare(const std::string& root, std::size_t size, std::size_t count) {
    Prepared prepared;
    prepared.dir = root + "/read-" + std::to_string(size);
    prepared.size = size;
    raf::Options options;
    options.sync = false;  // filling the archive is setup, not the measurement
    raf::RecordWriter writer(prepared.dir, options);
    const std::string value = makeValue(size, 0);
    std::vector<std::string> batch(64, value);
    for (std::size_t i = 0; i < count; i += batch.size()) {
        const std::vector<raf::INDEX_TYPE> indexes = writer.appendBatch(batch);
        prepared.indexes.insert(prepared.indexes.end(), indexes.begin(), indexes.end());
    }
    writer.close();
    return prepared;
}

void benchmarkReads(const Prepared& prepared) {
    {
        const Usage before = usageNow();
        const Clock::time_point began = Clock::now();
        raf::RecordReader reader(prepared.dir);
        std::size_t count = 0;
        while (reader.readNext()) {
            ++count;
        }
        const double seconds = secondsSince(began);
        row("sequential", 1, count, static_cast<double>(count * prepared.size), seconds,
            cpuSince(before));
    }

    for (const unsigned threads : {1u, 2u, 4u, 8u}) {
        raf::RecordReader reader(prepared.dir);
        std::latch start(1);
        std::vector<std::thread> workers;
        const std::size_t per_thread = prepared.indexes.size();
        for (unsigned t = 0; t < threads; ++t) {
            workers.emplace_back([&, t] {
                start.wait();
                for (std::size_t step = 0; step < per_thread; ++step) {
                    // A fixed stride so every run reads the same sequence.
                    const std::size_t i = (step * 1103515245u + t * 2654435761u) % per_thread;
                    reader.read(prepared.indexes[i]);
                }
            });
        }
        const Usage before = usageNow();
        const Clock::time_point began = Clock::now();
        start.count_down();
        for (std::thread& worker : workers) {
            worker.join();
        }
        const double seconds = secondsSince(began);
        const std::size_t total = per_thread * threads;
        row("random", threads, total, static_cast<double>(total * prepared.size), seconds,
            cpuSince(before));
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string root = argc > 1 ? argv[1] : "/tmp/raf-bench";
    const std::size_t scale = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 1;
    removeTree(root);
    if (::mkdir(root.c_str(), 0755) != 0) {
        std::perror("mkdir");
        return 1;
    }

    std::printf("raf benchmark, archive root %s, scale %zu\n", root.c_str(), scale);

    struct Shape {
        const char* label;
        std::size_t size;
        std::size_t per_writer;
    };
    const Shape shapes[] = {{"4 KiB", 4096, 400 * scale}, {"1 MiB", 1u << 20, 32 * scale}};

    header("durable appends (O_DSYNC; a returning append is a durable record)");
    for (const Shape& shape : shapes) {
        for (const unsigned writers : {1u, 2u, 4u, 8u}) {
            const std::string dir =
                root + "/sync-" + std::to_string(shape.size) + "-" + std::to_string(writers);
            const Usage before = usageNow();
            const double seconds =
                runAppend(dir, writers, shape.per_writer, shape.size, true, false);
            row(shape.label, writers, shape.per_writer * writers,
                static_cast<double>(shape.per_writer * writers * shape.size), seconds,
                cpuSince(before));
        }
    }

    header("buffered appends (sync off: the ceiling the layers above the disk allow)");
    for (const Shape& shape : shapes) {
        for (const unsigned writers : {1u, 4u, 8u}) {
            const std::string dir =
                root + "/nosync-" + std::to_string(shape.size) + "-" + std::to_string(writers);
            const std::size_t per_writer = shape.per_writer * 4;
            const Usage before = usageNow();
            const double seconds = runAppend(dir, writers, per_writer, shape.size, false, false);
            row(shape.label, writers, per_writer * writers,
                static_cast<double>(per_writer * writers * shape.size), seconds,
                cpuSince(before));
        }
    }

    header("batched appends, 32 records per call (sync off)");
    for (const Shape& shape : shapes) {
        for (const unsigned writers : {1u, 4u}) {
            const std::string dir =
                root + "/batch-" + std::to_string(shape.size) + "-" + std::to_string(writers);
            const std::size_t per_writer = shape.per_writer * 4;
            const Usage before = usageNow();
            const double seconds = runAppend(dir, writers, per_writer, shape.size, false, true);
            row(shape.label, writers, per_writer * writers,
                static_cast<double>(per_writer * writers * shape.size), seconds,
                cpuSince(before));
        }
    }

    for (const Shape& shape : shapes) {
        const std::size_t count = shape.per_writer * 8;
        const Prepared prepared = prepare(root, shape.size, count);
        std::string title = "reads of " + std::string(shape.label) + " records";
        header(title.c_str());
        benchmarkReads(prepared);

        // Opening an archive scans every segment to find where its records end.
        const Clock::time_point began = Clock::now();
        { raf::RecordWriter writer(prepared.dir); }
        std::printf("recovery scan of %zu records: %.3f s\n", count, secondsSince(began));
    }

    const Usage total = usageNow();
    std::printf("\nresources: %.1f s of CPU, peak RSS %.1f MiB\n", total.cpu_seconds,
                static_cast<double>(total.peak_rss_kib) / 1024.0);

    removeTree(root);
    return 0;
}
