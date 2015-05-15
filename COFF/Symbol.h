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
  Defined(ObjectFile *F, Section *Sec, COFFSymbolRef SymRef)
    : Symbol(DefinedKind), File(F), Sym(SymRef),
      Section(File->Sections[Sym.getSectionNumber()]) {}

  static bool classof(const Symbol *S) { return S->kind() == DefinedKind; }
  bool IsCOMDAT() const { return Section && Section->IsCOMDAT(); }

  ObjectFile *File;
  COFFSymbolRef Sym;
  InputSection *Section;
};

class CanBeDefined : public Symbol {
public:
  CanBeDefined(ArchiveFile *A, Archive::Symbol *S)
    : Symbol(CanBeDefinedKind), Archive(A), Sym(S) {}
  static bool classof(const Symbol *S) { return S->kind() == CanBeDefinedKind; }

  ObjectFile *getMember() {
    if (BeingReplaced)
      return nullptr;
    BeingReplaced = true;
    return Archive->getMember(Sym);
  }

  ArchiveFile *Archive;
  Archive::Symbol *Sym;

private:
  bool BeingReplaced = false;
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
  std::unique_ptr<Archive> Archive;
  ObjectFile *getMember(Archive::Symbol *Sym);

private:
  ArchiveFile(StringRef N, std::unique_ptr<std::Archive> A,
	      std::unique_ptr<MemoryBuffer> M)
    : Name(N), Archive(std::move(F)), MB(std::move(M)) {}

  std::unique_ptr<MemoryBuffer> MB;
  std::vector<std::unique_ptr<ObjectFile>> Members;
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
  ObjectFile(StringRef N, std::unique_ptr<COFFObject> *F)
    : Name(N), COFFFile(std::move(F)) {}

  std::unique_ptr<MemoryBuffer> MB;
};

class InputSection {
public:
  InputSection(COFFObjectFile *F, const coff_section *S)
    : File(F), Section(S) {}
  bool IsCOMDAT() const {
    return Section->Characteristics & IMAGE_SCN_LNK_COMDAT;
  }

  COFFObjectFile *File;
  const coff_section *Section;
  uint64_t RVA = 0;
  uint64_t FileOff = 0;
};

typedef std::map<llvm::StringRef, SymbolRef> SymbolTable;

} // namespace pecoff
} // namespace lld

#endif
