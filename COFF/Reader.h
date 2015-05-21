//===- Reader.h -----------------------------------------------------------===//
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
#include "llvm/Support/FileUtilities.h"
#include <map>
#include <memory>
#include <set>
#include <vector>

using llvm::object::Archive;
using llvm::object::COFFObjectFile;
using llvm::object::COFFSymbolRef;
using llvm::object::coff_relocation;
using llvm::object::coff_section;
using llvm::sys::fs::file_magic;
using llvm::sys::fs::identify_magic;

namespace lld {
namespace coff {

const uint32_t PermMask = 0xF00000F0;

class ArchiveFile;
class Chunk;
class InputFile;
class ObjectFile;
class OutputSection;
class Defined;
struct SymbolRef;

class Chunk {
public:
  virtual const uint8_t *getData() const = 0;
  virtual size_t getSize() const = 0;
  virtual void applyRelocations(uint8_t *Buffer) = 0;
  virtual bool isBSS() const { return false; }
  virtual uint32_t getPermission() const { return 0; }
  virtual StringRef getSectionName() const { llvm_unreachable("not implemented"); }

  uint64_t getRVA() { return RVA; }
  uint64_t getFileOff() { return FileOff; }
  uint64_t getAlign() { return Align; }
  void setRVA(uint64_t V) { RVA = V; }
  void setFileOff(uint64_t V) { FileOff = V; }
  void setAlign(uint64_t V) { Align = V; }

  void setOutputSection(OutputSection *O) { Out = O; }
  OutputSection *getOutputSection() { return Out; }

private:
  uint64_t RVA = 0;
  uint64_t FileOff = 0;
  uint64_t Align = 1;
  OutputSection *Out = nullptr;
};

class SectionChunk : public Chunk {
public:
  SectionChunk(ObjectFile *File, const coff_section *Header);
  const uint8_t *getData() const override;
  size_t getSize() const override;
  void applyRelocations(uint8_t *Buffer) override;
  bool isBSS() const override;
  bool isCOMDAT() const;
  uint32_t getPermission() const override;
  StringRef getSectionName() const override { return SectionName; }

private:
  void applyRelocation(uint8_t *Buffer, const coff_relocation *Rel);

  ObjectFile *File;
  const coff_section *Header;
  StringRef SectionName;
  ArrayRef<uint8_t> Data;
};

class StringChunk : public Chunk {
public:
  StringChunk(StringRef S) : Data(S.size() + 1) {
    memcpy(Data.data(), S.data(), S.size());
    Data[S.size()] = 0;
  }

  const uint8_t *getData() const override { return &Data[0]; }
  size_t getSize() const override { return Data.size(); }
  void applyRelocations(uint8_t *Buffer) override {}

private:
  std::vector<uint8_t> Data;
};

static const uint8_t ImportFuncData[] = {
  0xff, 0x25, 0x00, 0x00, 0x00, 0x00, // JMP *0x0
};

class ImportFuncChunk : public Chunk {
public:
  ImportFuncChunk(Defined *S)
    : ImpSymbol(S),
      Data(ImportFuncData, ImportFuncData + sizeof(ImportFuncData)) {}

  const uint8_t *getData() const override { return &Data[0]; }
  size_t getSize() const override { return Data.size(); }
  void applyRelocations(uint8_t *Buffer) override;

private:
  Defined *ImpSymbol;
  std::vector<uint8_t> Data;
};

class Symbol {
public:
  enum Kind {
    DefinedRegularKind,
    DefinedImportDataKind,
    DefinedImportFuncKind,
    UndefinedKind,
    CanBeDefinedKind,
  };
  Kind kind() const { return SymbolKind; }
  virtual ~Symbol() {}

  virtual bool isExternal() { return true; }
  virtual StringRef getName() = 0;

  void setSymbolRefAddress(SymbolRef **PP) { SymbolRefPP = PP; }
  void setSymbolRef(SymbolRef *P) { *SymbolRefPP = P; }

protected:
  Symbol(Kind K) : SymbolKind(K) {}

private:
  const Kind SymbolKind;
  SymbolRef **SymbolRefPP = nullptr;
};

class Defined : public Symbol {
public:
  Defined(Kind K) : Symbol(K) {}

  static bool classof(const Symbol *S) {
    Kind K = S->kind();
    return DefinedRegularKind <= K && K <= DefinedImportFuncKind;
  }

  virtual uint64_t getRVA() = 0;
  virtual uint64_t getFileOff() = 0;
  virtual bool isCOMDAT() const { return false; }
};

class DefinedRegular : public Defined {
public:
  DefinedRegular(ObjectFile *F, StringRef N, COFFSymbolRef SymRef);

  static bool classof(const Symbol *S) {
    return S->kind() == DefinedRegularKind;
  }

  StringRef getName() override { return Name; }
  uint64_t getRVA() override;
  uint64_t getFileOff() override;
  bool isCOMDAT() const override;
  bool isExternal() override { return Sym.isExternal(); }

private:
  ObjectFile *File;
  StringRef Name;
  COFFSymbolRef Sym;
  SectionChunk *Chunk;
};

class DefinedImportData : public Defined {
public:
  DefinedImportData(StringRef D, StringRef N)
    : Defined(DefinedImportDataKind), DLLName(D),
      Name((Twine("__imp_") + N).str()), ExpName(N) {}

  static bool classof(const Symbol *S) {
    return S->kind() == DefinedImportDataKind;
  }

  StringRef getName() override { return Name; }
  uint64_t getRVA() override { return Location->getRVA(); }
  uint64_t getFileOff() override { return Location->getFileOff(); }
  StringRef getDLLName() { return DLLName; }
  StringRef getExportName() { return ExpName; }
  void setLocation(Chunk *AddressTable) { Location = AddressTable; }

private:
  StringRef DLLName;
  std::string Name;
  std::string ExpName;
  Chunk *Location = nullptr;
};

class DefinedImportFunc : public Defined {
public:
  DefinedImportFunc(StringRef N, DefinedImportData *S)
    : Defined(DefinedImportFuncKind), Name(N), Data(S) {}

  static bool classof(const Symbol *S) {
    return S->kind() == DefinedImportFuncKind;
  }

  StringRef getName() override { return Name; }
  uint64_t getRVA() override { return Data.getRVA(); }
  uint64_t getFileOff() override { return Data.getFileOff(); }
  Chunk *getChunk() { return &Data; }

private:
  std::string Name;
  DefinedImportData *ImpSymbol;
  ImportFuncChunk Data;
};

class CanBeDefined : public Symbol {
public:
  CanBeDefined(ArchiveFile *F, const Archive::Symbol S)
    : Symbol(CanBeDefinedKind), Name(S.getName()), File(F), Sym(S) {}

  static bool classof(const Symbol *S) {
    return S->kind() == CanBeDefinedKind;
  }

  StringRef getName() override { return Name; }
  ErrorOr<std::unique_ptr<InputFile>> getMember();

private:
  StringRef Name;
  ArchiveFile *File;
  const Archive::Symbol Sym;
};

class Undefined : public Symbol {
public:
  Undefined(ObjectFile *F, StringRef N)
    : Symbol(UndefinedKind), File(F), Name(N) {}

  static bool classof(const Symbol *S) {
    return S->kind() == UndefinedKind;
  }

  StringRef getName() override { return Name; }

private:
  ObjectFile *File;
  StringRef Name;
};

struct SymbolRef {
  SymbolRef(Symbol *P) : Ptr(P) {}
  SymbolRef() : Ptr(nullptr) {}
  Symbol *Ptr;
};

class InputFile {
public:
  enum Kind { ArchiveKind, ObjectKind, ImplibKind };
  Kind kind() const { return FileKind; }
  virtual ~InputFile() {}

  virtual StringRef getName() = 0;
  virtual std::vector<Symbol *> getSymbols() = 0;

protected:
  InputFile(Kind K) : FileKind(K) {}

private:
  const Kind FileKind;
};

class ArchiveFile : public InputFile {
public:
  static bool classof(const InputFile *F) { return F->kind() == ArchiveKind; }

  static ErrorOr<std::unique_ptr<ArchiveFile>> create(StringRef Path);

  StringRef getName() override { return Name; }
  std::vector<Symbol *> getSymbols() override;

  std::string Name;
  std::unique_ptr<Archive> File;

  ErrorOr<MemoryBufferRef> getMember(const Archive::Symbol *Sym);

private:
  ArchiveFile(StringRef N, std::unique_ptr<Archive> F,
	      std::unique_ptr<MemoryBuffer> M)
    : InputFile(ArchiveKind), Name(N), File(std::move(F)), MB(std::move(M)) {}

  std::unique_ptr<MemoryBuffer> MB;
  std::set<const char *> Seen;
};

class ObjectFile : public InputFile {
public:
  static bool classof(const InputFile *F) { return F->kind() == ObjectKind; }

  static ErrorOr<std::unique_ptr<ObjectFile>> create(StringRef Path);
  static ErrorOr<std::unique_ptr<ObjectFile>>
    create(StringRef Path, MemoryBufferRef MB);

  StringRef getName() override { return Name; }
  std::vector<Symbol *> getSymbols() override;

  std::string Name;
  std::vector<SymbolRef *> Symbols;
  std::vector<SectionChunk> Chunks;
  std::unique_ptr<COFFObjectFile> COFFFile;

private:
  ObjectFile(StringRef Name, std::unique_ptr<COFFObjectFile> File);

  std::unique_ptr<MemoryBuffer> MB;
};

class ImplibFile : public InputFile {
public:
  ImplibFile(MemoryBufferRef M);

  static bool classof(const InputFile *F) { return F->kind() == ImplibKind; }

  StringRef getName() override;
  std::vector<Symbol *> getSymbols() override { return Symbols; }

private:
  void readImplib();

  MemoryBufferRef MBRef;
  std::vector<Symbol *> Symbols;
};

class OutputSection {
public:
  OutputSection(StringRef Nam, uint32_t SectionIndex);
  void setRVA(uint64_t);
  void setFileOffset(uint64_t);
  void addChunk(Chunk *C);
  StringRef getName() { return Name; }
  uint64_t getSectionIndex() { return SectionIndex; }
  std::vector<Chunk *> &getChunks() { return Chunks; }

  const llvm::object::coff_section *getHeader() { return &Header; }
  void addPermission(uint32_t C);
  uint32_t getPermission() { return Header.Characteristics & PermMask; }
  uint32_t getCharacteristics() { return Header.Characteristics; }
  uint64_t getRVA() { return Header.VirtualAddress; }
  uint64_t getFileOff() { return Header.PointerToRawData; }
  uint64_t getVirtualSize() { return Header.VirtualSize; }
  uint64_t getRawSize() { return Header.SizeOfRawData; }

private:
  llvm::object::coff_section Header;
  StringRef Name;
  uint32_t SectionIndex;
  std::vector<Chunk *> Chunks;
};

typedef std::map<llvm::StringRef, SymbolRef *> SymbolTable;

} // namespace coff
} // namespace lld

#endif
