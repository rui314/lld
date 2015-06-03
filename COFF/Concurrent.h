//===- Concurrent.h -------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_CONCURRENT_H
#define LLD_COFF_CONCURRENT_H

#include <functional>

#ifdef _MSC_VER
#  include <atomic>
#  include <eh.h>
#  include <concrt.h>
#  include <concurrent_unordered_map.h>
#  include <concurrent_unordered_set.h>
#  include <concurrent_vector.h>
#  include <ppl.h>
#else
#  include <unordered_map>
#  include <unordered_set>
#  include <vector>
#endif

namespace lld {
namespace coff {

#ifdef _MSC_VER

class TaskGroup {
public:
  void run(std::function<void()> F) { G.run(F); }
  void wait() { G.wait(); }
private:
  concurrency::task_group G;
};

template<typename T, typename U>
using ConcMap = concurrency::concurrent_unordered_map<T, U>;
template<typename T> using ConcSet = concurrency::concurrent_unordered_set<T>;
template<typename T> using ConcVector = concurrency::concurrent_vector<T>;

#else

class TaskGroup {
public:
  void run(std::function<void()> F) { F(); }
  void wait() {}
};

template<class T, typename U> using ConcMap = std::unordered_map<T, U>;
template<class T> using ConcSet = std::unordered_set<T>;
template<class T> using ConcVector = std::vector<T>;

#endif

} // namespace coff
} // namespace lld

#endif
