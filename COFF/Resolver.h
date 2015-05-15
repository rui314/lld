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
  std::error_code addFile(ObjectFile *File);
  std::error_code addFile(ArchiveFile *Archive);
  bool reportRemainingUndefines();
  std::vector<std::unique_ptr<COFFObjectFile>> &getFiles() { return Files; }

private:
  std::error_code resolve(SymbolRef *Ref, Symbol *Sym);
  ErrorOr<SymbolRef *> createSymbol(COFFObjectFile *Obj, COFFSymbolRef Sym);

  std::vector<std::unique_ptr<ObjectFile>> Files;
  std::vector<std::unique_ptr<ARchiveFile>> Archives;
  mutable BumpPtrAllocator Alloc;
  SymbolTable Symtab;
};

} // namespace pecoff
} // namespace lld

#endif
