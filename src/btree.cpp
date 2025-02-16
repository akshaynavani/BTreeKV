#include "btreekv/btree.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "btreekv/internal_node.h"
#include "btreekv/leaf_node.h"
#include "btreekv/meta_page.h"
#include "btreekv/slotted_page.h"

namespace btreekv {

namespace {

// A leaf left with fewer used bytes than this after a delete tries to merge
// with its right sibling.
constexpr size_t kMergeThreshold = SlottedPage::kCapacity / 4;

// Node views take a mutable pointer; read paths only call their const
// members on pages held under a read latch.
char* Bytes(const ReadPageGuard& guard) { return const_cast<char*>(guard.data()); }

bool IsLeaf(const char* data) { return SlottedPage(const_cast<char*>(data)).type() == PageType::kLeaf; }

void ValidateEntry(std::string_view key, std::string_view value) {
  if (key.size() > kMaxKeySize) {
    throw std::invalid_argument("key is " + std::to_string(key.size()) + " bytes; the limit is " +
                                std::to_string(kMaxKeySize));
  }
  if (value.size() > kMaxValueSize) {
    throw std::invalid_argument("value is " + std::to_string(value.size()) + " bytes; the limit is " +
                                std::to_string(kMaxValueSize));
  }
}

// Picks a split point for an overflowing node whose entries (in key order)
// have the given byte footprints. Entries [0, k) stay in the left node and
// entries [k + gap, n) move to the right node; with gap == 1 the entry at k is
// pushed up to the parent (internal split), with gap == 0 nothing is skipped
// (leaf split). Splits by bytes rather than by count, choosing the k that
// minimises the fuller half, so variable-length entries always fit.
size_t ChooseSplit(const std::vector<size_t>& sizes, size_t gap) {
  const size_t n = sizes.size();
  std::vector<size_t> prefix(n + 1, 0);
  for (size_t i = 0; i < n; ++i) {
    prefix[i + 1] = prefix[i] + sizes[i];
  }

  size_t best = 0;
  size_t best_cost = SIZE_MAX;
  for (size_t k = 1; k + gap < n; ++k) {
    const size_t left = prefix[k];
    const size_t right = prefix[n] - prefix[k + gap];
    if (left > SlottedPage::kCapacity || right > SlottedPage::kCapacity) {
      continue;
    }
    const size_t cost = std::max(left, right);
    if (cost < best_cost) {
      best_cost = cost;
      best = k;
    }
  }
  if (best == 0) {
    throw std::logic_error("no split point leaves both halves within a page");
  }
  return best;
}

}  // namespace

// The pages a writer currently holds, top-down. `path` is a contiguous chain:
// path[i + 1] is child number child_index[i] of path[i]. If root_guard is
// engaged, path[0] is the root and the root pointer is latched exclusively.
struct BTree::WriteContext {
  std::optional<WriteLatchGuard> root_guard;
  std::vector<WritePageGuard> path;
  std::vector<size_t> child_index;

  // Called when the newest page in `path` is safe: nothing above it can
  // change any more, so let readers back in.
  void ReleaseAncestors() {
    root_guard.reset();
    if (path.size() > 1) {
      WritePageGuard last = std::move(path.back());
      path.clear();
      path.push_back(std::move(last));
    }
    child_index.clear();
  }
};

BTree::BTree(const std::string& path, const BTreeOptions& options) {
  if (options.pool_frames < kMinPoolFrames) {
    throw std::invalid_argument("pool_frames must be at least " + std::to_string(kMinPoolFrames));
  }
  disk_ = std::make_unique<DiskManager>(path);
  pool_ = std::make_unique<BufferPool>(disk_.get(), options.pool_frames);
  if (disk_->PageCount() == 0) {
    Bootstrap();
  } else {
    LoadMeta();
  }
}

BTree::~BTree() {
  try {
    Flush();
  } catch (...) {
    // Nothing sensible to do from a destructor.
  }
}

void BTree::Bootstrap() {
  const page_id_t meta = disk_->AllocatePage();
  if (meta != kMetaPageId) {
    throw std::logic_error("meta page must be page 0");
  }
  const page_id_t root = disk_->AllocatePage();
  {
    WritePageGuard guard = pool_->NewWrite(root);
    LeafNode(guard.data()).Init(root);
  }
  root_page_id_ = root;
  free_list_head_ = kInvalidPageId;
  height_.store(1);
  num_entries_.store(0);

  WriteMeta();
  pool_->FlushAll();
}

void BTree::LoadMeta() {
  MetaPage meta;
  {
    ReadPageGuard guard = pool_->FetchRead(kMetaPageId);
    meta = MetaPage::ReadFrom(guard.data());
  }
  if (meta.page_count > disk_->PageCount()) {
    disk_->SetPageCount(meta.page_count);
  }
  if (meta.root_page_id == kMetaPageId || meta.root_page_id >= disk_->PageCount()) {
    throw std::runtime_error("corrupt meta page: root page id out of range");
  }
  if (meta.tree_height == 0) {
    throw std::runtime_error("corrupt meta page: zero tree height");
  }
  root_page_id_ = meta.root_page_id;
  free_list_head_ = meta.free_list_head;
  height_.store(meta.tree_height);
  num_entries_.store(meta.num_entries);
}

void BTree::WriteMeta() {
  MetaPage meta;
  meta.root_page_id = root_page_id_;
  meta.free_list_head = free_list_head_;
  meta.page_count = disk_->PageCount();
  meta.tree_height = height_.load();
  meta.num_entries = num_entries_.load();

  WritePageGuard guard = pool_->FetchWrite(kMetaPageId);
  meta.WriteTo(guard.data());
  guard.MarkDirty();
}

void BTree::Flush() {
  std::lock_guard<std::mutex> writer(writer_mu_);
  WriteMeta();
  pool_->FlushAll();
}

// ---- Reads -----------------------------------------------------------------

ReadPageGuard BTree::DescendForRead(std::string_view key, bool leftmost) const {
  ReadPageGuard guard;
  {
    // Latch the root page before letting go of the root pointer, so a writer
    // cannot swap in a new root between reading the id and latching it.
    ReadLatchGuard root_guard(root_latch_);
    guard = pool_->FetchRead(root_page_id_);
  }

  while (!IsLeaf(guard.data())) {
    const InternalNode node(Bytes(guard));
    const page_id_t child = leftmost ? node.ChildAt(0) : node.ChildFor(key);
    // Crab: latch the child while still holding the parent, then release
    // the parent by overwriting the guard.
    ReadPageGuard child_guard = pool_->FetchRead(child);
    guard = std::move(child_guard);
  }
  return guard;
}

bool BTree::Get(std::string_view key, std::string* value) const {
  if (key.size() > kMaxKeySize) {
    return false;  // could never have been stored
  }
  ReadPageGuard guard = DescendForRead(key, false);
  const LeafNode leaf(Bytes(guard));
  const std::optional<uint16_t> slot = leaf.Find(key);
  if (!slot) {
    return false;
  }
  if (value != nullptr) {
    value->assign(leaf.ValueAt(*slot));
  }
  return true;
}

std::optional<std::string> BTree::Get(std::string_view key) const {
  std::string value;
  if (!Get(key, &value)) {
    return std::nullopt;
  }
  return value;
}

// ---- Introspection ---------------------------------------------------------

size_t BTree::FreeListLength() const {
  std::lock_guard<std::mutex> writer(writer_mu_);
  size_t length = 0;
  page_id_t id = free_list_head_;
  while (id != kInvalidPageId) {
    if (++length > disk_->PageCount()) {
      throw std::logic_error("free list contains a cycle");
    }
    ReadPageGuard guard = pool_->FetchRead(id);
    id = SlottedPage(Bytes(guard)).link();
  }
  return length;
}

int BTree::CheckSubtree(page_id_t id, const std::optional<std::string>& lo, const std::optional<std::string>& hi,
                        std::vector<page_id_t>* leaves, uint64_t* entries) const {
  auto fail = [id](const std::string& what) {
    throw std::logic_error("page " + std::to_string(id) + ": " + what);
  };

  ReadPageGuard guard = pool_->FetchRead(id);
  const SlottedPage page(Bytes(guard));
  if (page.page_id() != id) {
    fail("header records page id " + std::to_string(page.page_id()));
  }
  if (page.type() != PageType::kLeaf && page.type() != PageType::kInternal) {
    fail("reachable page is neither a leaf nor an internal node");
  }

  for (uint16_t i = 0; i < page.num_slots(); ++i) {
    const std::string_view key = page.KeyAt(i);
    if (i > 0 && !(page.KeyAt(static_cast<uint16_t>(i - 1)) < key)) {
      fail("keys out of order at slot " + std::to_string(i));
    }
    if ((lo && key < std::string_view(*lo)) || (hi && key >= std::string_view(*hi))) {
      fail("key at slot " + std::to_string(i) + " escapes its separator bounds");
    }
  }

  if (page.type() == PageType::kLeaf) {
    if (page.level() != 0) {
      fail("leaf with non-zero level");
    }
    leaves->push_back(id);
    *entries += page.num_slots();
    return 0;
  }

  const InternalNode node(Bytes(guard));
  if (node.level() == 0) {
    fail("internal node at level 0");
  }
  for (size_t c = 0; c < node.num_children(); ++c) {
    const std::optional<std::string> child_lo =
        c == 0 ? lo : std::optional<std::string>(std::string(node.KeyAt(static_cast<uint16_t>(c - 1))));
    const std::optional<std::string> child_hi =
        c == node.num_keys() ? hi : std::optional<std::string>(std::string(node.KeyAt(static_cast<uint16_t>(c))));
    const int child_level = CheckSubtree(node.ChildAt(c), child_lo, child_hi, leaves, entries);
    if (child_level + 1 != node.level()) {
      fail("child " + std::to_string(c) + " is at level " + std::to_string(child_level));
    }
  }
  return node.level();
}

void BTree::CheckInvariants() const {
  std::lock_guard<std::mutex> writer(writer_mu_);

  page_id_t root;
  {
    ReadLatchGuard root_guard(root_latch_);
    root = root_page_id_;
  }

  std::vector<page_id_t> leaves;
  uint64_t entries = 0;
  const int root_level = CheckSubtree(root, std::nullopt, std::nullopt, &leaves, &entries);
  if (static_cast<uint32_t>(root_level) + 1 != Height()) {
    throw std::logic_error("root level " + std::to_string(root_level) + " disagrees with height " +
                           std::to_string(Height()));
  }
  if (entries != Size()) {
    throw std::logic_error("leaves hold " + std::to_string(entries) + " entries but the tree counts " +
                           std::to_string(Size()));
  }

  // The next links must thread the leaves in exactly in-order sequence.
  page_id_t id = leaves.front();
  for (size_t i = 0; i < leaves.size(); ++i) {
    if (id != leaves[i]) {
      throw std::logic_error("leaf chain reaches page " + std::to_string(id) + " where page " +
                             std::to_string(leaves[i]) + " was expected");
    }
    ReadPageGuard guard = pool_->FetchRead(id);
    id = LeafNode(Bytes(guard)).next_leaf();
  }
  if (id != kInvalidPageId) {
    throw std::logic_error("leaf chain continues past the last leaf to page " + std::to_string(id));
  }
}

// ---- Writes ----------------------------------------------------------------

bool BTree::IsSafe(WritePageGuard& guard, std::string_view key, std::string_view value, WriteOp op) const {
  SlottedPage page(guard.data());
  if (page.type() == PageType::kLeaf) {
    if (op == WriteOp::kInsert) {
      // Even an overwrite is judged by the full new entry; being
      // conservative here only means holding the parent a little longer.
      return page.FreeSpace() >= SlottedPage::EntrySize(key.size(), value.size());
    }
    // Delete: safe unless removing the key would push the leaf under the
    // merge threshold. A missing key changes nothing at all.
    LeafNode leaf(guard.data());
    const std::optional<uint16_t> slot = leaf.Find(key);
    if (!slot) {
      return true;
    }
    const size_t freed = SlottedPage::EntrySize(key.size(), leaf.ValueAt(*slot).size());
    return leaf.UsedBytes() - freed >= kMergeThreshold;
  }

  InternalNode node(guard.data());
  if (op == WriteOp::kInsert) {
    return node.HasRoomForSeparator();
  }
  // Delete: a child merge removes one separator from this node. That only
  // matters for a root down to its last separator (it would collapse), but
  // treating every one-separator node as unsafe keeps the rule uniform.
  return node.num_keys() > 1;
}

void BTree::DescendForWrite(std::string_view key, std::string_view value, WriteOp op, WriteContext* ctx) {
  ctx->path.reserve(Height() + 1);
  ctx->child_index.reserve(Height());

  ctx->root_guard.emplace(root_latch_);
  WritePageGuard root = pool_->FetchWrite(root_page_id_);
  const bool root_safe = IsSafe(root, key, value, op);
  ctx->path.push_back(std::move(root));
  if (root_safe) {
    ctx->root_guard.reset();
  }

  while (!IsLeaf(ctx->path.back().data())) {
    InternalNode node(ctx->path.back().data());
    const uint16_t index = node.ChildIndexFor(key);
    WritePageGuard child = pool_->FetchWrite(node.ChildAt(index));
    const bool safe = IsSafe(child, key, value, op);
    ctx->child_index.push_back(index);
    ctx->path.push_back(std::move(child));
    if (safe) {
      ctx->ReleaseAncestors();
    }
  }
}

WritePageGuard BTree::AllocatePage() {
  page_id_t id;
  if (free_list_head_ != kInvalidPageId) {
    id = free_list_head_;
    ReadPageGuard guard = pool_->FetchRead(id);
    const SlottedPage page(Bytes(guard));
    if (page.type() != PageType::kFree) {
      throw std::runtime_error("free list points at page " + std::to_string(id) + " which is not free");
    }
    free_list_head_ = page.link();
  } else {
    id = disk_->AllocatePage();
  }
  return pool_->NewWrite(id);
}

void BTree::FreePage(WritePageGuard guard) {
  const page_id_t id = guard.page_id();
  SlottedPage page(guard.data());
  page.Init(PageType::kFree, id, 0);
  page.set_link(free_list_head_);
  free_list_head_ = id;
  guard.MarkDirty();
}

void BTree::SplitLeaf(WritePageGuard& leaf_guard, std::string_view key, std::string_view value,
                      std::string* separator, page_id_t* right_id) {
  LeafNode leaf(leaf_guard.data());

  // Materialize the leaf's contents with the new entry merged in.
  std::vector<std::pair<std::string, std::string>> entries;
  entries.reserve(static_cast<size_t>(leaf.size()) + 1);
  bool placed = false;
  for (uint16_t i = 0; i < leaf.size(); ++i) {
    const std::string_view k = leaf.KeyAt(i);
    if (!placed && key <= k) {
      entries.emplace_back(std::string(key), std::string(value));
      placed = true;
      if (k == key) {
        continue;  // overwrite: drop the old value
      }
    }
    entries.emplace_back(std::string(k), std::string(leaf.ValueAt(i)));
  }
  if (!placed) {
    entries.emplace_back(std::string(key), std::string(value));
  }

  std::vector<size_t> sizes;
  sizes.reserve(entries.size());
  for (const auto& e : entries) {
    sizes.push_back(SlottedPage::EntrySize(e.first.size(), e.second.size()));
  }
  const size_t split = ChooseSplit(sizes, 0);

  // The new right sibling takes over the old next link. Nobody can reach it
  // until we release this leaf (via the next link) or the parent (via the
  // separator), and we hold both.
  WritePageGuard right_guard = AllocatePage();
  LeafNode right(right_guard.data());
  right.Init(right_guard.page_id());
  right.set_next_leaf(leaf.next_leaf());
  for (size_t i = split; i < entries.size(); ++i) {
    if (!right.Append(entries[i].first, entries[i].second)) {
      throw std::logic_error("right half of a leaf split does not fit");
    }
  }

  leaf.Init(leaf_guard.page_id());
  leaf.set_next_leaf(right_guard.page_id());
  for (size_t i = 0; i < split; ++i) {
    if (!leaf.Append(entries[i].first, entries[i].second)) {
      throw std::logic_error("left half of a leaf split does not fit");
    }
  }
  leaf_guard.MarkDirty();
  right_guard.MarkDirty();

  // B+tree copy-up: the right leaf's first key becomes the separator.
  *separator = entries[split].first;
  *right_id = right_guard.page_id();
}

void BTree::SplitInternal(WritePageGuard& node_guard, uint16_t pos, const std::string& key, page_id_t child,
                          std::string* separator, page_id_t* right_id) {
  InternalNode node(node_guard.data());
  const uint8_t level = node.level();

  std::vector<std::string> keys;
  std::vector<page_id_t> children;
  keys.reserve(static_cast<size_t>(node.num_keys()) + 1);
  children.reserve(node.num_children() + 1);
  children.push_back(node.ChildAt(0));
  for (uint16_t i = 0; i < node.num_keys(); ++i) {
    keys.emplace_back(node.KeyAt(i));
    children.push_back(node.ChildAt(static_cast<size_t>(i) + 1));
  }
  keys.insert(keys.begin() + pos, key);
  children.insert(children.begin() + pos + 1, child);

  std::vector<size_t> sizes;
  sizes.reserve(keys.size());
  for (const std::string& k : keys) {
    sizes.push_back(SlottedPage::EntrySize(k.size(), InternalNode::kChildSize));
  }
  // keys[mid] moves up; its right child becomes the new node's leftmost.
  const size_t mid = ChooseSplit(sizes, 1);

  WritePageGuard right_guard = AllocatePage();
  InternalNode right(right_guard.data());
  right.Init(right_guard.page_id(), level, children[mid + 1]);
  for (size_t i = mid + 1; i < keys.size(); ++i) {
    if (!right.InsertAt(right.num_keys(), keys[i], children[i + 1])) {
      throw std::logic_error("right half of an internal split does not fit");
    }
  }

  node.Init(node_guard.page_id(), level, children[0]);
  for (size_t i = 0; i < mid; ++i) {
    if (!node.InsertAt(node.num_keys(), keys[i], children[i + 1])) {
      throw std::logic_error("left half of an internal split does not fit");
    }
  }
  node_guard.MarkDirty();
  right_guard.MarkDirty();

  *separator = keys[mid];
  *right_id = right_guard.page_id();
}

void BTree::InsertIntoParent(WriteContext* ctx, size_t index, std::string separator, page_id_t right_id) {
  while (true) {
    if (index == 0) {
      // The node that split is the top of our latched path. It must be the
      // root, or the safety checks released an ancestor they should not have.
      WritePageGuard& old_root = ctx->path[0];
      if (!ctx->root_guard || old_root.page_id() != root_page_id_) {
        throw std::logic_error("split propagated above the latched path");
      }
      const uint8_t level = SlottedPage(old_root.data()).level();
      WritePageGuard new_root = AllocatePage();
      InternalNode root(new_root.data());
      root.Init(new_root.page_id(), static_cast<uint8_t>(level + 1), old_root.page_id());
      if (!root.InsertAt(0, separator, right_id)) {
        throw std::logic_error("separator does not fit in an empty root");
      }
      new_root.MarkDirty();
      root_page_id_ = new_root.page_id();
      height_.fetch_add(1);
      return;
    }

    WritePageGuard& parent_guard = ctx->path[index - 1];
    InternalNode parent(parent_guard.data());
    const auto pos = static_cast<uint16_t>(ctx->child_index[index - 1]);
    if (parent.InsertAt(pos, separator, right_id)) {
      parent_guard.MarkDirty();
      return;
    }

    std::string up;
    page_id_t new_right;
    SplitInternal(parent_guard, pos, separator, right_id, &up, &new_right);
    separator = std::move(up);
    right_id = new_right;
    --index;
  }
}

bool BTree::Put(std::string_view key, std::string_view value) {
  ValidateEntry(key, value);
  std::lock_guard<std::mutex> writer(writer_mu_);

  WriteContext ctx;
  DescendForWrite(key, value, WriteOp::kInsert, &ctx);
  WritePageGuard& leaf_guard = ctx.path.back();
  LeafNode leaf(leaf_guard.data());

  switch (leaf.Upsert(key, value)) {
    case LeafNode::UpsertResult::kInserted:
      leaf_guard.MarkDirty();
      num_entries_.fetch_add(1, std::memory_order_relaxed);
      return true;
    case LeafNode::UpsertResult::kUpdated:
      leaf_guard.MarkDirty();
      return false;
    case LeafNode::UpsertResult::kNoSpace:
      break;
  }

  const bool existed = leaf.Find(key).has_value();
  std::string separator;
  page_id_t right_id;
  SplitLeaf(leaf_guard, key, value, &separator, &right_id);
  InsertIntoParent(&ctx, ctx.path.size() - 1, std::move(separator), right_id);
  if (!existed) {
    num_entries_.fetch_add(1, std::memory_order_relaxed);
  }
  return !existed;
}

void BTree::MergeWithRightSibling(WriteContext* ctx) {
  const size_t leaf_index = ctx->path.size() - 1;
  WritePageGuard& parent_guard = ctx->path[leaf_index - 1];
  WritePageGuard& leaf_guard = ctx->path[leaf_index];
  InternalNode parent(parent_guard.data());
  const size_t child = ctx->child_index[leaf_index - 1];

  // Only merge rightward, within the same parent. Latching a left sibling
  // would go against the left-to-right order scans use and could deadlock.
  if (child >= parent.num_keys()) {
    return;
  }

  WritePageGuard sibling_guard = pool_->FetchWrite(parent.ChildAt(child + 1));
  LeafNode leaf(leaf_guard.data());
  LeafNode sibling(sibling_guard.data());
  if (leaf.UsedBytes() + sibling.UsedBytes() > SlottedPage::kCapacity) {
    return;
  }

  // Every key in the sibling sorts after every key in this leaf.
  for (uint16_t i = 0; i < sibling.size(); ++i) {
    if (!leaf.Append(sibling.KeyAt(i), sibling.ValueAt(i))) {
      throw std::logic_error("leaf merge overflowed after size check");
    }
  }
  leaf.set_next_leaf(sibling.next_leaf());
  leaf_guard.MarkDirty();

  // Separator `child` sits between this leaf and the sibling.
  parent.RemoveAt(static_cast<uint16_t>(child));
  parent_guard.MarkDirty();
  FreePage(std::move(sibling_guard));

  if (parent.num_keys() == 0 && ctx->root_guard && parent_guard.page_id() == root_page_id_) {
    // The root has a single child left; make that child the root.
    root_page_id_ = parent.ChildAt(0);
    height_.fetch_sub(1);
    FreePage(std::move(parent_guard));
  }
}

bool BTree::Delete(std::string_view key) {
  if (key.size() > kMaxKeySize) {
    return false;
  }
  std::lock_guard<std::mutex> writer(writer_mu_);

  WriteContext ctx;
  DescendForWrite(key, std::string_view(), WriteOp::kDelete, &ctx);
  WritePageGuard& leaf_guard = ctx.path.back();
  LeafNode leaf(leaf_guard.data());
  if (!leaf.Remove(key)) {
    return false;
  }
  leaf_guard.MarkDirty();
  num_entries_.fetch_sub(1, std::memory_order_relaxed);

  // An unsafe leaf kept its parent latched; a safe one is alone in the path.
  if (ctx.path.size() >= 2 && leaf.UsedBytes() < kMergeThreshold) {
    MergeWithRightSibling(&ctx);
  }
  return true;
}

}  // namespace btreekv
