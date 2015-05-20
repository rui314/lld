//===- ImportTable.h ------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_IMPORT_TABLE_H
#define LLD_COFF_IMPORT_TABLE_H

#include "Reader.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/COFF.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <memory>
#include <set>
#include <vector>

using namespace llvm::support::endian;

namespace lld {
namespace coff {

class HintNameChunk : public Chunk {
public:
  HintNameChunk(StringRef Name)
    : Data(new std::vector<uint8_t>(llvm::RoundUpToAlignment(Name.size() + 4, 2))) {
    memcpy(&((*Data)[2]), Name.data(), Name.size());
  }
  
  const uint8_t *getData() const override { return &(*Data)[0]; }
  size_t getSize() const override { return Data->size(); }
  void applyRelocations(uint8_t *Buffer) override {}

private:
  std::vector<uint8_t> *Data;
};

class LookupChunk : public Chunk {
public:
  LookupChunk(HintNameChunk *H) : HintName(H) {}
  
  const uint8_t *getData() const override {
    return reinterpret_cast<const uint8_t *>(&Ent);
  }

  size_t getSize() const override { return sizeof(Ent); }

  void applyRelocations(uint8_t *Buffer) override {
    write32le(Buffer + getFileOff(), HintName->getRVA());
  }

  HintNameChunk *HintName;

private:
  uint64_t Ent = 0;
};

class DirectoryChunk : public Chunk {
public:
  DirectoryChunk(StringRef N) : Name(N) {}

  const uint8_t *getData() const override {
    return reinterpret_cast<const uint8_t *>(&Ent);
  }

  size_t getSize() const override { return sizeof(Ent); }

  void applyRelocations(uint8_t *Buffer) override {
    auto *E = reinterpret_cast<llvm::COFF::ImportDirectoryTableEntry *>(Buffer + getFileOff());
    E->ImportLookupTableRVA = LookupTab->getRVA();
    E->NameRVA = Name.getRVA();
    E->ImportAddressTableRVA = AddressTab->getRVA();
  }

  StringChunk Name;
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
  ImportTable(StringRef DLLName, std::vector<DefinedImplib *> &Symbols)
      : DirTab(DLLName) {
    for (DefinedImplib *S : Symbols)
      HintNameTables.emplace_back(S->getExportName());
    
    for (HintNameChunk &H : HintNameTables) {
      LookupTables.emplace_back(&H);
      AddressTables.emplace_back(&H);
    }

    for (int I = 0, E = Symbols.size(); I < E; ++I)
      Symbols[I]->setLocation(&AddressTables[I]);

    DirTab.LookupTab = &LookupTables[0];
    DirTab.AddressTab = &AddressTables[0];
  }

  DirectoryChunk DirTab;
  std::vector<LookupChunk> LookupTables;
  std::vector<LookupChunk> AddressTables;
  std::vector<HintNameChunk> HintNameTables;
};

} // namespace coff
} // namespace lld

#endif
