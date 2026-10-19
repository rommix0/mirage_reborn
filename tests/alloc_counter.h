#pragma once
// Counts heap allocations made by the constructing thread. alloc_counter.cpp
// replaces the global operator new/delete family for the whole test
// executable (malloc/free underneath, a thread_local flag on top), so every
// allocation in the process goes through it and only the flagged thread's
// calls are counted. Use it to prove that a code path never allocates:
//
//   AllocationScope scope;
//   ...code under test...
//   CHECK(scope.count() == 0);
//
// Scopes nest; each counts the allocations made while it was alive.
#include <cstddef>

namespace mirage_test {

struct AllocationScope {
  AllocationScope();
  ~AllocationScope();
  AllocationScope(const AllocationScope&) = delete;
  AllocationScope& operator=(const AllocationScope&) = delete;
  // Calls to global operator new (any form) on this thread since construction.
  size_t count() const;

 private:
  size_t start_;
  bool wasActive_;
};

}  // namespace mirage_test
