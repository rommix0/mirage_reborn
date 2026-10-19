#pragma once
// Counts heap allocations and frees made by the constructing thread. alloc_counter.cpp
// replaces the global operator new/delete family for the whole test
// executable (malloc/free underneath, a thread_local flag on top), so every
// allocation in the process goes through it and only the flagged thread's
// calls are counted. Use it to prove that a code path never allocates or frees:
//
//   AllocationScope scope;
//   ...code under test...
//   CHECK(scope.count() == 0);
//   CHECK(scope.frees() == 0);
//
// Scopes nest; each counts the calls made while it was alive.
#include <cstddef>

namespace mirage_test {

struct AllocationScope {
  AllocationScope();
  ~AllocationScope();
  AllocationScope(const AllocationScope&) = delete;
  AllocationScope& operator=(const AllocationScope&) = delete;
  // Calls to global operator new (any form) on this thread since construction.
  size_t count() const;
  // Calls to global operator delete (any form) with a non-null pointer on this
  // thread since construction.
  size_t frees() const;

 private:
  size_t start_;
  size_t freesStart_;
  bool wasActive_;
};

}  // namespace mirage_test
