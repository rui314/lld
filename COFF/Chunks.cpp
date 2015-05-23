//===- Chunks.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Chunks.h"
#include "Files.h"
#include "Writer.h"
#include "lld/Core/Error.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/COFF.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm::object;
using namespace llvm::support::endian;
using llvm::COFF::ImportHeader;
using llvm::RoundUpToAlignment;

namespace lld {
namespace coff {

SectionChunk::SectionChunk(ObjectFile *F, const coff_section *H, uint32_t SI)
    : File(F), Header(H), SectionIndex(SI) {
  File->COFFFile->getSectionName(Header, SectionName);
  if (!isBSS())
    File->COFFFile->getSectionContents(Header, Data);
  unsigned Shift = ((Header->Characteristics & 0x00F00000) >> 20) - 1;
  Align = uint32_t(1) << Shift;
}

const uint8_t *SectionChunk::getData() const {
  assert(!isBSS());
  return Data.data();
}

bool SectionChunk::isRoot() {
  return !isCOMDAT() && !IsChild && !(Header->Characteristics & llvm::COFF::IMAGE_SCN_CNT_CODE);
}

void SectionChunk::markLive() {
  if (Live)
    return;
  Live = true;
  DataRefImpl Ref;
  Ref.p = uintptr_t(Header);
  COFFObjectFile *FP = File->COFFFile.get();
  for (const auto &I : SectionRef(Ref, FP).relocations()) {
    const coff_relocation *Rel = FP->getCOFFRelocation(I);
    if (auto *S = dyn_cast<Defined>(File->SymbolRefs[Rel->SymbolTableIndex]->Ptr))
      S->markLive();
  }
  for (Chunk *C : Children)
    C->markLive();
}

void SectionChunk::addAssociative(SectionChunk *Child) {
  Child->IsChild = true;
  Children.push_back(Child);
}

void SectionChunk::applyRelocations(uint8_t *Buffer) {
  DataRefImpl Ref;
  Ref.p = uintptr_t(Header);
  COFFObjectFile *FP = File->COFFFile.get();
  for (const auto &I : SectionRef(Ref, FP).relocations()) {
    const coff_relocation *Rel = FP->getCOFFRelocation(I);
    applyReloc(Buffer, Rel);
  }
}

static void add16(uint8_t *L, int32_t V) { write16le(L, read16le(L) + V); }
static void add32(uint8_t *L, int32_t V) { write32le(L, read32le(L) + V); }
static void add64(uint8_t *L, int64_t V) { write64le(L, read64le(L) + V); }

void SectionChunk::applyReloc(uint8_t *Buffer, const coff_relocation *Rel) {
  using namespace llvm::COFF;
  uint8_t *Off = Buffer + FileOff + Rel->VirtualAddress;
  auto *Sym = cast<Defined>(File->SymbolRefs[Rel->SymbolTableIndex]->Ptr);
  uint64_t S = Sym->getRVA();
  uint64_t P = RVA + Rel->VirtualAddress;
  OutputSection *Sec = getOutputSection();
  switch (Rel->Type) {
  case IMAGE_REL_AMD64_ADDR32:   add32(Off, ImageBase + S); break;
  case IMAGE_REL_AMD64_ADDR64:   add64(Off, ImageBase + S); break;
  case IMAGE_REL_AMD64_ADDR32NB: add32(Off, S); break;
  case IMAGE_REL_AMD64_REL32:    add32(Off, S - P - 4); break;
  case IMAGE_REL_AMD64_REL32_1:  add32(Off, S - P - 5); break;
  case IMAGE_REL_AMD64_REL32_2:  add32(Off, S - P - 6); break;
  case IMAGE_REL_AMD64_REL32_3:  add32(Off, S - P - 7); break;
  case IMAGE_REL_AMD64_REL32_4:  add32(Off, S - P - 8); break;
  case IMAGE_REL_AMD64_REL32_5:  add32(Off, S - P - 9); break;
  case IMAGE_REL_AMD64_SECTION:  add16(Off, Sec->getSectionIndex()); break;
  case IMAGE_REL_AMD64_SECREL:   add32(Off, S - Sec->getRVA()); break;
  default:
    llvm::report_fatal_error("Unsupported relocation type");
  }
}

bool SectionChunk::isBSS() const {
  return Header->Characteristics & llvm::COFF::IMAGE_SCN_CNT_UNINITIALIZED_DATA;
}

uint32_t SectionChunk::getPermissions() const {
  return Header->Characteristics & PermMask;
}

size_t SectionChunk::getSize() const {
  return Header->SizeOfRawData;
}

bool SectionChunk::isCOMDAT() const {
  return Header->Characteristics & llvm::COFF::IMAGE_SCN_LNK_COMDAT;
}

void SectionChunk::printDiscardMessage() {
  uint32_t E = File->COFFFile->getNumberOfSymbols();
  for (uint32_t I = 0; I < E; ++I) {
    auto SrefOrErr = File->COFFFile->getSymbol(I);
    COFFSymbolRef Sym = SrefOrErr.get();
    if (Sym.getSectionNumber() != SectionIndex)
      continue;
    if (!Sym.isFunctionDefinition())
      continue;
    StringRef SymbolName;
    File->COFFFile->getSymbolName(Sym, SymbolName);
    llvm::dbgs() << "Discarded " << SymbolName << " from "
                 << File->getShortName() << "\n";
    I += Sym.getNumberOfAuxSymbols();
  }
}

size_t CommonChunk::getSize() const {
  return Sym.getValue();
}

uint32_t CommonChunk::getPermissions() const {
  using namespace llvm::COFF;
  return IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
}

void ImportFuncChunk::applyRelocations(uint8_t *Buffer) {
  uint32_t Operand = ImpSymbol->getRVA() - RVA - Data.size();
  write32le(Buffer + FileOff + 2, Operand);
}

HintNameChunk::HintNameChunk(StringRef Name)
  : Data(new std::vector<uint8_t>(llvm::RoundUpToAlignment(Name.size() + 4, 2))) {
  memcpy(&((*Data)[2]), Name.data(), Name.size());
}

void LookupChunk::applyRelocations(uint8_t *Buffer) {
  write32le(Buffer + getFileOff(), HintName->getRVA());
}

void DirectoryChunk::applyRelocations(uint8_t *Buffer) {
  auto *E = (llvm::COFF::ImportDirectoryTableEntry *)(Buffer + getFileOff());
  E->ImportLookupTableRVA = LookupTab->getRVA();
  E->NameRVA = DLLName->getRVA();
  E->ImportAddressTableRVA = AddressTab->getRVA();
}

ImportTable::ImportTable(StringRef N, std::vector<DefinedImportData *> &Symbols) {
  DLLName = new StringChunk(N);
  DirTab = new DirectoryChunk(DLLName);
  for (DefinedImportData *S : Symbols)
    HintNameTables.push_back(new HintNameChunk(S->getExportName()));

  for (HintNameChunk *H : HintNameTables) {
    LookupTables.push_back(new LookupChunk(H));
    AddressTables.push_back(new LookupChunk(H));
  }

  for (int I = 0, E = Symbols.size(); I < E; ++I)
    Symbols[I]->setLocation(AddressTables[I]);

  DirTab->LookupTab = LookupTables[0];
  DirTab->AddressTab = AddressTables[0];
}

}
}
