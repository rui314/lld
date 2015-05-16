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

#include "lld/Core/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/COFF.h"
#include <map>
#include <memory>
#include <set>
#include <vector>

using llvm::object::Archive;
using llvm::object::COFFObjectFile;
using llvm::object::COFFSymbolRef;
using llvm::object::coff_section;

namespace lld {
namespace coff {

class ArchiveFile;
class InputSection;
class ObjectFile;

class Symbol {
public:
  enum Kind { DefinedKind, UndefinedKind, CanBeDefinedKind };
  Kind kind() const { return SymbolKind; }

protected:
  Symbol(Kind K) : SymbolKind(K) {}

private:
  const Kind SymbolKind;
};

class Defined : public Symbol {
public:
  Defined(ObjectFile *F, COFFSymbolRef SymRef);
  static bool classof(const Symbol *S) { return S->kind() == DefinedKind; }
  bool IsCOMDAT() const;

  ObjectFile *File;
  COFFSymbolRef Sym;
  InputSection *Section;
};

class CanBeDefined : public Symbol {
public:
  CanBeDefined(ArchiveFile *F, Archive::Symbol *S)
    : Symbol(CanBeDefinedKind), File(F), Sym(S) {}
  static bool classof(const Symbol *S) { return S->kind() == CanBeDefinedKind; }

  ErrorOr<std::unique_ptr<ObjectFile>> getMember();

  ArchiveFile *File;
  Archive::Symbol *Sym;
};

class Undefined : public Symbol {
public:
  Undefined(ObjectFile *F) : Symbol(UndefinedKind), File(F) {}
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
  ErrorOr<std::unique_ptr<ObjectFile>> getMember(Archive::Symbol *Sym);

private:
  ArchiveFile(StringRef N, std::unique_ptr<Archive> F,
	      std::unique_ptr<MemoryBuffer> M)
    : Name(N), File(std::move(F)), MB(std::move(M)) {}

  std::unique_ptr<MemoryBuffer> MB;
  std::set<const char *> Seen;
};

class ObjectFile {
public:
  static ErrorOr<std::unique_ptr<ObjectFile>> create(StringRef Path);
  static ErrorOr<std::unique_ptr<ObjectFile>>
    create(StringRef Path, MemoryBufferRef MB);

  std::string Name;
  std::vector<SymbolRef *> Symbols;
  std::vector<InputSection> Sections;
  std::unique_ptr<COFFObjectFile> COFFFile;

private:
  ObjectFile(StringRef N, std::unique_ptr<COFFObjectFile> F)
    : Name(N), COFFFile(std::move(F)) {}

  std::unique_ptr<MemoryBuffer> MB;
};

class InputSection {
public:
  InputSection(COFFObjectFile *F, const coff_section *S)
    : File(F), Section(S) {
    if (File && Section)
      File->getSectionName(Section, Name);
  }

  bool IsCOMDAT() const {
    return Section->Characteristics & llvm::COFF::IMAGE_SCN_LNK_COMDAT;
  }

  ErrorOr<StringRef> getName() {
    StringRef Name;
    if (File->getSectionName(Section, Name))
      return "";
    return Name;
  }

  ArrayRef<uint8_t> getContents() {
    ArrayRef<uint8_t> Res;
    File->getSectionContents(Section, Res);
    return Res;
  }

  COFFObjectFile *File;
  const coff_section *Section;
  StringRef Name;
  uint64_t RVA = 0;
  uint64_t FileOff = 0;
};

typedef std::map<llvm::StringRef, SymbolRef> SymbolTable;

} // namespace coff
} // namespace lld

#endif
