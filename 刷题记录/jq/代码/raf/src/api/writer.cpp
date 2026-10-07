#include <raf/raf.h>

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>

#include "io/io_backend.h"
#include "common/log.h"
#include "store/archive.h"
#include "store/format.h"
#include "store/segment.h"

namespace raf {
namespace {

// Records per write. Each one contributes two iovecs, which keeps a full batch
// inside the kernel's IOV_MAX and so inside a single write. Larger batches are
// split into runs of this size; every run is durable before the next starts.
constexpr std::size_t kMaxRecordsPerWrite = 512;

}  // namespace

struct RecordWriter::Impl {
    Impl(const std::string& path, const Options& opts)
        : backend(io::defaultBackend()), dir(path), options(opts) {
        if (options.maxSegmentBytes > kMaxSegmentBytes) {
            options.maxSegmentBytes = kMaxSegmentBytes;
        }
        if (options.maxSegmentBytes <= format::kHeaderSize) {
            throw std::logic_error("raf: maxSegmentBytes leaves no room for records");
        }
        if (options.maxRecordBytes == 0) {
            options.maxRecordBytes = kMaxRecordSize;
        }
        if (options.maxRecordBytes > kRecordSizeCeiling) {
            throw std::logic_error("raf: maxRecordBytes of " +
                                   std::to_string(options.maxRecordBytes) + " is over the " +
                                   std::to_string(kRecordSizeCeiling) +
                                   " byte ceiling the record format allows");
        }
        store::Archive::create(backend, dir);
        segment = store::Archive::acquireForWrite(backend, dir, options);
    }

    io::IoBackend& backend;
    std::string dir;
    Options options;

    std::mutex mutex;
    std::unique_ptr<store::Segment> segment;
    bool closed = false;
    store::AppendScratch scratch;
    std::vector<std::string_view> views;

    void ensureOpen() const {
        if (closed) {
            throw std::logic_error("raf: writer is closed");
        }
    }

    void checkLength(std::string_view value) const {
        if (value.size() > options.maxRecordBytes) {
            throw std::logic_error("raf: record of " + std::to_string(value.size()) +
                                   " bytes exceeds the " +
                                   std::to_string(options.maxRecordBytes) + " byte limit");
        }
    }

    // Moves to a segment with room for `needed` bytes. The old one stays
    // locked until the new one is in hand, so the search cannot hand us back
    // the segment we just outgrew.
    void rollOver(std::uint64_t needed) {
        std::unique_ptr<store::Segment> previous = std::move(segment);
        segment = store::Archive::acquireForWrite(backend, dir, options, needed);
        RAF_LOG_DEBUG("writer rolled from segment {} to {}", previous->id(), segment->id());
    }

    void appendAll(const std::string_view* values, std::size_t count, INDEX_TYPE* out_index) {
        std::size_t done = 0;
        while (done < count) {
            const std::uint64_t offset = segment->writeOffset();
            std::uint64_t used = 0;
            std::size_t run = 0;
            while (run < kMaxRecordsPerWrite && done + run < count) {
                const std::uint64_t span =
                    format::recordSpan(static_cast<std::uint32_t>(values[done + run].size()));
                // A segment always takes at least one record, however small
                // maxSegmentBytes is; otherwise nothing would ever be written.
                const bool must_fit = run > 0 || offset > format::kHeaderSize;
                if (must_fit && offset + used + span > options.maxSegmentBytes) {
                    break;
                }
                used += span;
                ++run;
            }
            if (run == 0) {
                rollOver(format::recordSpan(static_cast<std::uint32_t>(values[done].size())));
                continue;
            }
            segment->appendRecords(values + done, run, out_index + done, scratch);
            done += run;
        }
    }
};

RecordWriter::RecordWriter(const std::string& filepath) : RecordWriter(filepath, Options{}) {}

RecordWriter::RecordWriter(const std::string& filepath, const Options& options)
    : impl_(std::make_unique<Impl>(filepath, options)) {}

RecordWriter::RecordWriter(RecordWriter&&) noexcept = default;
RecordWriter& RecordWriter::operator=(RecordWriter&&) noexcept = default;
RecordWriter::~RecordWriter() = default;

INDEX_TYPE RecordWriter::append(const std::string& value) {
    return append(std::string_view(value));
}

INDEX_TYPE RecordWriter::append(std::string_view value) {
    impl_->checkLength(value);
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->ensureOpen();
    INDEX_TYPE index = kInvalidIndex;
    impl_->appendAll(&value, 1, &index);
    return index;
}

std::vector<INDEX_TYPE> RecordWriter::appendBatch(const std::vector<std::string>& values) {
    for (const std::string& value : values) {
        impl_->checkLength(value);
    }
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->ensureOpen();

    // Views, not copies: the payloads go to the kernel out of the caller's own
    // strings.
    impl_->views.assign(values.begin(), values.end());
    std::vector<INDEX_TYPE> indexes(values.size(), kInvalidIndex);
    impl_->appendAll(impl_->views.data(), impl_->views.size(), indexes.data());
    return indexes;
}

std::vector<INDEX_TYPE> RecordWriter::appendBatch(const std::vector<std::string_view>& values) {
    for (const std::string_view value : values) {
        impl_->checkLength(value);
    }
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->ensureOpen();
    std::vector<INDEX_TYPE> indexes(values.size(), kInvalidIndex);
    impl_->appendAll(values.data(), values.size(), indexes.data());
    return indexes;
}

void RecordWriter::close() {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (impl_->closed) {
        return;
    }
    impl_->closed = true;
    impl_->segment.reset();
}

std::uint32_t RecordWriter::segmentId() const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (!impl_->segment) {
        throw std::logic_error("raf: writer is closed");
    }
    return impl_->segment->id();
}

}  // namespace raf
