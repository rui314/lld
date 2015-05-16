//===- Resolver.h ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_RESOLVER_H
#define LLD_COFF_RESOLVER_H

#include "Symbol.h"

namespace lld {
namespace coff {

class Resolver {
public:
  std::error_code addFile(std::unique_ptr<ObjectFile> File);
  std::error_code addFile(std::unique_ptr<ArchiveFile> Archive);
  bool reportRemainingUndefines();
  std::vector<std::unique_ptr<ObjectFile>> &getFiles() {
    return Files;
  }

  uint64_t getRVA(StringRef Symbol);

private:
  std::error_code resolve(SymbolRef *Ref, Symbol *Sym);
  std::error_code addMemberFile(CanBeDefined *Sym);
  Symbol *createSymbol(ObjectFile *Obj, COFFSymbolRef Sym);

  std::vector<std::unique_ptr<ObjectFile>> Files;
  std::vector<std::unique_ptr<ArchiveFile>> Archives;
  mutable llvm::BumpPtrAllocator Alloc;
  SymbolTable Symtab;
};

} // namespace pecoff
} // namespace lld

#endif
