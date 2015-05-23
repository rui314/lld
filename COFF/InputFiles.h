//===- InputFiles.h -------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_INPUT_FILES_H
#define LLD_COFF_INPUT_FILES_H

#include "Chunks.h"
#include "Memory.h"
#include "Symbols.h"
#include "lld/Core/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/COFF.h"
#include <memory>
#include <set>
#include <vector>

using llvm::object::Archive;
using llvm::object::COFFObjectFile;

namespace lld {
namespace coff {

class InputFile {
public:
  enum Kind { ArchiveKind, ObjectKind, ImplibKind };
  Kind kind() const { return FileKind; }
  virtual ~InputFile() {}

  virtual StringRef getName() = 0;
  virtual std::vector<std::unique_ptr<SymbolBody>> &getSymbols() = 0;

  std::string getShortName();
  void setParentName(StringRef N) { ParentName = N; }

protected:
  InputFile(Kind K) : FileKind(K) {}

private:
  const Kind FileKind;
  StringRef ParentName;
};

class ArchiveFile : public InputFile {
public:
  static bool classof(const InputFile *F) { return F->kind() == ArchiveKind; }

  static ErrorOr<std::unique_ptr<ArchiveFile>> create(StringRef Path);

  StringRef getName() override { return Name; }
  std::vector<std::unique_ptr<SymbolBody>> &getSymbols() override { return Symbols; }

  std::string Name;
  std::unique_ptr<Archive> File;

  ErrorOr<MemoryBufferRef> getMember(const Archive::Symbol *Sym);

private:
  ArchiveFile(StringRef Name, std::unique_ptr<Archive> File,
              std::unique_ptr<MemoryBuffer> Mem);

  std::unique_ptr<MemoryBuffer> MB;
  std::vector<std::unique_ptr<SymbolBody>> Symbols;
  std::set<const char *> Seen;
};

class ObjectFile : public InputFile {
public:
  static bool classof(const InputFile *F) { return F->kind() == ObjectKind; }

  static ErrorOr<std::unique_ptr<ObjectFile>> create(StringRef Path);
  static ErrorOr<std::unique_ptr<ObjectFile>>
    create(StringRef Path, MemoryBufferRef MB);

  StringRef getName() override { return Name; }
  std::vector<std::unique_ptr<SymbolBody>> &getSymbols() override { return SymbolBodies; }
  StringRef getDirectives() { return Directives; }

  std::string Name;
  std::vector<std::unique_ptr<SymbolBody>> SymbolBodies;
  std::vector<SymbolBody *> SparseSymbols;
  std::vector<Symbol *> SymSymSym;
  std::vector<std::unique_ptr<Chunk>> Chunks;
  std::unique_ptr<COFFObjectFile> COFFFile;

private:
  ObjectFile(StringRef Name, std::unique_ptr<COFFObjectFile> File);
  void initializeChunks();
  void initializeSymbols();

  SymbolBody *createSymbol(StringRef Name, COFFSymbolRef Sym,
                       const void *Aux, bool IsFirst);

  std::unique_ptr<MemoryBuffer> MB;
  StringRef Directives;
};

class ImportFile : public InputFile {
public:
  ImportFile(MemoryBufferRef M);

  static bool classof(const InputFile *F) { return F->kind() == ImplibKind; }

  StringRef getName() override;

  std::vector<std::unique_ptr<SymbolBody>> &getSymbols() override {
    return SymbolBodies;
  }

private:
  void readImplib();

  MemoryBufferRef MBRef;
  std::vector<std::unique_ptr<SymbolBody>> SymbolBodies;
  StringAllocator Alloc;
};

} // namespace coff
} // namespace lld

#endif
