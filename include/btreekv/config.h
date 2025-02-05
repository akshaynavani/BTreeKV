#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace btreekv {

using page_id_t = uint32_t;
using frame_id_t = uint32_t;

// Every on-disk page and every buffer pool frame is exactly this many bytes.
inline constexpr std::size_t kPageSize = 4096;

inline constexpr page_id_t kInvalidPageId = std::numeric_limits<page_id_t>::max();

// Page 0 of every database file holds the file header (see meta_page.h).
inline constexpr page_id_t kMetaPageId = 0;

// Entry size limits. They are chosen so that a byte-balanced split of an
// overflowing leaf always produces two halves that fit in a page (see
// ChooseSplit in btree.cpp and the static_asserts in leaf_node.h).
inline constexpr std::size_t kMaxKeySize = 256;
inline constexpr std::size_t kMaxValueSize = 1024;

inline constexpr std::size_t kDefaultPoolFrames = 256;  // 1 MiB of cached pages

// A writer may pin its whole root-to-leaf path plus a sibling or a freshly
// split page and a new root; below this the tree cannot make progress.
inline constexpr std::size_t kMinPoolFrames = 8;

}  // namespace btreekv
