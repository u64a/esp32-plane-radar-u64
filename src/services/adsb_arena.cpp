#include "services/adsb_arena.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace services::adsb {

namespace {

constexpr size_t kAlign = alignof(max_align_t);

size_t alignUp(size_t value, size_t alignment) {
  return (value + (alignment - 1)) & ~(alignment - 1);
}

}  // namespace

struct BoundedArena::BlockHeader {
  size_t size;  // payload bytes, always a multiple of kAlign
  size_t used;  // 0 = free, 1 = allocated
};

// Bytes reserved for each block header, padded so the payload that follows is
// aligned for any scalar ArduinoJson stores.
const size_t BoundedArena::kHeaderSize = alignUp(sizeof(BoundedArena::BlockHeader), kAlign);

BoundedArena::BoundedArena(void* buffer, size_t size)
    : base_(nullptr), usable_(0), peak_in_use_(0) {
  auto raw = reinterpret_cast<uintptr_t>(buffer);
  const uintptr_t aligned = (raw + (kAlign - 1)) & ~(static_cast<uintptr_t>(kAlign) - 1);
  const size_t adjust = static_cast<size_t>(aligned - raw);
  if (buffer == nullptr || size <= adjust + kHeaderSize) {
    return;
  }
  base_ = reinterpret_cast<unsigned char*>(aligned);
  usable_ = size - adjust;
  reset();
}

void BoundedArena::reset() {
  if (base_ == nullptr) {
    return;
  }
  auto* head = reinterpret_cast<BlockHeader*>(base_);
  head->size = usable_ - kHeaderSize;
  head->used = 0;
  peak_in_use_ = 0;
}

BoundedArena::BlockHeader* BoundedArena::firstBlock() const {
  return reinterpret_cast<BlockHeader*>(base_);
}

BoundedArena::BlockHeader* BoundedArena::nextBlock(BlockHeader* block) const {
  auto* raw = reinterpret_cast<unsigned char*>(block) + kHeaderSize + block->size;
  return reinterpret_cast<BlockHeader*>(raw);
}

bool BoundedArena::inArena(const BlockHeader* block) const {
  const auto* raw = reinterpret_cast<const unsigned char*>(block);
  return raw >= base_ && raw < base_ + usable_;
}

void BoundedArena::coalesceFreeBlocks() {
  if (base_ == nullptr) {
    return;
  }
  BlockHeader* block = firstBlock();
  while (inArena(block)) {
    if (block->used == 0) {
      BlockHeader* next = nextBlock(block);
      while (inArena(next) && next->used == 0) {
        block->size += kHeaderSize + next->size;
        next = nextBlock(block);
      }
    }
    block = nextBlock(block);
  }
}

void* BoundedArena::allocate(size_t size) {
  if (base_ == nullptr) {
    return nullptr;
  }
  size_t need = alignUp(size == 0 ? kAlign : size, kAlign);

  BlockHeader* block = firstBlock();
  while (inArena(block)) {
    if (block->used == 0 && block->size >= need) {
      // Split only when the remainder can hold a header plus one aligned unit.
      if (block->size >= need + kHeaderSize + kAlign) {
        auto* raw = reinterpret_cast<unsigned char*>(block) + kHeaderSize + need;
        auto* split = reinterpret_cast<BlockHeader*>(raw);
        split->size = block->size - need - kHeaderSize;
        split->used = 0;
        block->size = need;
      }
      block->used = 1;
      sampleUsage();
      return reinterpret_cast<unsigned char*>(block) + kHeaderSize;
    }
    block = nextBlock(block);
  }
  return nullptr;
}

void BoundedArena::deallocate(void* ptr) {
  if (ptr == nullptr || base_ == nullptr) {
    return;
  }
  auto* block = reinterpret_cast<BlockHeader*>(reinterpret_cast<unsigned char*>(ptr) -
                                               kHeaderSize);
  block->used = 0;
  coalesceFreeBlocks();
}

void* BoundedArena::reallocate(void* ptr, size_t new_size) {
  if (ptr == nullptr) {
    return allocate(new_size);
  }
  if (new_size == 0) {
    deallocate(ptr);
    return nullptr;
  }

  auto* block = reinterpret_cast<BlockHeader*>(reinterpret_cast<unsigned char*>(ptr) -
                                               kHeaderSize);
  const size_t need = alignUp(new_size, kAlign);

  if (block->size >= need) {
    // Shrinking in place: split off the tail when it is large enough to reuse.
    if (block->size >= need + kHeaderSize + kAlign) {
      auto* raw = reinterpret_cast<unsigned char*>(block) + kHeaderSize + need;
      auto* split = reinterpret_cast<BlockHeader*>(raw);
      split->size = block->size - need - kHeaderSize;
      split->used = 0;
      block->size = need;
      coalesceFreeBlocks();
    }
    sampleUsage();
    return ptr;
  }

  // Try to grow into the immediately following free space.
  BlockHeader* next = nextBlock(block);
  if (inArena(next) && next->used == 0 &&
      block->size + kHeaderSize + next->size >= need) {
    block->size += kHeaderSize + next->size;
    if (block->size >= need + kHeaderSize + kAlign) {
      auto* raw = reinterpret_cast<unsigned char*>(block) + kHeaderSize + need;
      auto* split = reinterpret_cast<BlockHeader*>(raw);
      split->size = block->size - need - kHeaderSize;
      split->used = 0;
      block->size = need;
    }
    sampleUsage();
    return ptr;
  }

  // Fall back to a fresh block. The original block is untouched on failure.
  void* fresh = allocate(new_size);
  if (fresh == nullptr) {
    return nullptr;
  }
  std::memcpy(fresh, ptr, block->size < new_size ? block->size : new_size);
  deallocate(ptr);
  return fresh;
}

size_t BoundedArena::bytesInUse() const {
  if (base_ == nullptr) {
    return 0;
  }
  size_t used = 0;
  BlockHeader* block = firstBlock();
  while (inArena(block)) {
    if (block->used != 0) {
      used += kHeaderSize + block->size;
    }
    block = nextBlock(block);
  }
  return used;
}

void BoundedArena::sampleUsage() {
  const size_t used = bytesInUse();
  if (used > peak_in_use_) {
    peak_in_use_ = used;
  }
}

}  // namespace services::adsb
