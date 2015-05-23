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

struct Symbol {
  Symbol(SymbolBody *P) : Body(P) {}
  Symbol() : Body(nullptr) {}
  SymbolBody *Body;
};

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

  virtual bool isExternal() { return true; }
  StringRef getName() { return Name; }

  void setSymbolAddress(Symbol **PP) { SymbolRefPP = PP; }
  void setSymbol(Symbol *P) { *SymbolRefPP = P; }
  Symbol *getSymbol() { return *SymbolRefPP; }

protected:
  SymbolBody(Kind K, StringRef N) : SymbolKind(K), Name(N) {}

private:
  const Kind SymbolKind;
  StringRef Name;
  Symbol **SymbolRefPP = nullptr;
};

class Defined : public SymbolBody {
public:
  Defined(Kind K, StringRef Name) : SymbolBody(K, Name) {}

  static bool classof(const SymbolBody *S) {
    Kind K = S->kind();
    return DefinedRegularKind <= K && K <= DefinedImportFuncKind;
  }

  virtual uint64_t getRVA() = 0;
  virtual uint64_t getFileOff() = 0;
  virtual bool isCommon() const { return false; }
  virtual uint32_t getCommonSize() const { return 0; }
  virtual bool isCOMDAT() const { return false; }
  virtual void markLive() {}
};

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

class CanBeDefined : public SymbolBody {
public:
  CanBeDefined(ArchiveFile *F, const Archive::Symbol S)
    : SymbolBody(CanBeDefinedKind, S.getName()), File(F), Sym(S) {}

  static bool classof(const SymbolBody *S) {
    return S->kind() == CanBeDefinedKind;
  }

  ErrorOr<std::unique_ptr<InputFile>> getMember();

  ArchiveFile *File;

private:
  const Archive::Symbol Sym;
};

class Undefined : public SymbolBody {
public:
  Undefined(StringRef Name, SymbolBody **S = nullptr)
    : SymbolBody(UndefinedKind, Name), WeakExternal(S) {}

  static bool classof(const SymbolBody *S) {
    return S->kind() == UndefinedKind;
  }

  bool replaceWeakExternal();
  bool hasWeakExternal() { return WeakExternal; }

private:
  SymbolBody **WeakExternal;
};

} // namespace coff
} // namespace lld

#endif
