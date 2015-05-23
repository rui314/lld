//===- Chunks.h -----------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_CHUNKS_H
#define LLD_COFF_CHUNKS_H

#include "lld/Core/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Object/COFF.h"
#include <map>
#include <vector>

using llvm::object::COFFSymbolRef;
using llvm::object::coff_relocation;
using llvm::object::coff_section;
using llvm::sys::fs::file_magic;

namespace lld {
namespace coff {

class Defined;
class DefinedImportData;
class ObjectFile;
class OutputSection;

LLVM_ATTRIBUTE_NORETURN inline void unimplemented() {
  llvm_unreachable("internal error");
}

class Chunk {
public:
  ~Chunk() {}

  virtual const uint8_t *getData() const = 0;
  virtual size_t getSize() const = 0;
  virtual void applyRelocations(uint8_t *Buffer) {}
  virtual bool isBSS() const { return false; }
  virtual bool isCOMDAT() const { return false; }
  virtual bool isCommon() const { return false; }
  virtual uint32_t getPermissions() const { return 0; }
  virtual StringRef getSectionName() const { unimplemented(); }
  virtual void printDiscardMessage() { unimplemented(); }

  virtual bool isRoot() { return false; }
  virtual bool isLive() { return true; }
  virtual void markLive() {}

  uint64_t getRVA() { return RVA; }
  uint64_t getFileOff() { return FileOff; }
  uint32_t getAlign() { return Align; }
  void setRVA(uint64_t V) { RVA = V; }
  void setFileOff(uint64_t V) { FileOff = V; }

  void setOutputSection(OutputSection *O) { Out = O; }
  OutputSection *getOutputSection() { return Out; }

protected:
  uint32_t Align = 1;
  uint64_t RVA = 0;
  uint64_t FileOff = 0;

private:
  OutputSection *Out = nullptr;
};

class SectionChunk : public Chunk {
public:
  SectionChunk(ObjectFile *File, const coff_section *Header,
               uint32_t SectionIndex);
  const uint8_t *getData() const override;
  size_t getSize() const override;
  void applyRelocations(uint8_t *Buffer) override;
  bool isBSS() const override;
  bool isCOMDAT() const override;
  uint32_t getPermissions() const override;
  StringRef getSectionName() const override { return SectionName; }
  void printDiscardMessage() override;

  bool isRoot() override;
  void markLive() override;
  bool isLive() override { return isRoot() || Live; }
  void addAssociative(SectionChunk *Child);

private:
  void applyReloc(uint8_t *Buffer, const coff_relocation *Rel);

  ObjectFile *File;
  const coff_section *Header;
  uint32_t SectionIndex;
  StringRef SectionName;
  ArrayRef<uint8_t> Data;
  bool Live = false;
  std::vector<Chunk *> Children;
  bool IsChild = false;
};

class CommonChunk : public Chunk {
public:
  CommonChunk(const COFFSymbolRef S) : Sym(S) {}
  const uint8_t *getData() const override { unimplemented(); }
  size_t getSize() const override;
  bool isBSS() const override { return true; }
  bool isCommon() const override { return true; }
  uint32_t getPermissions() const override;
  StringRef getSectionName() const override { return ".bss"; }

private:
  const COFFSymbolRef Sym;
};

class StringChunk : public Chunk {
public:
  StringChunk(StringRef S) : Data(S.size() + 1) {
    memcpy(Data.data(), S.data(), S.size());
    Data[S.size()] = 0;
  }

  const uint8_t *getData() const override { return &Data[0]; }
  size_t getSize() const override { return Data.size(); }

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

class HintNameChunk : public Chunk {
public:
  HintNameChunk(StringRef Name);
  const uint8_t *getData() const override { return &(*Data)[0]; }
  size_t getSize() const override { return Data->size(); }
  void applyRelocations(uint8_t *Buffer) override {}

private:
  std::vector<uint8_t> *Data;
};

class LookupChunk : public Chunk {
public:
  LookupChunk(HintNameChunk *H) : HintName(H) {}
  const uint8_t *getData() const override { return (const uint8_t *)&Ent; }
  size_t getSize() const override { return sizeof(Ent); }
  void applyRelocations(uint8_t *Buffer) override;
  HintNameChunk *HintName;

private:
  uint64_t Ent = 0;
};

class DirectoryChunk : public Chunk {
public:
  DirectoryChunk(StringChunk *N) : DLLName(N) {}
  const uint8_t *getData() const override { return (const uint8_t *)&Ent; }
  size_t getSize() const override { return sizeof(Ent); }
  void applyRelocations(uint8_t *Buffer) override;

  StringChunk *DLLName;
  LookupChunk *LookupTab;
  LookupChunk *AddressTab;

private:
  llvm::COFF::ImportDirectoryTableEntry Ent = {};
};

class NullChunk : public Chunk {
public:
  NullChunk(size_t Size) : Data(Size) {}
  const uint8_t *getData() const override { return Data.data(); }
  size_t getSize() const override { return Data.size(); }
  void applyRelocations(uint8_t *Buffer) override {}

private:
  std::vector<uint8_t> Data;
};

class ImportTable {
public:
  ImportTable(StringRef N, std::vector<DefinedImportData *> &Symbols);
  StringChunk *DLLName;
  DirectoryChunk *DirTab;
  std::vector<LookupChunk *> LookupTables;
  std::vector<LookupChunk *> AddressTables;
  std::vector<HintNameChunk *> HintNameTables;
};

} // namespace coff
} // namespace lld

#endif
