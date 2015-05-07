//===- Writer.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Writer.h"
#include "llvm/Object/COFF.h"

using llvm::object::COFFObjectFile;

namespace lld {
namespace coff {

void write(std::vector<std::unique_ptr<COFFObjectFile>> &Files) {
}

} // namespace coff
} // namespace lld
