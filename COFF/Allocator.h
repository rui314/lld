//===- Alloc.h ------------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_ALLOCATOR_H
#define LLD_COFF_ALLOCATOR_H

#include "lld/Core/LLVM.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Allocator.h"
#include <mutex>
#include <memory>

namespace lld {
namespace coff {

class StringAllocator {
public:
  StringRef save(StringRef S) {
    std::lock_guard<std::mutex> Lock(Mutex);
    char *P = Alloc.Allocate<char>(S.size() + 1);
    memcpy(P, S.data(), S.size());
    P[S.size()] = '\0';
    return StringRef(P, S.size());
  }

  StringRef save(Twine S) { return save(StringRef(S.str())); }
  StringRef save(const char *S) { return save(StringRef(S)); }

private:
  llvm::BumpPtrAllocator Alloc;
  std::mutex Mutex;
};

} // namespace coff
} // namespace lld

#endif
