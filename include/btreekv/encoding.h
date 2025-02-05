#pragma once

#include <cstdint>
#include <cstring>

// Fixed-width integer access into raw page bytes. Pages are byte arrays, so
// fields are read and written with memcpy rather than through casts, which
// keeps unaligned access well-defined. Values are stored in host byte order;
// database files are not portable between hosts of different endianness.

namespace btreekv {

inline uint16_t LoadU16(const char* p) {
  uint16_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

inline uint32_t LoadU32(const char* p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

inline uint64_t LoadU64(const char* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

inline void StoreU16(char* p, uint16_t v) { std::memcpy(p, &v, sizeof(v)); }
inline void StoreU32(char* p, uint32_t v) { std::memcpy(p, &v, sizeof(v)); }
inline void StoreU64(char* p, uint64_t v) { std::memcpy(p, &v, sizeof(v)); }

}  // namespace btreekv
