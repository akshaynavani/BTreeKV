#include "btreekv/btree.h"

#include <stdexcept>
#include <utility>

#include "btreekv/internal_node.h"
#include "btreekv/leaf_node.h"
#include "btreekv/meta_page.h"
#include "btreekv/slotted_page.h"

namespace btreekv {

namespace {

// Node views take a mutable pointer; read paths only call their const
// members on pages held under a read latch.
char* Bytes(const ReadPageGuard& guard) { return const_cast<char*>(guard.data()); }

bool IsLeaf(const char* data) { return SlottedPage(const_cast<char*>(data)).type() == PageType::kLeaf; }

}  // namespace

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

}  // namespace btreekv
