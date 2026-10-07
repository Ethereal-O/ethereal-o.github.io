#include <raf/raf.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "io/io_backend.h"
#include "common/log.h"
#include "store/archive.h"
#include "store/format.h"
#include "store/segment.h"

namespace raf {
namespace {

// Where a cursor is. Four states, and no combination of them: the flags this
// replaced spelled sixteen, of which a handful could happen and nothing said
// which.
//
// A payload that has not landed yet lands here as kStalled, the same as a
// header that is not there: an append that has not returned has not written
// anything the reader promises to show, so the prefetched header is dropped
// and the segment is simply out of records for now. A retry fetches it
// again, which costs one header read on a cursor that had nothing to give.
enum class CursorState {
    kNeedsHeader,  // nothing prefetched; `head` means nothing
    kReady,        // `head` describes the record at `offset`
    kStalled,      // nothing more to be had until the segment grows
    kDone,         // damaged: this segment contributes nothing further
};

// Iteration state for one segment. Every cursor keeps one record header
// prefetched, which is what the merge compares to decide who goes next; the
// payload read that follows picks up the next header in the same call.
struct Cursor {
    store::Segment* segment = nullptr;
    std::uint64_t offset = format::kHeaderSize;
    // Size of the segment when this reader opened it. Records that end at or
    // before this were already complete then, and every reader that opened on
    // the same sizes walks them in the same order.
    std::uint64_t frontier = 0;
    // Meaningful in kReady, and in no other state.
    format::RecordMeta head{};
    CursorState state = CursorState::kNeedsHeader;
    // Sliding window over this segment, under O_DIRECT only. It belongs to
    // the cursor rather than the segment because iteration is where the
    // locality is, and because a segment is read by several threads at once
    // while a cursor is not.
    std::unique_ptr<store::ReadWindow> window;

    std::uint64_t headEnd() const { return offset + format::recordSpan(head.length); }
    bool headInFrontier() const { return headEnd() <= frontier; }

    // Past this record, with `next` as the header that came along with the
    // payload read -- empty when the read did not reach one, which leaves
    // the next header to be fetched on its own.
    void advance(const std::optional<format::RecordMeta>& next) {
        offset += format::recordSpan(head.length);
        if (next) {
            head = *next;
            state = CursorState::kReady;
        } else {
            state = CursorState::kNeedsHeader;
        }
    }
};

}  // namespace

// Everything below this line runs single threaded. Not one of these methods
// takes a lock; the four public methods in RecordReader take them all, which
// is what makes the locking auditable by reading forty lines rather than
// three hundred. The ordering those forty lines keep: iter_mutex before
// table_mutex, never the other way.
struct RecordReader::Impl {
    Impl(const std::string& path, const Options& opts)
        : backend(io::defaultBackend()), dir(path), options(opts) {
        store::Archive::requireExists(backend, dir);
        adoptNewSegments();
        initial_segment_count = segments.size();
        syncCursors();
    }

    io::IoBackend& backend;
    std::string dir;
    Options options;

    // Guards the segment table. Random reads only need this one, so they never
    // queue behind an iteration in progress.
    std::shared_mutex table_mutex;
    std::vector<std::unique_ptr<store::Segment>> segments;
    std::unordered_map<std::uint32_t, store::Segment*> by_id;

    // Ordered before table_mutex. Cursors are only ever touched under it, and
    // so are the Segment objects they point at, which is why iteration can use
    // those pointers without holding the table lock.
    std::mutex iter_mutex;
    std::vector<Cursor> cursors;
    std::size_t initial_segment_count = 0;
    INDEX_TYPE current = kInvalidIndex;

    std::atomic<bool> closed{false};

    void ensureOpen() const {
        if (closed) {
            throw std::logic_error("raf: reader is closed");
        }
    }

    store::Segment* find(std::uint32_t id) const {
        const auto found = by_id.find(id);
        return found == by_id.end() ? nullptr : found->second;
    }

    // Picks up segments created since the last look. Writers that started
    // after this reader did show up here. Caller holds table_mutex.
    void adoptNewSegments() {
        for (const std::uint32_t id : store::Archive::listSegments(backend, dir)) {
            if (by_id.count(id) != 0) {
                continue;
            }
            std::unique_ptr<store::Segment> segment =
                store::Archive::openForRead(backend, dir, id, options.directReads);
            if (!segment) {
                continue;
            }
            by_id.emplace(id, segment.get());
            segments.push_back(std::move(segment));
        }
    }

    // Adds a cursor for every segment that does not have one. New segments
    // start with an empty frontier: nothing in them was complete when this
    // reader opened, so nothing in them is covered by the ordering guarantee.
    // Caller holds table_mutex and iter_mutex.
    void syncCursors() {
        for (std::size_t i = cursors.size(); i < segments.size(); ++i) {
            Cursor cursor;
            cursor.segment = segments[i].get();
            cursor.frontier =
                (i < initial_segment_count) ? segments[i]->refreshSize() : format::kHeaderSize;
            if (options.directReads) {
                cursor.window = std::make_unique<store::ReadWindow>();
            }
            cursors.push_back(std::move(cursor));
        }
    }

    // Gives every stalled cursor another chance, now that the segments have
    // been looked at again.
    void revive() {
        for (Cursor& cursor : cursors) {
            if (cursor.state == CursorState::kDone) {
                continue;
            }
            cursor.segment->refreshSize();
            if (cursor.state == CursorState::kStalled) {
                cursor.state = CursorState::kNeedsHeader;
            }
        }
    }

    // Prefetches the header at a cursor's offset, which is what the merge
    // compares. Only ever called on a cursor that has none.
    void fetchHead(Cursor& cursor) {
        switch (cursor.segment->fetchMeta(cursor.offset, cursor.head, cursor.window.get())) {
            case store::RecordStatus::kOk:
                cursor.state = CursorState::kReady;
                break;
            case store::RecordStatus::kMissing:
                cursor.state = CursorState::kStalled;
                break;
            case store::RecordStatus::kDamaged:
                RAF_LOG_ERROR("stopping iteration of {} at offset {}: damaged record header",
                              cursor.segment->path(), cursor.offset);
                cursor.state = CursorState::kDone;
                break;
        }
    }

    void refill() {
        for (Cursor& cursor : cursors) {
            if (cursor.state == CursorState::kNeedsHeader) {
                fetchHead(cursor);
            }
        }
    }

    // Records that were already on disk when the reader opened go first, all of
    // them, before anything written since. That is what makes two readers that
    // opened on the same segment sizes agree on the order. Within a group the
    // merge key is the append timestamp, with the segment id and offset
    // breaking ties so the order is total.
    Cursor* select() {
        Cursor* best = nullptr;
        bool best_in_frontier = false;
        for (Cursor& cursor : cursors) {
            if (cursor.state != CursorState::kReady) {
                continue;
            }
            const bool in_frontier = cursor.headInFrontier();
            if (best == nullptr || (in_frontier && !best_in_frontier)) {
                best = &cursor;
                best_in_frontier = in_frontier;
                continue;
            }
            if (best_in_frontier && !in_frontier) {
                continue;
            }
            const auto key = [](const Cursor& c) {
                return std::tuple(c.head.timestamp_ns, c.segment->id(), c.offset);
            };
            if (key(cursor) < key(*best)) {
                best = &cursor;
            }
        }
        return best;
    }

    // The next record, or nothing when every cursor has run out of what it
    // can reach without another look at the directory. Caller holds
    // iter_mutex.
    std::optional<std::string> advance() {
        // Once, for whoever is missing a header. After this the only cursor
        // that can need one is the one that just gave up its own.
        refill();

        while (Cursor* cursor = select()) {
            std::string value;
            std::optional<format::RecordMeta> next;
            const store::RecordStatus status = cursor->segment->readPayload(
                cursor->offset, cursor->head, value, next, cursor->window.get());

            if (status == store::RecordStatus::kMissing) {
                // The header is there and the payload is not, so the append
                // that would have made this record durable never returned.
                // Nothing was promised about it, so treat it as unwritten:
                // this segment has no more records for now, and the other
                // cursors carry on. Nobody's header changed, so there is
                // nothing to fetch before selecting again.
                cursor->state = CursorState::kStalled;
                continue;
            }

            const std::uint64_t record_offset = cursor->offset;
            // May leave this cursor without a header: the payload read picks
            // the next one up, but only when it reached that far.
            cursor->advance(next);
            if (cursor->state == CursorState::kNeedsHeader) {
                fetchHead(*cursor);
            }

            if (status == store::RecordStatus::kDamaged) {
                RAF_LOG_ERROR("skipping damaged record in {} at offset {}",
                              cursor->segment->path(), record_offset);
                continue;
            }

            current = format::makeIndex(cursor->segment->id(), record_offset);
            return value;  // moved into the optional, not copied
        }
        return std::nullopt;
    }

    // Caller holds iter_mutex.
    void rewindCursors() {
        for (Cursor& cursor : cursors) {
            cursor.offset = format::kHeaderSize;
            cursor.frontier = cursor.segment->refreshSize();
            cursor.state = CursorState::kNeedsHeader;
        }
        current = kInvalidIndex;
    }

    // Caller holds both.
    void release() {
        closed = true;
        cursors.clear();
        by_id.clear();
        segments.clear();
    }
};

RecordReader::RecordReader(const std::string& filepath)
    : impl_(std::make_unique<Impl>(filepath, Options{})) {}

RecordReader::RecordReader(const std::string& filepath, const Options& options)
    : impl_(std::make_unique<Impl>(filepath, options)) {}

RecordReader::RecordReader(RecordReader&&) noexcept = default;
RecordReader& RecordReader::operator=(RecordReader&&) noexcept = default;
RecordReader::~RecordReader() = default;

std::optional<std::string> RecordReader::read(const INDEX_TYPE& index) {
    impl_->ensureOpen();
    const std::uint32_t segment_id = segmentOf(index);
    const std::uint64_t offset = offsetOf(index);

    {
        // Held across the read so that a concurrent close() cannot pull the
        // segment out from under it.
        std::shared_lock<std::shared_mutex> guard(impl_->table_mutex);
        if (store::Segment* segment = impl_->find(segment_id)) {
            return segment->readRecord(offset);
        }
    }

    // The index may name a segment a writer created after we opened.
    {
        std::unique_lock<std::shared_mutex> guard(impl_->table_mutex);
        impl_->adoptNewSegments();
    }

    std::shared_lock<std::shared_mutex> guard(impl_->table_mutex);
    if (store::Segment* segment = impl_->find(segment_id)) {
        return segment->readRecord(offset);
    }
    return std::nullopt;
}

std::optional<std::string> RecordReader::readNext() {
    impl_->ensureOpen();
    std::lock_guard<std::mutex> guard(impl_->iter_mutex);

    if (std::optional<std::string> value = impl_->advance()) {
        return value;
    }
    // Everything reachable is used up. Look again -- segments grow, and new
    // ones appear -- and give every stalled cursor one more turn. Once.
    {
        std::unique_lock<std::shared_mutex> table(impl_->table_mutex);
        impl_->adoptNewSegments();
        impl_->syncCursors();
    }
    impl_->revive();
    return impl_->advance();
}

INDEX_TYPE RecordReader::currentIndex() const {
    std::lock_guard<std::mutex> guard(impl_->iter_mutex);
    return impl_->current;
}

void RecordReader::rewind() {
    impl_->ensureOpen();
    std::lock_guard<std::mutex> guard(impl_->iter_mutex);
    // Re-snapshot the frontier so that a rewind behaves like reopening: the
    // records that exist now are the ones covered by the ordering guarantee.
    impl_->rewindCursors();
}

void RecordReader::close() {
    std::lock_guard<std::mutex> guard(impl_->iter_mutex);
    std::unique_lock<std::shared_mutex> table(impl_->table_mutex);
    if (impl_->closed) {
        return;
    }
    impl_->release();
}

}  // namespace raf
