#pragma once

#include <cstddef>
#include <string>

namespace raf::detail {

// Grows `s` to `n` bytes that the caller is about to overwrite. resize() would
// zero them first; C++23 gives us a way to skip that, and payloads here run to
// megabytes.
inline void resizeUninitialized(std::string& s, std::size_t n) {
#if defined(__cpp_lib_string_resize_and_overwrite)
    s.resize_and_overwrite(n, [](char*, std::size_t size) { return size; });
#else
    s.resize(n);
#endif
}

}  // namespace raf::detail
