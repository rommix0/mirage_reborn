#include "alloc_counter.h"

#include <cstdint>
#include <cstdlib>
#include <new>

namespace {

// Constant-initialised, so touching them never allocates.
thread_local bool g_counting = false;
thread_local size_t g_allocations = 0;

void* allocate(std::size_t size) noexcept {
  if (g_counting) ++g_allocations;
  return std::malloc(size ? size : 1);
}

void* allocateAligned(std::size_t size, std::align_val_t align) noexcept {
  if (g_counting) ++g_allocations;
  if (!size) size = 1;
#ifdef _MSC_VER
  return _aligned_malloc(size, std::size_t(align));
#else
  // aligned_alloc wants a size that is a multiple of the alignment.
  const std::size_t a = std::size_t(align);
  return std::aligned_alloc(a, (size + a - 1) / a * a);
#endif
}

void release(void* p) noexcept { std::free(p); }

void releaseAligned(void* p) noexcept {
#ifdef _MSC_VER
  _aligned_free(p);
#else
  std::free(p);
#endif
}

}  // namespace

namespace mirage_test {

AllocationScope::AllocationScope() : start_(g_allocations), wasActive_(g_counting) {
  g_counting = true;
}
AllocationScope::~AllocationScope() { g_counting = wasActive_; }
size_t AllocationScope::count() const { return g_allocations - start_; }

}  // namespace mirage_test

// The whole global allocation family, so every allocation pairs with its
// matching deallocation.
void* operator new(std::size_t n) {
  if (void* p = allocate(n)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  if (void* p = allocate(n)) return p;
  throw std::bad_alloc();
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return allocate(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return allocate(n); }
void* operator new(std::size_t n, std::align_val_t a) {
  if (void* p = allocateAligned(n, a)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t a) {
  if (void* p = allocateAligned(n, a)) return p;
  throw std::bad_alloc();
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return allocateAligned(n, a);
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return allocateAligned(n, a);
}

void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, std::size_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t) noexcept { release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete(void* p, std::align_val_t) noexcept { releaseAligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { releaseAligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { releaseAligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { releaseAligned(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
  releaseAligned(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
  releaseAligned(p);
}
