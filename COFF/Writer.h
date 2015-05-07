//===- Writer.h -----------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include <vector>
#include <memory>

namespace llvm {
namespace object {
class COFFObjectFile;
}
}

namespace lld {
namespace coff {

void write(std::vector<std::unique_ptr<llvm::object::COFFObjectFile>> &Files);

} // namespace pecoff
} // namespace lld
