//===- Chunks.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Chunks.h"
#include "InputFiles.h"
#include "Writer.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/COFF.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm::object;
using namespace llvm::support::endian;
using namespace llvm::COFF;
using llvm::RoundUpToAlignment;

namespace lld {
namespace coff {

SectionChunk::SectionChunk(ObjectFile *F, const coff_section *H, uint32_t SI)
    : File(F), Header(H), SectionIndex(SI) {
  // Initialize SectionName.
  File->getCOFFObj()->getSectionName(Header, SectionName);
  // Bit [20:24] contains section alignment.
  unsigned Shift = ((Header->Characteristics & 0xF00000) >> 20) - 1;
  Align = uint32_t(1) << Shift;
}

void SectionChunk::writeTo(uint8_t *Buf) {
  if (!hasData())
    return;
  ArrayRef<uint8_t> Data;
  File->getCOFFObj()->getSectionContents(Header, Data);
  memcpy(Buf + FileOff, Data.data(), Data.size());
}

// Returns true if this chunk should be considered as a GC root.
bool SectionChunk::isRoot() {
  // COMDAT sections are live only when they are referenced by something else.
  if (isCOMDAT())
    return false;

  // Associative sections are live if their parent COMDATs are live,
  // and vice versa, so they are not considered live by themselves.
  if (IsAssocChild)
    return false;

  // Only code is subject of dead-stripping.
  return !(Header->Characteristics & IMAGE_SCN_CNT_CODE);
}

void SectionChunk::markLive() {
  if (Live)
    return;
  Live = true;

  // Mark all symbols listed in the relocation table for this section.
  for (const auto &I : getSectionRef().relocations()) {
    const coff_relocation *Rel = File->getCOFFObj()->getCOFFRelocation(I);
    SymbolBody *B = File->getSymbolBody(Rel->SymbolTableIndex);
    if (auto *Def = dyn_cast<Defined>(B))
      Def->markLive();
  }

  // Mark associative sections if any.
  for (Chunk *C : AssocChildren)
    C->markLive();
}

void SectionChunk::addAssociative(SectionChunk *Child) {
  Child->IsAssocChild = true;
  AssocChildren.push_back(Child);
}

void SectionChunk::applyRelocations(uint8_t *Buf) {
  for (const auto &I : getSectionRef().relocations()) {
    const coff_relocation *Rel = File->getCOFFObj()->getCOFFRelocation(I);
    applyReloc(Buf, Rel);
  }
}

static void add16(uint8_t *P, int32_t V) { write16le(P, read16le(P) + V); }
static void add32(uint8_t *P, int32_t V) { write32le(P, read32le(P) + V); }
static void add64(uint8_t *P, int64_t V) { write64le(P, read64le(P) + V); }

// Implements x64 PE/COFF relocations.
void SectionChunk::applyReloc(uint8_t *Buf, const coff_relocation *Rel) {
  using namespace llvm::COFF;
  uint8_t *Off = Buf + FileOff + Rel->VirtualAddress;
  SymbolBody *Body = File->getSymbolBody(Rel->SymbolTableIndex);
  uint64_t S = cast<Defined>(Body)->getRVA();
  uint64_t P = RVA + Rel->VirtualAddress;
  switch (Rel->Type) {
  case IMAGE_REL_AMD64_ADDR32:   add32(Off, S + Config->ImageBase); break;
  case IMAGE_REL_AMD64_ADDR64:   add64(Off, S + Config->ImageBase); break;
  case IMAGE_REL_AMD64_ADDR32NB: add32(Off, S); break;
  case IMAGE_REL_AMD64_REL32:    add32(Off, S - P - 4); break;
  case IMAGE_REL_AMD64_REL32_1:  add32(Off, S - P - 5); break;
  case IMAGE_REL_AMD64_REL32_2:  add32(Off, S - P - 6); break;
  case IMAGE_REL_AMD64_REL32_3:  add32(Off, S - P - 7); break;
  case IMAGE_REL_AMD64_REL32_4:  add32(Off, S - P - 8); break;
  case IMAGE_REL_AMD64_REL32_5:  add32(Off, S - P - 9); break;
  case IMAGE_REL_AMD64_SECTION:  add16(Off, Out->getSectionIndex()); break;
  case IMAGE_REL_AMD64_SECREL:   add32(Off, S - Out->getRVA()); break;
  default:
    llvm::report_fatal_error("Unsupported relocation type");
  }
}

bool SectionChunk::hasData() const {
  return !(Header->Characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA);
}

uint32_t SectionChunk::getPermissions() const {
  return Header->Characteristics & PermMask;
}

bool SectionChunk::isCOMDAT() const {
  return Header->Characteristics & IMAGE_SCN_LNK_COMDAT;
}

bool SectionChunk::isMergeable(SectionChunk *Other) {
  assert(isCOMDAT());
  assert(Other->isCOMDAT());
  const coff_section *H = Other->Header;
  return (File != Other->File && 
          !(Header->Characteristics & IMAGE_SCN_MEM_WRITE) &&
          !(H->Characteristics & IMAGE_SCN_MEM_WRITE) &&
          Header->VirtualSize == H->VirtualSize &&
          Header->SizeOfRawData == H->SizeOfRawData &&
          Header->NumberOfRelocations == H->NumberOfRelocations &&
          (Header->Characteristics & PermMask) == (H->Characteristics & PermMask) &&
          hasSameRelocations(Other) &&
          hasSameContents(Other));
}

uint64_t SectionChunk::getHeaderHash() {
  if (HashVal)
    HashVal;

  uint64_t H = 0;
  if (hasData()) {
    ArrayRef<uint8_t> A;
    File->getCOFFObj()->getSectionContents(Header, A);
    H = llvm::hash_combine_range(A.data(), A.data() + A.size());
  }
  HashVal = llvm::hash_combine((uint32_t)Header->VirtualSize,
                               (uint32_t)Header->SizeOfRawData,
                               (uint32_t)Header->NumberOfRelocations,
                               (Header->Characteristics & PermMask),
                               H);
  return HashVal;
}

bool SectionChunk::hasSameRelocations(SectionChunk *Other) {
  relocation_iterator I1 = getSectionRef().relocations().begin();
  relocation_iterator E1 = getSectionRef().relocations().end();
  relocation_iterator I2 = Other->getSectionRef().relocations().begin();
  relocation_iterator E2 = Other->getSectionRef().relocations().end();

  for (; I1 != E1; ++I1, ++I2) {
    assert(I2 != E2);
    const coff_relocation *R1 = File->getCOFFObj()->getCOFFRelocation(*I1);
    const coff_relocation *R2 = Other->File->getCOFFObj()->getCOFFRelocation(*I2);
    if (R1->VirtualAddress != R2->VirtualAddress)
      return false;
    if (R1->Type != R2->Type)
      return false;
    if (R1->Type == IMAGE_REL_AMD64_SECTION)
      continue;
    SymbolBody *S1 = File->getSymbolBody(R1->SymbolTableIndex);
    SymbolBody *S2 = Other->File->getSymbolBody(R2->SymbolTableIndex);
    if (S1->getReplacement() != S2->getReplacement())
      return false;
  }
  return true;
}

bool SectionChunk::hasSameContents(SectionChunk *Other) {
  ArrayRef<uint8_t> A1, A2;
  File->getCOFFObj()->getSectionContents(Header, A1);
  Other->File->getCOFFObj()->getSectionContents(Header, A2);
  if (A1.size() != A2.size()) {
    // llvm::dbgs() << "diff: " << getDebugName() << " and " << Other->getDebugName() << "\n";
    return false;
    assert(A1.size() == A2.size());
  }
  return memcmp(A1.data(), A2.data(), A1.size()) == 0;
}

std::string SectionChunk::getDebugName() const {
  return (Twine(File->getShortName()) + ":" + SectionName +
          "(" + Twine::utohexstr(SectionIndex) + ")").str();
}

// Prints "Discarded <symbol>" for all external function symbols.
void SectionChunk::printDiscardedMessage() {
  uint32_t E = File->getCOFFObj()->getNumberOfSymbols();
  for (uint32_t I = 0; I < E; ++I) {
    auto SrefOrErr = File->getCOFFObj()->getSymbol(I);
    COFFSymbolRef Sym = SrefOrErr.get();
    if (uint32_t(Sym.getSectionNumber()) != SectionIndex)
      continue;
    if (!Sym.isFunctionDefinition())
      continue;
    StringRef SymbolName;
    File->getCOFFObj()->getSymbolName(Sym, SymbolName);
    llvm::dbgs() << "Discarded " << SymbolName << " from "
                 << File->getShortName() << "\n";
    I += Sym.getNumberOfAuxSymbols();
  }
}

SectionRef SectionChunk::getSectionRef() {
  DataRefImpl Ref;
  Ref.p = uintptr_t(Header);
  return SectionRef(Ref, File->getCOFFObj());
}

uint32_t CommonChunk::getPermissions() const {
  using namespace llvm::COFF;
  return IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_MEM_READ |
         IMAGE_SCN_MEM_WRITE;
}

void StringChunk::writeTo(uint8_t *Buf) {
  memcpy(Buf + FileOff, Str.data(), Str.size());
}

void ImportThunkChunk::writeTo(uint8_t *Buf) {
  memcpy(Buf + FileOff, ImportThunkData, sizeof(ImportThunkData));
}

void ImportThunkChunk::applyRelocations(uint8_t *Buf) {
  uint32_t Operand = ImpSymbol->getRVA() - RVA - getSize();
  // The first two bytes are a JMP instruction. Fill its operand.
  write32le(Buf + FileOff + 2, Operand);
}

size_t HintNameChunk::getSize() const {
  // Starts with 2 byte Hint field, followed by a null-terminated string,
  // ends with 0 or 1 byte padding.
  return RoundUpToAlignment(Name.size() + 3, 2);
}

void HintNameChunk::writeTo(uint8_t *Buf) {
  write16le(Buf + FileOff, Hint);
  memcpy(Buf + FileOff + 2, Name.data(), Name.size());
}

void LookupChunk::applyRelocations(uint8_t *Buf) {
  write32le(Buf + FileOff, HintName->getRVA());
}

void OrdinalOnlyChunk::writeTo(uint8_t *Buf) {
  // An import-by-ordinal slot has MSB 1 to indicate that
  // this is import-by-ordinal (and not import-by-name).
  write64le(Buf + FileOff, (uint64_t(1) << 63) | Ordinal);
}

void DirectoryChunk::applyRelocations(uint8_t *Buf) {
  auto *E = (coff_import_directory_table_entry *)(Buf + FileOff);
  E->ImportLookupTableRVA = LookupTab->getRVA();
  E->NameRVA = DLLName->getRVA();
  E->ImportAddressTableRVA = AddressTab->getRVA();
}

ImportTable::ImportTable(StringRef N,
                         std::vector<DefinedImportData *> &Symbols) {
  // Create the import table hader.
  DLLName = new StringChunk(N);
  DirTab = new DirectoryChunk(DLLName);

  // Create lookup and address tables. If they have external names,
  // we need to create HintName chunks to store the names.
  // If they don't (if they are import-by-ordinals), we store only
  // ordinal values to the table.
  for (DefinedImportData *S : Symbols) {
    if (S->getExternalName().empty()) {
      LookupTables.push_back(new OrdinalOnlyChunk(S->getOrdinal()));
      AddressTables.push_back(new OrdinalOnlyChunk(S->getOrdinal()));
      continue;
    }
    Chunk *C = new HintNameChunk(S->getExternalName(), S->getOrdinal());
    HintNameTables.push_back(C);
    LookupTables.push_back(new LookupChunk(C));
    AddressTables.push_back(new LookupChunk(C));
  }
  for (int I = 0, E = Symbols.size(); I < E; ++I)
    Symbols[I]->setLocation(AddressTables[I]);
  DirTab->LookupTab = LookupTables[0];
  DirTab->AddressTab = AddressTables[0];
}

} // namespace coff
} // namespace lld
