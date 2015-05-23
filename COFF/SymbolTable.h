//===- SymbolTable.h ------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_SYMBOL_TABLE_H
#define LLD_COFF_SYMBOL_TABLE_H

#include "InputFiles.h"
#include "Memory.h"
#include "llvm/Support/Allocator.h"
#include <unordered_map>

namespace lld {
namespace coff {

class SymbolTable {
public:
  SymbolTable();

  std::error_code addFile(std::unique_ptr<InputFile> File);
  bool reportRemainingUndefines();

  std::vector<Chunk *> getChunks();

  Symbol *find(StringRef Name);
  void dump();

  std::vector<std::unique_ptr<ImportFile>> ImportFiles;

private:
  std::error_code addFile(ObjectFile *File);
  std::error_code addFile(ArchiveFile *File);
  std::error_code addFile(ImportFile *File);

  std::error_code resolve(Symbol *Sym, SymbolRef **Ref);
  std::error_code addMemberFile(CanBeDefined *Sym);
  void addInitialSymbol(Symbol *Sym);

  std::unordered_map<llvm::StringRef, SymbolRef *> Symtab;
  std::vector<std::unique_ptr<ObjectFile>> ObjectFiles;
  std::vector<std::unique_ptr<ArchiveFile>> ArchiveFiles;
  std::vector<std::unique_ptr<Symbol>> OwnedSymbols;
  llvm::BumpPtrAllocator Alloc;
  StringAllocator StringAlloc;
};

} // namespace pecoff
} // namespace lld

#endif
