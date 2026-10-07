// Injected failures.
//
// The failures the filesystem will not produce on demand: a write that lands
// halfway and then reports EIO, an fsync that fails, a directory sync that
// leaves no trace to assert on. Everything here drives the store layer
// directly, because that is the level IoBackend is injectable at.
//
// One property is behind all of it: a failure must not let a piece of a
// record pass for a whole one, and must not leave the writer unable to go on.
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <raf/raf.h>

#include "store/archive.h"
#include "store/format.h"
#include "store/segment.h"

#include "fault_backend.h"
#include "test_support.h"

namespace raf {
namespace {

using store::Archive;
using store::ScanResult;
using store::Segment;

class FaultTest : public ::testing::Test {
protected:
    test::FaultBackend backend{io::posixBackend()};
    test::TempArchive tmp{"raf-faults"};
    Options options;

    FaultTest() { options.sync = false; }

    std::unique_ptr<Segment> acquire() {
        Archive::create(backend, tmp.path());
        return Archive::acquireForWrite(backend, tmp.path(), options);
    }

    std::vector<INDEX_TYPE> append(Segment& segment, const std::vector<std::string>& values) {
        std::vector<std::string_view> views(values.begin(), values.end());
        std::vector<INDEX_TYPE> indexes(values.size());
        store::AppendScratch scratch;
        segment.appendRecords(views.data(), views.size(), indexes.data(), scratch);
        return indexes;
    }

    std::string segmentZero() const { return Archive::segmentPath(tmp.path(), 0); }
};

// The write lands halfway and then fails. What is on disk at that moment is
// a header with no payload behind it, which is exactly the shape a reader
// must never accept -- and the append path cuts it off rather than leaving
// recovery to work it out.
TEST_F(FaultTest, AWriteThatFailsHalfwayLeavesNoPieceOfARecord) {
    std::unique_ptr<Segment> segment = acquire();
    const std::string good = test::makePayload(0, 0, 512);
    append(*segment, {good, good});
    const std::uint64_t after_two = segment->writeOffset();

    backend.writes_until_fault = 0;
    backend.partial_bytes = format::kMetaSize + 100;  // a header and part of its payload
    EXPECT_THROW(append(*segment, {test::makePayload(0, 2, 512)}), std::system_error);
    EXPECT_EQ(backend.faulted_writes, 1);

    EXPECT_EQ(segment->writeOffset(), after_two) << "a failed append must not move the offset";
    EXPECT_EQ(test::fileSize(segmentZero()), after_two)
        << "the debris must be cut off, not left for a reader to reason about";
}

// Not wedged: the same segment keeps taking records once the failure clears,
// and they come back with the two that preceded it.
TEST_F(FaultTest, AWriterCarriesOnAfterAFailedWrite) {
    std::unique_ptr<Segment> segment = acquire();
    const std::string first = test::makePayload(0, 0, 256);
    append(*segment, {first});

    backend.writes_until_fault = 0;
    backend.partial_bytes = 16;
    EXPECT_THROW(append(*segment, {test::makePayload(0, 1, 256)}), std::system_error);

    const std::string after = test::makePayload(0, 2, 256);
    ASSERT_NO_THROW(append(*segment, {after}));
    segment.reset();

    EXPECT_EQ(test::drain(tmp.path()), (std::vector<std::string>{first, after}));
}

// The record the failed write was meant to carry never gets an index that
// resolves, and the next one does not reuse its sequence number.
TEST_F(FaultTest, AFailedAppendHandsOutNothingAReaderWillHonour) {
    std::unique_ptr<Segment> segment = acquire();
    append(*segment, {test::makePayload(0, 0, 128)});

    backend.writes_until_fault = 0;
    backend.partial_bytes = format::kMetaSize;  // the header alone, payload missing
    EXPECT_THROW(append(*segment, {test::makePayload(0, 1, 128)}), std::system_error);

    const INDEX_TYPE reused = append(*segment, {test::makePayload(0, 2, 128)}).front();
    segment.reset();

    RecordReader reader(tmp.path());
    EXPECT_EQ(reader.read(reused), test::makePayload(0, 2, 128))
        << "the offset the failed record would have had belongs to the next one";
    EXPECT_EQ(test::drain(tmp.path()).size(), 2u);
}

// Recovery after a failure that was not cleaned up. The rollback in the
// append path is best effort -- it can itself fail -- so a scan has to reach
// the same verdict on its own.
TEST_F(FaultTest, RecoveryDropsAPartialRecordTheRollbackCouldNotRemove) {
    const std::string good = test::makePayload(0, 0, 512);
    {
        std::unique_ptr<Segment> segment = acquire();
        append(*segment, {good});
    }

    // Write a header with no payload behind it, the way a torn write leaves
    // the file when nothing cuts it back.
    const std::uint64_t torn_at = format::kHeaderSize + format::recordSpan(512);
    format::RecordMeta orphan{};
    orphan.magic = format::kRecordMagic;
    orphan.length = 512;
    orphan.timestamp_ns = 1;
    orphan.sequence = 1;
    orphan.data_crc = 0;
    orphan.meta_crc = format::computeMetaCrc(orphan);
    test::writeRaw(segmentZero(), torn_at, &orphan, sizeof(orphan));

    std::unique_ptr<Segment> reopened =
        Archive::acquireForWrite(io::posixBackend(), tmp.path(), options);
    ASSERT_TRUE(reopened);
    EXPECT_EQ(reopened->writeOffset(), torn_at) << "the torn header is not a record";
    reopened.reset();

    EXPECT_EQ(test::drain(tmp.path()), std::vector<std::string>{good});
}

// A segment whose header could not be made durable must not be handed back
// as a usable one. The alternative is an archive holding a file that has no
// readable header and no records, which every later scan has to special-case.
TEST_F(FaultTest, ASegmentWhoseHeaderCannotBeSyncedIsNotHandedBack) {
    Archive::create(backend, tmp.path());
    backend.fail_fsync = true;
    EXPECT_THROW(Archive::acquireForWrite(backend, tmp.path(), options), std::system_error);
}

// A new segment only counts once its directory entry is durable: a crash in
// between would leave records that no longer have a name. Nothing a reader
// can do reveals whether the sync happened, so this is the only way to pin
// it.
TEST_F(FaultTest, ANewSegmentSyncsTheDirectoryItWasCreatedIn) {
    Archive::create(backend, tmp.path());
    backend.directory_syncs.clear();

    std::unique_ptr<Segment> segment = Archive::acquireForWrite(backend, tmp.path(), options);
    ASSERT_TRUE(segment);
    EXPECT_EQ(backend.directory_syncs, std::vector<std::string>{tmp.path()});

    // Reopening an existing segment creates no entry, so it syncs nothing.
    append(*segment, {test::makePayload(0, 0, 64)});
    segment.reset();
    backend.directory_syncs.clear();
    segment = Archive::acquireForWrite(backend, tmp.path(), options);
    ASSERT_TRUE(segment);
    EXPECT_TRUE(backend.directory_syncs.empty());
}

// The frontier is a diagnostic, not data. An append that cannot publish it
// still has to succeed -- the cost of losing it is that a reader calls
// damage "end of data", which is a worse answer, not a wrong one.
TEST_F(FaultTest, AnAppendSurvivesAFrontierThatCannotBeWritten) {
    std::unique_ptr<Segment> segment = acquire();
    backend.fail_writes_at = format::kFrontierOffset;

    std::vector<std::string> written;
    for (int i = 0; i < 8; ++i) {
        written.push_back(test::makePayload(0, i, 256));
        ASSERT_NO_THROW(append(*segment, {written.back()}));
    }
    segment.reset();

    EXPECT_GE(backend.faulted_writes, 1) << "the frontier write should have been attempted";
    EXPECT_LE(backend.faulted_writes, 1)
        << "and given up on for good, not retried on every append";
    EXPECT_EQ(test::drain(tmp.path()), written);
}

}  // namespace
}  // namespace raf
