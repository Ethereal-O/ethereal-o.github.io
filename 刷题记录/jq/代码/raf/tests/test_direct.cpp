// Reading with Options::directReads.
//
// O_DIRECT gives the reader no page cache and three alignment rules to obey
// -- offset, length and buffer address all multiples of the device's logical
// block. Everything the buffered reader promises has to survive that: the
// same records in the same order, the same verdict on damage, the same view
// of a writer still appending.
//
// A filesystem that cannot do O_DIRECT at all (tmpfs, most notably) makes
// opening fail, and these skip rather than fail -- the suite should not
// depend on where TMPDIR points.
#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <raf/raf.h>

#include <cerrno>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <system_error>

#include "io/io_backend.h"
#include "store/read_window.h"
#include "store/segment.h"

#include "test_support.h"

namespace raf::test {
namespace {

// O_DIRECT is the default, so it is the other side that has to be asked for.
Options bufferedOptions() {
    Options options;
    options.directReads = false;
    return options;
}

// True if this filesystem will open a file O_DIRECT at all.
bool directSupported(const std::string& dir) {
    const std::string probe = dir + "/.direct-probe";
    const int fd = ::open(probe.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return false;
    }
    ::close(fd);
    const int direct = ::open(probe.c_str(), O_RDONLY | O_DIRECT);
    const bool ok = direct >= 0;
    if (ok) {
        ::close(direct);
    }
    ::unlink(probe.c_str());
    return ok;
}

#define SKIP_WITHOUT_DIRECT(dir)                                       \
    do {                                                               \
        if (!directSupported(dir)) {                                   \
            GTEST_SKIP() << "O_DIRECT is not available on " << (dir);  \
        }                                                              \
    } while (false)

// Records of a size that is not a multiple of anything, so every one of them
// starts and ends in the middle of a sector.
TEST(DirectRead, IteratesEveryRecordInOrder) {
    TempArchive archive;
    SKIP_WITHOUT_DIRECT(archive.root());

    constexpr int kCount = 500;
    {
        RecordWriter writer(archive.path());
        for (int i = 0; i < kCount; ++i) {
            writer.append(makePayload(0, i, 37 + (i % 11)));
        }
        writer.close();
    }

    RecordReader reader(archive.path());
    const std::vector<std::string> values = drain(reader);
    ASSERT_EQ(values.size(), static_cast<std::size_t>(kCount));
    for (int i = 0; i < kCount; ++i) {
        EXPECT_EQ(values[i], makePayload(0, i, 37 + (i % 11))) << "record " << i;
    }
}

// Buffered and direct readers must agree record for record.
TEST(DirectRead, AgreesWithTheBufferedReader) {
    TempArchive archive;
    SKIP_WITHOUT_DIRECT(archive.root());

    {
        RecordWriter writer(archive.path());
        for (int i = 0; i < 300; ++i) {
            writer.append(makePayload(7, i, 1 + (i * 97) % 9000));
        }
        writer.close();
    }

    RecordReader buffered(archive.path(), bufferedOptions());
    RecordReader direct(archive.path());
    EXPECT_EQ(drain(buffered), drain(direct));
}

// Iteration and random lookup take different paths through the window --
// one slides a window forward, the other speculates into a fresh one every
// call -- so agreeing on the bytes is not a given.
TEST(DirectRead, IterationAndRandomLookupAgree) {
    TempArchive archive;
    SKIP_WITHOUT_DIRECT(archive.root());

    std::vector<INDEX_TYPE> index;
    std::vector<std::string> written;
    {
        RecordWriter writer(archive.path());
        for (int i = 0; i < 300; ++i) {
            written.push_back(makePayload(7, i, 1 + (i * 97) % 9000));
            index.push_back(writer.append(written.back()));
        }
        writer.close();
    }

    RecordReader reader(archive.path());
    EXPECT_EQ(drain(reader), written);

    RecordReader lookup(archive.path());
    for (std::size_t i = 0; i < index.size(); ++i) {
        EXPECT_EQ(lookup.read(index[i]), written[i]) << "record " << i;
    }
}

// The window is a megabyte; a record larger than it must still come back
// whole, which is the case the oversized-read path exists for.
TEST(DirectRead, HandlesARecordLargerThanTheWindow) {
    TempArchive archive;
    SKIP_WITHOUT_DIRECT(archive.root());

    const std::string big = makePayload(1, 0, 3u << 20);
    {
        RecordWriter writer(archive.path());
        writer.append(makePayload(1, 1, 64));
        writer.append(big);
        writer.append(makePayload(1, 2, 64));
        writer.close();
    }

    RecordReader reader(archive.path());
    const std::vector<std::string> values = drain(reader);
    ASSERT_EQ(values.size(), 3u);
    EXPECT_EQ(values[1], big);
    EXPECT_TRUE(payloadIsIntact(values[1]));
}

// Random lookup takes a different path from iteration -- no window, one
// aligned read per call, and several threads on one segment.
TEST(DirectRead, RandomLookupReturnsTheSameRecords) {
    TempArchive archive;
    SKIP_WITHOUT_DIRECT(archive.root());

    std::vector<INDEX_TYPE> index;
    {
        RecordWriter writer(archive.path());
        for (int i = 0; i < 200; ++i) {
            index.push_back(writer.append(makePayload(3, i, 500 + i)));
        }
        writer.close();
    }

    RecordReader reader(archive.path());
    // Backwards, so nothing can pass by reading in the order it was written.
    for (int i = 199; i >= 0; --i) {
        const std::optional<std::string> value = reader.read(index[i]);
        ASSERT_TRUE(value.has_value()) << "record " << i;
        EXPECT_EQ(*value, makePayload(3, i, 500 + i));
    }
}

// A reader that catches up with a live writer, then sees more.
TEST(DirectRead, SeesRecordsAppendedAfterItCaughtUp) {
    TempArchive archive;
    SKIP_WITHOUT_DIRECT(archive.root());

    RecordWriter writer(archive.path());
    for (int i = 0; i < 20; ++i) {
        writer.append(makePayload(0, i, 300));
    }

    RecordReader reader(archive.path());
    EXPECT_EQ(drain(reader).size(), 20u);
    EXPECT_FALSE(reader.readNext().has_value());

    for (int i = 20; i < 40; ++i) {
        writer.append(makePayload(0, i, 300));
    }

    // The window holds bytes from before the append; a stale one would
    // report the end of the archive here.
    const std::vector<std::string> more = drain(reader);
    ASSERT_EQ(more.size(), 20u);
    EXPECT_EQ(more.front(), makePayload(0, 20, 300));
    writer.close();
}

// Damage must read as damage, not as the end of the data.
TEST(DirectRead, ReportsADamagedPayloadLikeTheBufferedReader) {
    TempArchive archive;
    SKIP_WITHOUT_DIRECT(archive.root());

    {
        RecordWriter writer(archive.path());
        for (int i = 0; i < 10; ++i) {
            writer.append(makePayload(0, i, 400));
        }
        writer.close();
    }

    const std::vector<std::string> paths = segmentPaths(archive.path());
    ASSERT_EQ(paths.size(), 1u);
    // Into the third record's payload, past its header.
    flipBit(paths[0], format::kHeaderSize + 2 * format::recordSpan(400) + format::kMetaSize + 11);

    RecordReader buffered(archive.path(), bufferedOptions());
    RecordReader direct(archive.path());
    const std::vector<std::string> from_buffered = drain(buffered);
    const std::vector<std::string> from_direct = drain(direct);
    EXPECT_EQ(from_buffered, from_direct);
    EXPECT_EQ(from_direct.size(), 9u);
    for (const std::string& value : from_direct) {
        EXPECT_TRUE(payloadIsIntact(value));
    }
}

// writeRaw needs the file to exist; these two make it.
void createFile(const std::string& path, std::size_t bytes, char fill) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const std::string blob(bytes, fill);
    out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
    if (!out) {
        throw std::runtime_error("could not create " + path);
    }
}

// An IoBackend that forwards everything and counts what it was asked to
// read. The window's whole job is to ask for the right number of bytes, and
// nothing above it can see whether it did.
class CountingBackend : public io::IoBackend {
public:
    explicit CountingBackend(io::IoBackend& inner) : inner_(inner) {}

    std::size_t requested = 0;
    int calls = 0;
    // Set to simulate a process out of descriptors.
    bool fail_open = false;
    int opens = 0;

    std::size_t readSome(int fd, iovec* iov, int count, std::uint64_t offset) override {
        for (int i = 0; i < count; ++i) {
            requested += iov[i].iov_len;
        }
        ++calls;
        return inner_.readSome(fd, iov, count, offset);
    }

    int open(const std::string& path, const io::OpenFlags& flags) override {
        ++opens;
        if (fail_open) {
            throw std::system_error(EMFILE, std::system_category(), "open " + path);
        }
        return inner_.open(path, flags);
    }
    int tryOpen(const std::string& path, const io::OpenFlags& flags, int* err) override {
        return inner_.tryOpen(path, flags, err);
    }
    void close(int fd) noexcept override { inner_.close(fd); }
    void writeAll(int fd, iovec* iov, int count, std::uint64_t offset) override {
        inner_.writeAll(fd, iov, count, offset);
    }
    void fsync(int fd) override { inner_.fsync(fd); }
    void truncate(int fd, std::uint64_t length) override { inner_.truncate(fd, length); }
    std::uint64_t size(int fd) override { return inner_.size(fd); }
    bool tryLockExclusive(int fd) override { return inner_.tryLockExclusive(fd); }
    void unlock(int fd) noexcept override { inner_.unlock(fd); }
    void syncDirectory(const std::string& path) override { inner_.syncDirectory(path); }
    void makeDirectory(const std::string& path) override { inner_.makeDirectory(path); }
    bool directoryExists(const std::string& path) override { return inner_.directoryExists(path); }
    std::vector<std::string> listDirectory(const std::string& path) override {
        return inner_.listDirectory(path);
    }

private:
    io::IoBackend& inner_;
};

// A window with no readahead must read what it was asked for and no more.
//
// It used to read its whole buffer every time. The buffer for a random
// lookup is sized for the largest payload worth speculating on, 64 KiB, so
// every 4 KiB lookup moved sixteen times the bytes it needed -- which the
// benchmark saw as an amplification of 16.12 and nothing in the suite saw at
// all.
TEST(ReadWindowBytes, FetchesWhatWasAskedForWhenThereIsNoReadahead) {
    TempArchive archive;
    const std::string path = archive.root() + "/bytes.dat";
    createFile(path, 64 * 1024, 'x');

    CountingBackend backend(io::posixBackend());
    io::OpenFlags flags;
    flags.read_only = true;
    const int fd = backend.open(path, flags);

    // Sized like the one a random lookup uses, configured like it too.
    store::ReadWindow window(0);
    char out[4096];
    ASSERT_EQ(window.read(backend, fd, 8192, out, sizeof(out), 32 + sizeof(out)), sizeof(out));
    backend.close(fd);

    EXPECT_EQ(backend.calls, 1);
    // One sector of slack at each end is all the alignment can cost.
    EXPECT_LE(backend.requested, sizeof(out) + 32 + 2 * store::ReadWindow::kSector);
}

// A window that had to grow for one oversized record must not keep reading
// that much afterwards. It used to read its whole buffer every time, so one
// large record made every small one after it expensive.
TEST(ReadWindowBytes, DoesNotKeepReadingABufferItHadToGrow) {
    TempArchive archive;
    const std::string path = archive.root() + "/grown.dat";
    createFile(path, 256 * 1024, 'z');

    CountingBackend backend(io::posixBackend());
    io::OpenFlags flags;
    flags.read_only = true;
    const int fd = backend.open(path, flags);

    store::ReadWindow window(0);
    std::vector<char> big(128 * 1024);
    ASSERT_EQ(window.read(backend, fd, 0, big.data(), big.size()), big.size());

    backend.requested = 0;
    backend.calls = 0;
    char small[4096];
    ASSERT_EQ(window.read(backend, fd, 200 * 1024, small, sizeof(small)), sizeof(small));
    backend.close(fd);

    EXPECT_EQ(backend.calls, 1);
    EXPECT_LE(backend.requested, sizeof(small) + 2 * store::ReadWindow::kSector)
        << "the grown buffer should not set the size of later reads";
}

// What a random lookup costs the device, which is the thing the window gets
// wrong in a way nothing above it can see.
//
// It was sized for the largest payload worth speculating on, 64 KiB, and
// filled itself every time, so a 4 KiB lookup moved sixteen times the bytes
// it needed. The benchmark called that an amplification of 16.12; the suite
// called it nothing at all.
TEST(ReadWindowBytes, ARandomLookupReadsAboutOneRecord) {
    TempArchive archive;
    SKIP_WITHOUT_DIRECT(archive.root());

    constexpr std::size_t kPayload = 4096;
    std::vector<INDEX_TYPE> index;
    {
        RecordWriter writer(archive.path());
        for (int i = 0; i < 40; ++i) {
            index.push_back(writer.append(makePayload(0, i, kPayload)));
        }
        writer.close();
    }

    const std::vector<std::string> paths = segmentPaths(archive.path());
    ASSERT_EQ(paths.size(), 1u);

    CountingBackend backend(io::posixBackend());
    io::OpenFlags flags;
    flags.read_only = true;
    flags.direct = true;
    const int fd = backend.open(paths[0], flags);
    store::Segment segment(backend, fd, 0, paths[0], /*locked=*/false, /*direct=*/true);
    ASSERT_TRUE(segment.verifyHeader());

    // The first lookup has nothing to speculate with and reads the header
    // alone before the payload; from the second on it reads both at once.
    ASSERT_TRUE(segment.readRecord(offsetOf(index[0])).has_value());
    backend.requested = 0;
    backend.calls = 0;
    for (int i = 1; i < 20; ++i) {
        ASSERT_TRUE(segment.readRecord(offsetOf(index[i])).has_value()) << "record " << i;
    }

    const std::size_t per_record = backend.requested / 19;
    EXPECT_LE(per_record, kPayload + format::kMetaSize + 2 * store::ReadWindow::kSector)
        << "read " << per_record << " bytes to return a " << kPayload << " byte record";
}

// Configured with readahead, the same window fetches ahead -- that is what
// makes iteration one system call per megabyte instead of one per record.
TEST(ReadWindowBytes, FetchesAheadWhenToldTo) {
    TempArchive archive;
    const std::string path = archive.root() + "/ahead.dat";
    createFile(path, 256 * 1024, 'y');

    CountingBackend backend(io::posixBackend());
    io::OpenFlags flags;
    flags.read_only = true;
    const int fd = backend.open(path, flags);

    store::ReadWindow window(128 * 1024);
    char out[64];
    for (int i = 0; i < 100; ++i) {
        ASSERT_EQ(window.read(backend, fd, 1000 + i * 64, out, sizeof(out)), sizeof(out));
    }
    backend.close(fd);

    EXPECT_EQ(backend.calls, 1) << "a hundred reads inside one window";
    EXPECT_EQ(backend.requested, 128u * 1024);
}

// Appending must survive a process that has run out of descriptors, and it
// must not keep asking for one.
//
// The frontier is published through a second descriptor on a file the
// segment already has open, so the usual way it becomes unavailable is the
// process hitting its limit -- 512 writers want 1024 descriptors against a
// default of 1024, which is what a sweep to 512 writers ran into. Asking
// again on the next append put a failing open() on every record.
TEST(FrontierDescriptor, AppendsSurviveAndStopAskingWhenDescriptorsRunOut) {
    TempArchive archive;
    const std::string path = archive.root() + "/seg-00000.raf";

    CountingBackend backend(io::posixBackend());
    io::OpenFlags flags;
    flags.create = true;
    const int fd = backend.open(path, flags);
    store::Segment segment(backend, fd, 0, path, /*locked=*/true);
    segment.initialize(1234, kMaxRecordSize);

    // From here the process has no descriptors left.
    backend.fail_open = true;
    backend.opens = 0;

    store::AppendScratch scratch;
    constexpr int kCount = 50;
    for (int i = 0; i < kCount; ++i) {
        const std::string payload = makePayload(0, i, 128);
        const std::string_view view(payload);
        INDEX_TYPE index = kInvalidIndex;
        ASSERT_NO_THROW(segment.appendRecords(&view, 1, &index, scratch)) << "append " << i;
    }

    EXPECT_EQ(backend.opens, 1) << "one attempt for the whole run, not one per append";

    // The records are there regardless: the frontier is a diagnostic, and
    // losing it must not lose data.
    for (int i = 0; i < kCount; ++i) {
        const std::optional<std::string> value =
            segment.readRecord(format::kHeaderSize + i * format::recordSpan(128));
        ASSERT_TRUE(value.has_value()) << "record " << i;
        EXPECT_EQ(*value, makePayload(0, i, 128));
    }
}

// Iteration must not re-read what it has already fetched, whatever the
// record size.
//
// A window refill that starts part way into a record ends part way into
// another, and every refill after it inherits that offset. At 256 KiB -- four
// records to a one megabyte window -- that cost a third of the device's
// bandwidth: measured 1.34 bytes asked of the device per payload byte, where
// the arithmetic says just over 1.00.
TEST(ReadWindowBytes, IterationDoesNotRefetchAtAnyRecordSize) {
    for (const std::size_t payload : {std::size_t{4096}, std::size_t{262144}}) {
        TempArchive archive;
        if (!directSupported(archive.root())) {
            GTEST_SKIP() << "O_DIRECT is not available";
        }
        // Large enough that the first and last window stop setting the
        // answer: a sixteen megabyte archive reads 1.12 at 4 KiB purely
        // because two of its eighteen windows are edges.
        const long total = static_cast<long>(64u << 20);
        const int count = static_cast<int>(total / static_cast<long>(payload));
        {
            RecordWriter writer(archive.path());
            std::vector<std::string> batch;
            for (int i = 0; i < count; ++i) {
                batch.push_back(makePayload(0, i, payload));
                if (batch.size() == 64 || i + 1 == count) {
                    writer.appendBatch(batch);
                    batch.clear();
                }
            }
            writer.close();
        }

        const std::vector<std::string> paths = segmentPaths(archive.path());
        ASSERT_EQ(paths.size(), 1u);

        CountingBackend backend(io::posixBackend());
        io::OpenFlags flags;
        flags.read_only = true;
        flags.direct = true;
        const int fd = backend.open(paths[0], flags);
        store::Segment segment(backend, fd, 0, paths[0], /*locked=*/false, /*direct=*/true);
        ASSERT_TRUE(segment.verifyHeader());

        store::ReadWindow window;
        std::uint64_t offset = format::kHeaderSize;
        std::string value;
        format::RecordMeta meta{};
        std::optional<format::RecordMeta> next;
        long bytes = 0;
        int read = 0;
        backend.requested = 0;
        while (read < count) {
            ASSERT_EQ(segment.fetchMeta(offset, meta, &window), store::RecordStatus::kOk);
            ASSERT_EQ(segment.readPayload(offset, meta, value, next, &window),
                      store::RecordStatus::kOk);
            bytes += static_cast<long>(meta.length);
            offset += format::recordSpan(meta.length);
            ++read;
        }
        backend.close(fd);

        const double ratio = static_cast<double>(backend.requested) / static_cast<double>(bytes);
        EXPECT_LT(ratio, 1.10) << payload << " byte records read " << ratio
                               << " bytes from the device per payload byte, in "
                               << backend.calls << " calls for " << read << " records";
    }
}

}  // namespace
}  // namespace raf::test
