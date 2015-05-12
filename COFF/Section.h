//===- Section.h ----------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_SECTION_H
#define LLD_COFF_SECTION_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/FileOutputBuffer.h"

namespace llvm {
template <typename T> class ArrayRef;
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
    : File(F), Header(S), Name(N) {}

  uint64_t getSectionSize() const { return File->getSectionSize(Header); }
  llvm::ArrayRef<uint8_t> getContent() const;

  llvm::object::COFFObjectFile *File;
  const llvm::object::coff_section *Header;
  llvm::StringRef Name;
  uint64_t FileOffset = 0;
  uint64_t RVA = 0;
};

} // namespace pecoff
} // namespace lld

#endif
