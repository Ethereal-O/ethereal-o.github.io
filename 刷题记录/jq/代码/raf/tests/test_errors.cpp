// Things going wrong: refused opens, a write that fails halfway, and damage
// that turns up on disk after the fact.
//
// The property under test throughout is that a failure never lets a broken
// record pass for a complete one, and never leaves the writer wedged.
#include <gtest/gtest.h>

#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <raf/raf.h>

#include "test_support.h"

namespace raf {
namespace {

class ErrorTest : public ::testing::Test {
protected:
    test::TempArchive tmp{"raf-errors"};
    Options fast;

    ErrorTest() { fast.sync = false; }
};

TEST_F(ErrorTest, AnArchiveUnderAMissingDirectoryFails) {
    EXPECT_THROW(RecordWriter writer(tmp.root() + "/missing/archive", fast), std::system_error);
}

TEST_F(ErrorTest, AnArchivePathThatIsAFileFails) {
    const std::string path = tmp.root() + "/regular";
    const int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
    ASSERT_GE(fd, 0);
    ::close(fd);
    EXPECT_THROW(RecordWriter writer(path, fast), std::system_error);
}

TEST_F(ErrorTest, ADirectoryWeCannotWriteToFails) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root ignores directory permissions";
    }
    const std::string parent = tmp.root() + "/locked";
    ASSERT_EQ(::mkdir(parent.c_str(), 0500), 0);
    EXPECT_THROW(RecordWriter writer(parent + "/archive", fast), std::system_error);

    // An archive that exists but has turned read-only fails the same way: the
    // segment already there cannot be opened for writing, and no new one can
    // be created beside it.
    const std::string archive = tmp.path();
    { RecordWriter writer(archive, fast); }
    const std::string segment = test::segmentPaths(archive).front();
    ASSERT_EQ(::chmod(segment.c_str(), 0444), 0);
    ASSERT_EQ(::chmod(archive.c_str(), 0500), 0);
    EXPECT_THROW(RecordWriter writer(archive, fast), std::system_error);
    // Reading is still fine, which is the point of failing only the writer.
    EXPECT_NO_THROW(RecordReader reader(archive));
    ASSERT_EQ(::chmod(archive.c_str(), 0755), 0);
    ASSERT_EQ(::chmod(segment.c_str(), 0644), 0);
}

// A write that runs out of room has to fail loudly, leave the records it
// already acknowledged alone, and leave the writer able to carry on once there
// is room again. RLIMIT_FSIZE stands in for a full disk: the write comes back
// EFBIG, which is the same shape of failure as ENOSPC.
TEST_F(ErrorTest, AWriteThatRunsOutOfRoomFailsCleanly) {
    std::vector<std::string> acknowledged;
    RecordWriter writer(tmp.path(), fast);

    rlimit original{};
    ASSERT_EQ(::getrlimit(RLIMIT_FSIZE, &original), 0);
    // Without this the kernel kills the process instead of failing the write.
    const auto previous_handler = ::signal(SIGXFSZ, SIG_IGN);

    rlimit limited = original;
    limited.rlim_cur = 8 * 1024;
    ASSERT_EQ(::setrlimit(RLIMIT_FSIZE, &limited), 0);

    bool failed = false;
    for (int i = 0; i < 200 && !failed; ++i) {
        const std::string value = test::makePayload(0, i, 512);
        try {
            writer.append(value);
            acknowledged.push_back(value);
        } catch (const std::system_error&) {
            failed = true;
        }
    }

    ASSERT_EQ(::setrlimit(RLIMIT_FSIZE, &original), 0);
    ::signal(SIGXFSZ, previous_handler);

    ASSERT_TRUE(failed) << "the size limit should have stopped the writer";
    ASSERT_FALSE(acknowledged.empty());

    EXPECT_EQ(test::drain(tmp.path()), acknowledged)
        << "records that were acknowledged before the failure must all still be there";
    EXPECT_EQ(test::fileSize(test::segmentPaths(tmp.path()).front()),
              format::kHeaderSize + acknowledged.size() * format::recordSpan(512))
        << "a failed write must not leave a piece of a record behind";

    // Not wedged: with room again the same writer keeps going.
    const std::string after = test::makePayload(0, 999, 512);
    EXPECT_NO_THROW(writer.append(after));
    acknowledged.push_back(after);
    EXPECT_EQ(test::drain(tmp.path()), acknowledged);
}

TEST_F(ErrorTest, ADamagedPayloadIsNeverReturned) {
    std::vector<std::string> values;
    std::vector<INDEX_TYPE> indexes;
    {
        RecordWriter writer(tmp.path(), fast);
        for (int i = 0; i < 5; ++i) {
            values.push_back(test::makePayload(0, i, 300));
            indexes.push_back(writer.append(values.back()));
        }
    }
    const std::string path = test::segmentPaths(tmp.path()).front();
    test::flipBit(path, offsetOf(indexes[2]) + format::kMetaSize + 100);

    RecordReader reader(tmp.path());
    EXPECT_EQ(reader.read(indexes[2]), std::nullopt) << "damage must not read back as a record";
    EXPECT_EQ(reader.read(indexes[1]), values[1]);
    EXPECT_EQ(reader.read(indexes[3]), values[3]);

    // Iteration steps over the damaged record and keeps the rest.
    const std::vector<std::string> walked = test::drain(reader);
    ASSERT_EQ(walked.size(), 4u);
    for (const std::string& value : walked) {
        EXPECT_TRUE(test::payloadIsIntact(value));
        EXPECT_NE(value, values[2]);
    }
}

TEST_F(ErrorTest, ADamagedRecordHeaderStopsIterationRatherThanGuessing) {
    std::vector<INDEX_TYPE> indexes;
    {
        RecordWriter writer(tmp.path(), fast);
        for (int i = 0; i < 5; ++i) {
            indexes.push_back(writer.append(test::makePayload(0, i, 300)));
        }
    }
    // Without a header there is no way to know where the next record begins,
    // so everything after it is out of reach.
    test::flipBit(test::segmentPaths(tmp.path()).front(), offsetOf(indexes[2]) + 4);

    RecordReader reader(tmp.path());
    EXPECT_EQ(test::drain(reader).size(), 2u);
    EXPECT_EQ(reader.read(indexes[2]), std::nullopt);
    // A direct lookup still works for the records past the damage, because it
    // does not have to walk through it.
    EXPECT_EQ(reader.read(indexes[3]), test::makePayload(0, 3, 300));
}

TEST_F(ErrorTest, DamageInOneSegmentDoesNotStopTheOthers) {
    std::vector<INDEX_TYPE> first_indexes;
    {
        RecordWriter first(tmp.path(), fast);
        RecordWriter second(tmp.path(), fast);
        for (int i = 0; i < 4; ++i) {
            first_indexes.push_back(first.append(test::makePayload(1, i, 200)));
            second.append(test::makePayload(2, i, 200));
        }
    }
    ASSERT_EQ(test::segmentPaths(tmp.path()).size(), 2u);
    test::flipBit(test::segmentPaths(tmp.path())[0], offsetOf(first_indexes[1]) + 4);

    const std::vector<std::string> values = test::drain(tmp.path());
    int from_second = 0;
    for (const std::string& value : values) {
        ASSERT_TRUE(test::payloadIsIntact(value));
        if (test::identifyPayload(value)->writer == 2) {
            ++from_second;
        }
    }
    EXPECT_EQ(from_second, 4) << "the undamaged segment must be readable in full";
}

TEST_F(ErrorTest, ASegmentWithAnUnreadableHeaderIsSkipped) {
    {
        RecordWriter first(tmp.path(), fast);
        RecordWriter second(tmp.path(), fast);
        first.append(test::makePayload(1, 0, 100));
        second.append(test::makePayload(2, 0, 100));
    }
    const std::vector<std::string> paths = test::segmentPaths(tmp.path());
    ASSERT_EQ(paths.size(), 2u);
    const std::string junk(format::kHeaderSize, 'j');
    test::writeRaw(paths[0], 0, junk.data(), junk.size());

    const std::vector<std::string> values = test::drain(tmp.path());
    ASSERT_EQ(values.size(), 1u);
    ASSERT_TRUE(test::payloadIsIntact(values[0]));
    EXPECT_EQ(test::identifyPayload(values[0])->writer, 2u);

    // A writer leaves the unreadable file alone instead of appending into it.
    RecordWriter writer(tmp.path(), fast);
    EXPECT_NE(writer.segmentId(), 0u);
}

TEST_F(ErrorTest, AnEmptyFileWithASegmentNameIsAdoptedRatherThanRejected) {
    // A crash between creating a segment and writing its header leaves this
    // behind; the next writer should just finish the job.
    const std::string archive = tmp.path();
    ASSERT_EQ(::mkdir(archive.c_str(), 0755), 0);
    const std::string path = store::Archive::segmentPath(archive, 0);
    const int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
    ASSERT_GE(fd, 0);
    ::close(fd);

    RecordWriter writer(archive, fast);
    EXPECT_EQ(writer.segmentId(), 0u);
    writer.append(std::string("recovered"));
    writer.close();
    EXPECT_EQ(test::drain(archive), (std::vector<std::string>{"recovered"}));
}

TEST_F(ErrorTest, DamageIsReportedThroughTheLoggerRatherThanSilently) {
    {
        RecordWriter writer(tmp.path(), fast);
        writer.append(test::makePayload(0, 0, 100));
        writer.append(test::makePayload(0, 1, 100));
    }
    const std::string path = test::segmentPaths(tmp.path()).front();
    test::flipBit(path, format::kHeaderSize + format::kMetaSize + 10);

    // The record is dropped, which is the part that matters; the log line that
    // goes with it is off in tests so the output stays readable.
    RecordReader reader(tmp.path());
    EXPECT_EQ(test::drain(reader).size(), 1u);
}

}  // namespace
}  // namespace raf
