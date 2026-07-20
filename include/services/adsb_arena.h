#pragma once

// Fixed, caller-owned bounded allocator used to back the ArduinoJson per-object
// decoder. It never touches the global heap: every request is satisfied from a
// caller-provided byte buffer, and exhaustion is reported as a null pointer so
// ArduinoJson maps it to DeserializationError::NoMemory. This header is
// Arduino-free and ArduinoJson-free so the allocator can be unit-tested in
// isolation.

#include <cstddef>

namespace services::adsb {

// First-fit free-list allocator with boundary coalescing over one contiguous
// buffer. allocate/deallocate/reallocate follow malloc/free/realloc semantics,
// including a realloc that leaves the original block intact when it cannot
// satisfy the new size.
class BoundedArena {
 public:
  BoundedArena(void* buffer, size_t size);

  void* allocate(size_t size);
  void deallocate(void* ptr);
  void* reallocate(void* ptr, size_t new_size);

  // Return the arena to a single free block.
  void reset();

  size_t capacity() const { return usable_; }
  size_t bytesInUse() const;
  size_t peakBytesInUse() const { return peak_in_use_; }

 private:
  struct BlockHeader;
  static const size_t kHeaderSize;

  BlockHeader* firstBlock() const;
  BlockHeader* nextBlock(BlockHeader* block) const;
  bool inArena(const BlockHeader* block) const;
  void coalesceFreeBlocks();
  void sampleUsage();

  unsigned char* base_;
  size_t usable_;
  size_t peak_in_use_;
};

}  // namespace services::adsb
