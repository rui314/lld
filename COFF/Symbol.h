//===- Symbol.h -----------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_SYMBOL_H
#define LLD_COFF_SYMBOL_H

#include "Section.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <map>

namespace lld {
namespace coff {

class ArchiveFile;
class ObjectFile;
class Section;

class Symbol {
public:
  enum Kind { DefinedKind, UndefinedKind, CanBeDefinedKind };
  Kind kind() const { return SymbolKind; }

protected:
  Symbol(Kind K) : SymbolKind(K) {}

private:
  const Kind SymbolKind;
};

class Defined {
public:
  Defined(ObjectFile *F, COFFSymbolRef S)
    : Symbol(DefinedKind), File(F), Sym(S) {}
  static bool classof(const Symbol *S) { return S->kind() == DefinedKind; }

  ObjectFile *File;
  COFFSymbolRef Sym;
};

class CanBeDefined {
public:
  CanBeDefined(ArchiveFile *P, Archive::Symbol *S)
    : Symbol(CanBeDefinedKind), Parent(P), Sym(S) {}
  static bool classof(const Symbol *S) { return S->kind() == CanBeDefinedKind; }

  ArchiveFile *Parent;
  Archive::Symbol *Sym;
};

class Undefined {
public:
  Undefined() : Symbol(UndefinedKind) {}
  static bool classof(const Symbol *S) { return S->kind() == UndefinedKind; }

  ObjectFile *File;
};

struct SymbolRef {
  StringRef Name;
  Symbol *Ptr = nullptr;
};

class ArchiveFile {
public:
  static ErrorOr<std::unique_ptr<ArchiveFile>> create(StringRef Path);

  std::string Name;
  std::unique_ptr<Archive> File;

private:
  ArchiveFile(StringRef N, std::unique_ptr<std::Archive> F,
	      std::unique_ptr<MemoryBuffer> M)
    : Name(N), File(std::move(F)), MB(std::move(M)) {}

  std::unique_ptr<MemoryBuffer> MB;
};

class ObjectFile {
public:
  static ErrorOr<std::unique_ptr<ObjectFile>> create(StringRef Path);
  static ErrorOr<std::unique_ptr<ObjectFile>>
    create(StringRef Path, MemoryBufferRef MB);

  std::string Name;
  std::vector<Symbol *> Symbols;
  std::vector<Section *> Sections;
  std::unique_ptr<COFFObjectFile> File;

private:
  ObjectFile(StringRef N, std::unique_ptr<COFFObject> *F)
    : Name(N), File(std::move(F)) {}

  std::unique_ptr<MemoryBuffer> MB;
};

class InputSection {
public:
  COFFObjectFile *File;
  coff_section *Section;
  uint64_t RVA;
  uint64_t FileOff;
};

typedef std::map<llvm::StringRef, SymbolRef> SymbolTable;

} // namespace pecoff
} // namespace lld

#endif
