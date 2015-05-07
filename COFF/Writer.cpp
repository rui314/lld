//===- Writer.cpp ---------------------------------------------------------===// 
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Writer.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using llvm::object::COFFObjectFile;
using llvm::object::coff_section;

namespace lld {
namespace coff {

void write(std::vector<std::unique_ptr<COFFObjectFile>> &Files) {
  if (Files.empty())
    return;
  COFFObjectFile *File = Files[0].get();
  (void)File->getMachine();
  for (const auto &SectionRef : File->sections()) {
    const coff_section *Sec = File->getCOFFSection(SectionRef);
    StringRef Name;
    if (auto EC = File->getSectionName(Sec, Name)) {
      llvm::errs() << "Failed to get a section name: " << EC.message() << "\n";
      return;
    }
    llvm::dbgs() << Name << "\n";
  }
}

} // namespace coff
} // namespace lld
