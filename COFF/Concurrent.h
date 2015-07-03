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

namespace lld {
namespace coff {

void async(std::function<void()> F);

} // namespace coff
} // namespace lld

#endif
