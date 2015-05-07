//===- Writer.h -----------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringRef.h"
#include <memory>
#include <vector>

namespace llvm {
namespace object {
class COFFObjectFile;
}
}

namespace lld {
namespace coff {

void write(llvm::StringRef OutputPath,
	   std::vector<std::unique_ptr<llvm::object::COFFObjectFile>> &Files);

} // namespace pecoff
} // namespace lld
