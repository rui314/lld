//===- Writer.h -----------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_WRITER_H
#define LLD_COFF_WRITER_H

#include "llvm/ADT/StringRef.h"
#include <memory>
#include <vector>

namespace llvm {
namespace object {
class COFFObjectFile;
struct coff_section;
}
}

namespace lld {
namespace coff {

class Section {
public:
 Section(llvm::object::COFFObjectFile *F,
	 const llvm::object::coff_section *S, llvm::StringRef N)
    : File(F), Sec(S), Name(N) {}
  llvm::object::COFFObjectFile *File;
  const llvm::object::coff_section *Sec;
  llvm::StringRef Name;
};

typedef std::vector<std::unique_ptr<Section>> SectionList;

void write(llvm::StringRef OutputPath, SectionList &Files);

} // namespace pecoff
} // namespace lld

#endif
