// Store layer: the segment file format, and what a scan makes of a file that
// a crash or a bad sector got to first.
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "store/archive.h"
#include "store/segment.h"
#include "store/format.h"
#include "test_support.h"

namespace raf {
namespace {

using store::Archive;
using store::RecordStatus;
using store::ScanResult;
using store::Segment;

class SegmentTest : public ::testing::Test {
protected:
    io::IoBackend& backend = io::posixBackend();
    test::TempArchive tmp{"raf-segment"};
    Options options;

    SegmentTest() { options.sync = false; }

    std::unique_ptr<Segment> acquire() {
        Archive::create(backend, tmp.path());
        return Archive::acquireForWrite(backend, tmp.path(), options);
    }

    // Appends `values` through the store layer and returns their indexes.
    std::vector<INDEX_TYPE> append(Segment& segment, const std::vector<std::string>& values) {
        std::vector<std::string_view> views(values.begin(), values.end());
        std::vector<INDEX_TYPE> indexes(values.size());
        store::AppendScratch scratch;
        segment.appendRecords(views.data(), views.size(), indexes.data(), scratch);
        return indexes;
    }

    std::string segmentZero() const { return Archive::segmentPath(tmp.path(), 0); }
};

TEST_F(SegmentTest, ANewSegmentStartsWithAValidHeader) {
    std::unique_ptr<Segment> segment = acquire();
    ASSERT_TRUE(segment);
    EXPECT_EQ(segment->id(), 0u);
    EXPECT_EQ(segment->writeOffset(), format::kHeaderSize);
    EXPECT_TRUE(segment->verifyHeader());
    EXPECT_EQ(test::fileSize(segmentZero()), format::kHeaderSize);
}

TEST_F(SegmentTest, SegmentNamesRoundTrip) {
    EXPECT_EQ(Archive::segmentPath("/tmp/a", 0), "/tmp/a/seg-00000.raf");
    EXPECT_EQ(Archive::segmentPath("/tmp/a/", 42), "/tmp/a/seg-00042.raf");
    EXPECT_EQ(Archive::parseSegmentName("seg-00042.raf"), 42u);
    EXPECT_EQ(Archive::parseSegmentName("seg-00000.raf"), 0u);
    EXPECT_FALSE(Archive::parseSegmentName("seg-.raf").has_value());
    EXPECT_FALSE(Archive::parseSegmentName("seg-00042.tmp").has_value());
    EXPECT_FALSE(Archive::parseSegmentName("other-00042.raf").has_value());
    EXPECT_FALSE(Archive::parseSegmentName("seg-0004x.raf").has_value());
    // Past what the index can address.
    EXPECT_FALSE(Archive::parseSegmentName("seg-99999.raf").has_value());
}

TEST_F(SegmentTest, RecordsLandBackToBackAfterTheHeader) {
    std::unique_ptr<Segment> segment = acquire();
    const std::vector<std::string> values = {"a", "", std::string(1000, 'x')};
    const std::vector<INDEX_TYPE> indexes = append(*segment, values);

    std::uint64_t expected = format::kHeaderSize;
    for (std::size_t i = 0; i < values.size(); ++i) {
        EXPECT_EQ(offsetOf(indexes[i]), expected);
        expected += format::recordSpan(static_cast<std::uint32_t>(values[i].size()));
    }
    EXPECT_EQ(segment->writeOffset(), expected);
    EXPECT_EQ(test::fileSize(segmentZero()), expected);

    for (std::size_t i = 0; i < values.size(); ++i) {
        EXPECT_EQ(segment->readRecord(offsetOf(indexes[i])), values[i]);
    }
}

TEST_F(SegmentTest, AppendTimestampsNeverGoBackwards) {
    std::unique_ptr<Segment> segment = acquire();
    const std::vector<INDEX_TYPE> indexes = append(*segment, {"a", "b", "c"});
    std::uint64_t previous = 0;
    for (const INDEX_TYPE index : indexes) {
        const format::RecordMeta meta = test::readMeta(segmentZero(), offsetOf(index));
        EXPECT_GE(meta.timestamp_ns, previous);
        previous = meta.timestamp_ns;
    }
}

TEST_F(SegmentTest, SequenceNumbersContinueAfterReopen) {
    {
        std::unique_ptr<Segment> segment = acquire();
        append(*segment, {"a", "b"});
    }
    std::unique_ptr<Segment> reopened = acquire();
    EXPECT_EQ(reopened->nextSequence(), 2u);
    const std::vector<INDEX_TYPE> indexes = append(*reopened, {"c"});
    EXPECT_EQ(test::readMeta(segmentZero(), offsetOf(indexes[0])).sequence, 2u);
}

TEST_F(SegmentTest, ScanFindsEveryRecordAfterAReopen) {
    std::uint64_t end = 0;
    {
        std::unique_ptr<Segment> segment = acquire();
        append(*segment, {"one", "two", "three"});
        end = segment->writeOffset();
    }
    std::unique_ptr<Segment> reopened = acquire();
    EXPECT_EQ(reopened->writeOffset(), end);
}

// Trailing bytes that are not a record are what a crash mid-write leaves. They
// belong to an append that never returned, so dropping them loses nothing the
// API promised.
TEST_F(SegmentTest, ScanDropsATornTail) {
    std::uint64_t end = 0;
    {
        std::unique_ptr<Segment> segment = acquire();
        append(*segment, {"one", "two"});
        end = segment->writeOffset();
    }
    const std::string garbage(17, '\xa5');
    test::writeRaw(segmentZero(), end, garbage.data(), garbage.size());
    ASSERT_EQ(test::fileSize(segmentZero()), end + garbage.size());

    std::unique_ptr<Segment> reopened = acquire();
    EXPECT_EQ(reopened->writeOffset(), end);
    EXPECT_EQ(test::fileSize(segmentZero()), end) << "the tail must be truncated, not left behind";
    EXPECT_EQ(reopened->readRecord(format::kHeaderSize), "one");
}

TEST_F(SegmentTest, ScanDropsARecordWhosePayloadNeverLanded) {
    std::uint64_t end = 0;
    {
        std::unique_ptr<Segment> segment = acquire();
        append(*segment, {"one"});
        end = segment->writeOffset();
        append(*segment, {std::string(500, 'p')});
    }
    // Cut the second record's payload in half: header on disk, payload not.
    test::truncateFile(segmentZero(), end + format::kMetaSize + 250);

    std::unique_ptr<Segment> reopened = acquire();
    EXPECT_EQ(reopened->writeOffset(), end);
    EXPECT_EQ(test::fileSize(segmentZero()), end);
}

// A payload that fails its checksum with a good record behind it is damage,
// not a torn tail, and the records after it are still the reader's to return.
TEST_F(SegmentTest, ScanStepsOverADamagedRecordAndKeepsTheRestOfTheFile) {
    std::vector<INDEX_TYPE> indexes;
    std::uint64_t end = 0;
    {
        std::unique_ptr<Segment> segment = acquire();
        indexes = append(*segment, {"first", "second", "third"});
        end = segment->writeOffset();
    }
    test::flipBit(segmentZero(), offsetOf(indexes[1]) + format::kMetaSize);

    std::unique_ptr<Segment> reopened = acquire();
    EXPECT_EQ(reopened->writeOffset(), end) << "the file must not shrink past a good record";
    EXPECT_EQ(test::fileSize(segmentZero()), end);
    EXPECT_EQ(reopened->readRecord(offsetOf(indexes[0])), "first");
    EXPECT_EQ(reopened->readRecord(offsetOf(indexes[1])), std::nullopt);
    EXPECT_EQ(reopened->readRecord(offsetOf(indexes[2])), "third");
}

TEST_F(SegmentTest, ScanCountsWhatItFound) {
    std::vector<INDEX_TYPE> indexes;
    {
        std::unique_ptr<Segment> segment = acquire();
        indexes = append(*segment, {"first", "second", "third"});
    }
    test::flipBit(segmentZero(), offsetOf(indexes[1]) + format::kMetaSize);
    const std::string garbage(9, '\x00');
    test::writeRaw(segmentZero(), test::fileSize(segmentZero()), garbage.data(), garbage.size());

    // Recovery truncates, so it needs the segment open for writing.
    io::OpenFlags flags;
    const int fd = backend.open(segmentZero(), flags);
    ASSERT_TRUE(backend.tryLockExclusive(fd));
    Segment segment(backend, fd, 0, segmentZero(), /*locked=*/true);
    const ScanResult scan = segment.recover();
    EXPECT_EQ(scan.record_count, 2u);
    EXPECT_EQ(scan.damaged_records, 1u);
    EXPECT_EQ(scan.truncated_bytes, garbage.size());
    EXPECT_EQ(scan.next_sequence, 3u);
}

// The last record in the file is still a record the writer acknowledged, and
// the published frontier says so. Breaking it is damage, not an unfinished
// write -- which the old file-length rule could not tell, because a file that
// stops right after a record is never long enough to prove anything.
TEST_F(SegmentTest, ABadHeaderBelowTheFrontierIsDamage) {
    std::vector<INDEX_TYPE> indexes;
    {
        std::unique_ptr<Segment> segment = acquire();
        indexes = append(*segment, {"first", "second"});
    }
    test::flipBit(segmentZero(), offsetOf(indexes[1]));

    std::unique_ptr<Segment> segment = Archive::openForRead(backend, tmp.path(), 0);
    ASSERT_TRUE(segment);
    format::RecordMeta meta{};
    EXPECT_EQ(segment->fetchMeta(offsetOf(indexes[0]), meta), RecordStatus::kOk);
    EXPECT_EQ(segment->fetchMeta(offsetOf(indexes[1]), meta), RecordStatus::kDamaged);
}

// Past the frontier the writer never acknowledged anything, so bytes that do
// not parse are an unfinished write however garbled they look.
TEST_F(SegmentTest, ABadHeaderAboveTheFrontierLooksLikeAnUnfinishedWrite) {
    std::uint64_t tail = 0;
    {
        std::unique_ptr<Segment> segment = acquire();
        append(*segment, {"first", "second"});
        tail = segment->writeOffset();
    }
    // Debris of the kind a torn write leaves: not zero, not a valid header.
    const std::string debris(format::kMetaSize, '\xA5');
    test::writeRaw(segmentZero(), tail, debris.data(), debris.size());

    std::unique_ptr<Segment> segment = Archive::openForRead(backend, tmp.path(), 0);
    ASSERT_TRUE(segment);
    format::RecordMeta meta{};
    EXPECT_EQ(segment->fetchMeta(tail, meta), RecordStatus::kMissing);
}

// The frontier is a lower bound with no checksum of its own, so a value that
// cannot be true is ignored rather than believed. Believing an overstated one
// would turn every unwritten byte into reported damage.
TEST_F(SegmentTest, AnImpossibleFrontierIsIgnored) {
    {
        std::unique_ptr<Segment> segment = acquire();
        append(*segment, {"first"});
    }
    const std::uint64_t absurd = ~std::uint64_t{0};
    test::writeRaw(segmentZero(), format::kFrontierOffset, &absurd, sizeof(absurd));

    std::unique_ptr<Segment> segment = Archive::openForRead(backend, tmp.path(), 0);
    ASSERT_TRUE(segment);
    EXPECT_EQ(segment->publishedFrontier(), 0u);
}

// The file's length has nothing to do with this verdict any more. The old
// rule rested on it entirely -- a damaged header counted as damage only once
// the file had grown a whole record past it -- which is why a preallocated
// file broke it and a dead writer's file never satisfied it.
TEST_F(SegmentTest, FileLengthDoesNotDecideDamage) {
    std::vector<INDEX_TYPE> indexes;
    {
        std::unique_ptr<Segment> segment = acquire();
        indexes = append(*segment, {"first", "second"});
    }
    test::flipBit(segmentZero(), offsetOf(indexes[1]));

    format::RecordMeta meta{};
    {
        std::unique_ptr<Segment> segment = Archive::openForRead(backend, tmp.path(), 0);
        ASSERT_TRUE(segment);
        EXPECT_EQ(segment->fetchMeta(offsetOf(indexes[1]), meta), RecordStatus::kDamaged);
    }

    // Same bytes, much longer file: still damage, where before the length was
    // the only thing that made it so.
    test::truncateFile(segmentZero(), offsetOf(indexes[1]) + 16 * 1024 * 1024);
    {
        std::unique_ptr<Segment> segment = Archive::openForRead(backend, tmp.path(), 0);
        ASSERT_TRUE(segment);
        EXPECT_EQ(segment->fetchMeta(offsetOf(indexes[1]), meta), RecordStatus::kDamaged);
    }
}

TEST_F(SegmentTest, ReadingAPayloadPicksUpTheNextHeaderInTheSameCall) {
    std::vector<INDEX_TYPE> indexes;
    {
        std::unique_ptr<Segment> segment = acquire();
        indexes = append(*segment, {"first", "second"});
    }
    std::unique_ptr<Segment> segment = Archive::openForRead(backend, tmp.path(), 0);
    ASSERT_TRUE(segment);

    format::RecordMeta head{};
    ASSERT_EQ(segment->fetchMeta(offsetOf(indexes[0]), head), RecordStatus::kOk);

    std::string value;
    std::optional<format::RecordMeta> next;
    EXPECT_EQ(segment->readPayload(offsetOf(indexes[0]), head, value, next),
              RecordStatus::kOk);
    EXPECT_EQ(value, "first");
    ASSERT_TRUE(next.has_value()) << "the next header rides along with the payload read";
    EXPECT_EQ(next->sequence, 1u);

    // The last record has nothing behind it.
    const format::RecordMeta second = *next;
    EXPECT_EQ(segment->readPayload(offsetOf(indexes[1]), second, value, next), RecordStatus::kOk);
    EXPECT_EQ(value, "second");
    EXPECT_FALSE(next.has_value());
}

TEST_F(SegmentTest, ReadingADamagedPayloadIsReportedSeparately) {
    std::vector<INDEX_TYPE> indexes;
    {
        std::unique_ptr<Segment> segment = acquire();
        indexes = append(*segment, {"first", "second"});
    }
    test::flipBit(segmentZero(), offsetOf(indexes[0]) + format::kMetaSize);

    std::unique_ptr<Segment> segment = Archive::openForRead(backend, tmp.path(), 0);
    ASSERT_TRUE(segment);
    format::RecordMeta head{};
    ASSERT_EQ(segment->fetchMeta(offsetOf(indexes[0]), head), RecordStatus::kOk);

    std::string value;
    std::optional<format::RecordMeta> next;
    EXPECT_EQ(segment->readPayload(offsetOf(indexes[0]), head, value, next),
              RecordStatus::kDamaged);
    EXPECT_TRUE(next.has_value());

    // The last record has nothing after it to vouch for the segment, which
    // used to make this case indistinguishable from a write cut short -- an
    // acknowledged record quietly demoted to "never written", with nothing
    // logged. The published frontier is past it, so it is damage.
    test::flipBit(segmentZero(), offsetOf(indexes[1]) + format::kMetaSize);
    ASSERT_EQ(segment->fetchMeta(offsetOf(indexes[1]), head), RecordStatus::kOk);
    EXPECT_EQ(segment->readPayload(offsetOf(indexes[1]), head, value, next),
              RecordStatus::kDamaged);
    // Nothing follows it, so there is no prefetched header to hand back; the
    // caller advances by the length this record's trusted meta_crc vouches for.
    EXPECT_FALSE(next.has_value());
}

// A payload cut short past the frontier was never acknowledged, so it is the
// tail of the file and not something to report as lost.
TEST_F(SegmentTest, APayloadCutShortAboveTheFrontierIsTheTail) {
    std::vector<INDEX_TYPE> indexes;
    std::uint64_t tail = 0;
    {
        std::unique_ptr<Segment> segment = acquire();
        indexes = append(*segment, {"first"});
        tail = segment->writeOffset();
    }
    // A header for a record whose payload never landed, written past the
    // frontier exactly as a torn append would leave it.
    format::RecordMeta meta{};
    meta.magic = format::kRecordMagic;
    meta.length = 64;
    meta.timestamp_ns = 1;
    meta.sequence = 1;
    meta.data_crc = 0;
    meta.meta_crc = format::computeMetaCrc(meta);
    test::writeRaw(segmentZero(), tail, &meta, sizeof(meta));

    std::unique_ptr<Segment> segment = Archive::openForRead(backend, tmp.path(), 0);
    ASSERT_TRUE(segment);
    format::RecordMeta head{};
    ASSERT_EQ(segment->fetchMeta(tail, head), RecordStatus::kOk);
    std::string value;
    std::optional<format::RecordMeta> next;
    EXPECT_EQ(segment->readPayload(tail, head, value, next), RecordStatus::kMissing);
}

// A random read guesses how long the record will be so it can fetch the
// header and the payload in one call. Every one of these cases has to come
// back with the right bytes whatever the guess happened to be.
TEST_F(SegmentTest, SpeculationLearnsTheRecordSizeAndStaysCorrect) {
    std::vector<std::string> values = {
        std::string(500, 'a'),
        std::string(500, 'b'),
        std::string(500, 'c'),
    };
    std::vector<INDEX_TYPE> indexes;
    {
        std::unique_ptr<Segment> segment = acquire();
        indexes = append(*segment, values);
    }
    std::unique_ptr<Segment> segment = Archive::openForRead(backend, tmp.path(), 0);
    ASSERT_TRUE(segment);
    EXPECT_EQ(segment->speculatedLength(), 0u) << "nothing to go on before the first read";

    EXPECT_EQ(segment->readRecord(offsetOf(indexes[0])), values[0]);
    EXPECT_EQ(segment->speculatedLength(), 500u) << "the first read is what teaches it";
    EXPECT_EQ(segment->readRecord(offsetOf(indexes[1])), values[1]);
    EXPECT_EQ(segment->readRecord(offsetOf(indexes[2])), values[2]);
}

TEST_F(SegmentTest, SpeculationSurvivesRecordsOfEveryOtherSize) {
    // Deliberately jagged: each read guesses the size of the one before it, so
    // every read here guesses wrong in one direction or the other.
    const std::vector<std::size_t> sizes = {0, 1, 4096, 7, 65536, 0, 300, 1, 100000, 33};
    std::vector<std::string> values;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        values.push_back(test::makePayload(0, i, sizes[i]));
    }
    std::vector<INDEX_TYPE> indexes;
    {
        std::unique_ptr<Segment> segment = acquire();
        indexes = append(*segment, values);
    }
    std::unique_ptr<Segment> segment = Archive::openForRead(backend, tmp.path(), 0);
    ASSERT_TRUE(segment);

    // Forwards, backwards, then jumping about: whatever order, whatever the
    // guess left over from the read before, the bytes have to be right.
    for (std::size_t i = 0; i < values.size(); ++i) {
        EXPECT_EQ(segment->readRecord(offsetOf(indexes[i])), values[i]) << "forwards " << i;
    }
    for (std::size_t i = values.size(); i-- > 0;) {
        EXPECT_EQ(segment->readRecord(offsetOf(indexes[i])), values[i]) << "backwards " << i;
    }
    for (std::size_t step = 0; step < values.size() * 3; ++step) {
        const std::size_t i = (step * 7) % values.size();
        EXPECT_EQ(segment->readRecord(offsetOf(indexes[i])), values[i]) << "jumping " << i;
    }
}

// Guessing large would mean allocating large for whatever comes next, and
// re-reading a lot when the guess misses. Past a ceiling it stops guessing.
TEST_F(SegmentTest, SpeculationGivesUpOnRecordsThatAreTooLarge) {
    const std::size_t big = 200000;
    std::vector<INDEX_TYPE> indexes;
    {
        std::unique_ptr<Segment> segment = acquire();
        indexes = append(*segment, {test::makePayload(0, 0, 64), test::makePayload(0, 1, big)});
    }
    std::unique_ptr<Segment> segment = Archive::openForRead(backend, tmp.path(), 0);
    ASSERT_TRUE(segment);

    EXPECT_EQ(segment->readRecord(offsetOf(indexes[0])), test::makePayload(0, 0, 64));
    EXPECT_EQ(segment->speculatedLength(), 64u);
    EXPECT_EQ(segment->readRecord(offsetOf(indexes[1])), test::makePayload(0, 1, big));
    EXPECT_EQ(segment->speculatedLength(), 0u) << "too big to be worth guessing";
    // And it picks guessing back up when the records get small again.
    EXPECT_EQ(segment->readRecord(offsetOf(indexes[0])), test::makePayload(0, 0, 64));
    EXPECT_EQ(segment->speculatedLength(), 64u);
}

// The guess runs off the end of the file for the last record, so the read
// comes back short. That is not an error, it is just the end of the file.
TEST_F(SegmentTest, SpeculationReadingPastTheEndOfTheFileIsFine) {
    std::vector<INDEX_TYPE> indexes;
    {
        std::unique_ptr<Segment> segment = acquire();
        // A long record first so the guess is much longer than the last one.
        indexes = append(*segment, {test::makePayload(0, 0, 40000), test::makePayload(0, 1, 10)});
    }
    std::unique_ptr<Segment> segment = Archive::openForRead(backend, tmp.path(), 0);
    ASSERT_TRUE(segment);
    EXPECT_EQ(segment->readRecord(offsetOf(indexes[0])), test::makePayload(0, 0, 40000));
    EXPECT_EQ(segment->speculatedLength(), 40000u);
    EXPECT_EQ(segment->readRecord(offsetOf(indexes[1])), test::makePayload(0, 1, 10))
        << "the guess overran the end of the file by 40 KiB";
}

// Speculation must not let damage through: the checksum is still over the
// payload that actually came back, however it got read.
TEST_F(SegmentTest, SpeculationStillChecksThePayload) {
    std::vector<INDEX_TYPE> indexes;
    {
        std::unique_ptr<Segment> segment = acquire();
        indexes =
            append(*segment, {std::string(500, 'a'), std::string(500, 'b'), std::string(500, 'c')});
    }
    test::flipBit(segmentZero(), offsetOf(indexes[1]) + format::kMetaSize + 200);

    std::unique_ptr<Segment> segment = Archive::openForRead(backend, tmp.path(), 0);
    ASSERT_TRUE(segment);
    EXPECT_EQ(segment->readRecord(offsetOf(indexes[0])), std::string(500, 'a'));
    EXPECT_EQ(segment->readRecord(offsetOf(indexes[1])), std::nullopt)
        << "guessed right, still bad";
    EXPECT_EQ(segment->readRecord(offsetOf(indexes[2])), std::string(500, 'c'));
}

TEST_F(SegmentTest, AFileThatIsNotASegmentIsIgnored) {
    Archive::create(backend, tmp.path());
    const std::string path = Archive::segmentPath(tmp.path(), 0);
    {
        io::OpenFlags flags;
        flags.create = true;
        const int fd = backend.open(path, flags);
        backend.close(fd);
    }
    const std::string junk(format::kHeaderSize, 'j');
    test::writeRaw(path, 0, junk.data(), junk.size());

    EXPECT_EQ(Archive::openForRead(backend, tmp.path(), 0), nullptr);
    // A writer leaves it alone and takes the next segment instead.
    std::unique_ptr<Segment> segment = Archive::acquireForWrite(backend, tmp.path(), options);
    ASSERT_TRUE(segment);
    EXPECT_EQ(segment->id(), 1u);
}

TEST_F(SegmentTest, WritersTakeSeparateSegments) {
    Archive::create(backend, tmp.path());
    std::unique_ptr<Segment> first = Archive::acquireForWrite(backend, tmp.path(), options);
    std::unique_ptr<Segment> second = Archive::acquireForWrite(backend, tmp.path(), options);
    std::unique_ptr<Segment> third = Archive::acquireForWrite(backend, tmp.path(), options);
    ASSERT_TRUE(first && second && third);
    EXPECT_EQ(first->id(), 0u);
    EXPECT_EQ(second->id(), 1u);
    EXPECT_EQ(third->id(), 2u);

    // Releasing one puts it back in circulation.
    const std::uint32_t freed = second->id();
    second.reset();
    std::unique_ptr<Segment> fourth = Archive::acquireForWrite(backend, tmp.path(), options);
    EXPECT_EQ(fourth->id(), freed);
}

TEST_F(SegmentTest, AFullSegmentIsLeftAlone) {
    Archive::create(backend, tmp.path());
    {
        std::unique_ptr<Segment> segment = Archive::acquireForWrite(backend, tmp.path(), options);
        append(*segment, {std::string(400, 'x')});
    }
    Options small = options;
    small.maxSegmentBytes = format::kHeaderSize + 256;
    std::unique_ptr<Segment> segment = Archive::acquireForWrite(backend, tmp.path(), small);
    ASSERT_TRUE(segment);
    EXPECT_EQ(segment->id(), 1u) << "segment 0 is past the limit, so a new one is taken";
}

}  // namespace
}  // namespace raf
