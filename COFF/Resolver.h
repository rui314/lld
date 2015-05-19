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

#include "Reader.h"

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

  std::vector<DefinedImplib *> ImpSyms;

private:
  std::error_code resolve(StringRef Name, Symbol *Sym);
  std::error_code addMemberFile(CanBeDefined *Sym);

  std::vector<std::unique_ptr<ObjectFile>> Files;
  std::vector<std::unique_ptr<ArchiveFile>> Archives;
  SymbolTable Symtab;
};

} // namespace pecoff
} // namespace lld

#endif
