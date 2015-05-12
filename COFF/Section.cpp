//===- Section.cpp --------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Section.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::object;

namespace lld {
namespace coff {

ArrayRef<uint8_t> Section::getContent() const {
  ArrayRef<uint8_t> Res;
  if (auto EC = File->getSectionContents(Header, Res))
    llvm::errs() << "getSectionContents failed: " << EC.message() << "\n";
  return Res;
}

} // namespace coff
} // namespace lld
