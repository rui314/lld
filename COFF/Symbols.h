//===- Symbols.h ----------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_SYMBOLS_H
#define LLD_COFF_SYMBOLS_H

#include "Chunks.h"
#include "lld/Core/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/COFF.h"
#include <memory>
#include <vector>

using llvm::object::Archive;
using llvm::object::COFFSymbolRef;

namespace lld {
namespace coff {

static const uint64_t ImageBase = 0x140000000;

class ArchiveFile;
class InputFile;
class ObjectFile;
class SymbolBody;

// A real symbol object, SymbolBody, is usually accessed indirectly
// through a Symbol. There's always one Symbol for each symbol name.
// The resolver updates SymbolBody pointers as it resolves symbols.
struct Symbol {
  Symbol(SymbolBody *P) : Body(P) {}
  SymbolBody *Body;
};

// The base class for real symbol classes.
class SymbolBody {
public:
  enum Kind {
    DefinedRegularKind,
    DefinedAbsoluteKind,
    DefinedImportDataKind,
    DefinedImportFuncKind,
    UndefinedKind,
    CanBeDefinedKind,
  };

  Kind kind() const { return SymbolKind; }
  virtual ~SymbolBody() {}

  // Returns true if this is an external symbol.
  virtual bool isExternal() { return true; }

  // Returns the symbol name.
  StringRef getName() { return Name; }

  // A SymbolBody has a backreference to a Symbol. Originally they are
  // doubly-linked. A backreference will never change. But the pointer
  // in the Symbol may be mutated by the resolver. If you have a
  // pointer P to a SymbolBody and are not sure whether the resolver
  // has chosen the object among other objects having the same name,
  // you can access P->getSymbol()->Body to get the resolver's result.
  void setBackref(Symbol *P) { Backref = P; }
  Symbol *getSymbol() { return Backref; }

protected:
  SymbolBody(Kind K, StringRef N) : SymbolKind(K), Name(N) {}

private:
  const Kind SymbolKind;
  StringRef Name;
  Symbol *Backref = nullptr;
};

// The base class for any defined symbols, including absolute symbols,
// etc.
class Defined : public SymbolBody {
public:
  Defined(Kind K, StringRef Name) : SymbolBody(K, Name) {}

  static bool classof(const SymbolBody *S) {
    Kind K = S->kind();
    return DefinedRegularKind <= K && K <= DefinedImportFuncKind;
  }

  // Returns the RVA (relative virtual address) of this symbol. The
  // writer sets and uses RVAs.
  virtual uint64_t getRVA() = 0;

  // Returns the file offset of this symbol in the final executable.
  // The writer uses this information to apply relocations.
  virtual uint64_t getFileOff() = 0;

  // Returns true if this is a common symbol.
  virtual bool isCommon() const { return false; }

  // Returns the size of a common symbol. If the resolver finds
  // multiple common symbols for the same name, it selects the
  // largest.
  virtual uint32_t getCommonSize() const {
    llvm::report_fatal_error("not implemeneted");
  }

  // Returns true if this is a COMDAT symbol. Usually, it is an error
  // if there are more than one defined symbols having the same name,
  // but COMDAT symbols are allowed to be duplicated.
  virtual bool isCOMDAT() const { return false; }

  // Called by the garbage collector. All Defined subclasses should
  // know how to call markLive to dependent symbols.
  virtual void markLive() {}
};

// Regular defined symbols read from object file symbol tables.
class DefinedRegular : public Defined {
public:
  DefinedRegular(ObjectFile *F, StringRef Name, COFFSymbolRef S, Chunk *C)
    : Defined(DefinedRegularKind, Name), File(F), Sym(S), Section(C) {}

  static bool classof(const SymbolBody *S) {
    return S->kind() == DefinedRegularKind;
  }

  uint64_t getRVA() override { return Section->getRVA() + Sym.getValue(); }
  bool isCommon() const override { return Section->isCommon(); }
  bool isCOMDAT() const override { return Section->isCOMDAT(); }
  bool isExternal() override { return Sym.isExternal(); }
  void markLive() override { Section->markLive(); }

  uint64_t getFileOff() override {
    return Section->getFileOff() + Sym.getValue();
  }

  uint32_t getCommonSize() const override {
    assert(isCommon());
    return Sym.getValue();
  }

private:
  ObjectFile *File;
  COFFSymbolRef Sym;
  Chunk *Section;
};

// Absolute symbols.
class DefinedAbsolute : public Defined {
public:
  DefinedAbsolute(StringRef Name, uint64_t VA)
    : Defined(DefinedAbsoluteKind, Name), RVA(VA - ImageBase) {}

  static bool classof(const SymbolBody *S) {
    return S->kind() == DefinedAbsoluteKind;
  }

  uint64_t getRVA() override { return RVA; }
  uint64_t getFileOff() override { llvm_unreachable("internal error"); }

private:
  uint64_t RVA;
};

// This class represents a symbol imported from a DLL. This has two
// names for internal use and external use. The former is used for
// name resolution, and the latter is used for the import descriptor
// table in an output. The former has "__imp_" prefix.
class DefinedImportData : public Defined {
public:
  DefinedImportData(StringRef D, StringRef ImportName, StringRef ExportName)
    : Defined(DefinedImportDataKind, ImportName),
      DLLName(D), ExpName(ExportName) {}

  static bool classof(const SymbolBody *S) {
    return S->kind() == DefinedImportDataKind;
  }

  uint64_t getRVA() override { return Location->getRVA(); }
  uint64_t getFileOff() override { return Location->getFileOff(); }
  StringRef getDLLName() { return DLLName; }
  StringRef getExportName() { return ExpName; }
  void setLocation(Chunk *AddressTable) { Location = AddressTable; }

private:
  StringRef DLLName;
  StringRef ExpName;
  Chunk *Location = nullptr;
};

// This class represents a symbol for a jump table entry which jumps
// to a function in a DLL. Linker are supposed to create such symbols
// without "__imp_" prefix for all function symbols exported from
// DLLs, so that you can call DLL functions as regular functions with
// a regular name. A function pointer is given as a DefinedImportData.
class DefinedImportFunc : public Defined {
public:
  DefinedImportFunc(StringRef Name, DefinedImportData *S)
    : Defined(DefinedImportFuncKind, Name), Data(S) {}

  static bool classof(const SymbolBody *S) {
    return S->kind() == DefinedImportFuncKind;
  }

  uint64_t getRVA() override { return Data.getRVA(); }
  uint64_t getFileOff() override { return Data.getFileOff(); }
  Chunk *getChunk() { return &Data; }

private:
  DefinedImportData *ImpSymbol;
  ImportFuncChunk Data;
};

// This class represents a symbol defined in an archive file. It is
// created from an archive file header, and it knows how to load an
// object file from an archive to replace itself with a defined
// symbol. If the resolver finds both Undefined and CanBeDefined for
// the same name, it will ask the CanBeDefined to load a file.
class CanBeDefined : public SymbolBody {
public:
  CanBeDefined(ArchiveFile *F, const Archive::Symbol S)
    : SymbolBody(CanBeDefinedKind, S.getName()), File(F), Sym(S) {}

  static bool classof(const SymbolBody *S) {
    return S->kind() == CanBeDefinedKind;
  }

  // Returns an object file for this symbol, or a nullptr if the file
  // was already returned.
  ErrorOr<std::unique_ptr<InputFile>> getMember();

private:
  ArchiveFile *File;
  const Archive::Symbol Sym;
};

// Undefined symbols.
class Undefined : public SymbolBody {
public:
  Undefined(StringRef Name, SymbolBody **S = nullptr)
    : SymbolBody(UndefinedKind, Name), Alias(S) {}

  static bool classof(const SymbolBody *S) {
    return S->kind() == UndefinedKind;
  }

  // An undefined symbol can have a fallback symbol which gives an
  // undefined symbol a second chance if it would remain undefined.
  // If it remains undefined, it'll be replaced with whatever the
  // Alias pointer points to.
  SymbolBody *getWeakAlias() { return Alias ? *Alias : nullptr; }

private:
  SymbolBody **Alias;
};

} // namespace coff
} // namespace lld

#endif
