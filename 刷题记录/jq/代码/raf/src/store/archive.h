// Store layer: the directory of segment files that makes up one archive.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <raf/raf.h>

#include "io/io_backend.h"
#include "store/segment.h"

namespace raf::store {

// An archive is a directory. One segment file per live writer is what lets
// several writers append at once without any of them waiting on the others --
// the "concurrent data writes v2" arrangement from the design doc.
class Archive {
public:
    static std::string segmentPath(const std::string& dir, std::uint32_t id);
    static std::optional<std::uint32_t> parseSegmentName(std::string_view name);

    // Segment ids present in the directory, ascending.
    static std::vector<std::uint32_t> listSegments(io::IoBackend& backend, const std::string& dir);

    static void create(io::IoBackend& backend, const std::string& dir);
    static void requireExists(io::IoBackend& backend, const std::string& dir);

    // Takes exclusive ownership of a segment: the first existing one nobody
    // else holds and that has at least `min_free` bytes of room left,
    // otherwise a new one. The returned segment has been recovered and is
    // positioned to append. Passing the size of the record about to be written
    // is what keeps a writer that is rolling over from picking up another
    // segment it would have to roll straight out of again.
    static std::unique_ptr<Segment> acquireForWrite(io::IoBackend& backend, const std::string& dir,
                                                    const Options& options,
                                                    std::uint64_t min_free = format::kMetaSize);

    // `direct` opens the segment O_DIRECT; see Options::directReads.
    static std::unique_ptr<Segment> openForRead(io::IoBackend& backend, const std::string& dir,
                                                std::uint32_t id, bool direct = true);
};

}  // namespace raf::store
