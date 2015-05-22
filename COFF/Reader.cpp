//===- Reader.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Reader.h"
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

DefinedRegular::DefinedRegular(ObjectFile *F, StringRef Name, COFFSymbolRef S, Chunk *C)
  : Defined(DefinedRegularKind, Name), File(F), Sym(S), Section(C) {}

bool DefinedRegular::isCommon() const {
  return Section->isCommon();
}

uint32_t DefinedRegular::getCommonSize() const {
  assert(isCommon());
  return Sym.getValue();
}

bool DefinedRegular::isCOMDAT() const {
  return Section->isCOMDAT();
}

uint64_t DefinedRegular::getRVA() {
  return Section->getRVA() + Sym.getValue();
}

uint64_t DefinedRegular::getFileOff() {
  return Section->getFileOff() + Sym.getValue();
}

ErrorOr<std::unique_ptr<InputFile>> CanBeDefined::getMember() {
  auto MBRefOrErr = File->getMember(&Sym);
  if (auto EC = MBRefOrErr.getError())
    return EC;
  MemoryBufferRef MBRef = MBRefOrErr.get();

  // getMember returns an empty buffer if the member was already
  // read from the library.
  if (MBRef.getBuffer().empty())
    return nullptr;

  file_magic Magic = identify_magic(StringRef(MBRef.getBuffer()));
  if (Magic == file_magic::coff_import_library)
    return llvm::make_unique<ImplibFile>(MBRef);

  if (Magic != file_magic::coff_object)
    return make_dynamic_error_code("unknown file type");

  StringRef Filename = MBRef.getBufferIdentifier();
  ErrorOr<std::unique_ptr<ObjectFile>> FileOrErr = ObjectFile::create(Filename, MBRef);
  if (auto EC = FileOrErr.getError())
    return EC;
  return std::move(FileOrErr.get());
}

static StringRef basename(StringRef Path) {
  size_t Pos = Path.rfind('\\');
  if (Pos == StringRef::npos)
    return Path;
  return Path.substr(Pos + 1);
}

std::string InputFile::getShortName() {
  StringRef Name = getName();
  if (ParentName == "")
    return Name;
  return (basename(Name) + "(" + basename(ParentName) + ")").str();
}

ErrorOr<std::unique_ptr<ArchiveFile>> ArchiveFile::create(StringRef Path) {
  auto MBOrErr = MemoryBuffer::getFile(Path);
  if (auto EC = MBOrErr.getError())
    return EC;
  std::unique_ptr<MemoryBuffer> MB = std::move(MBOrErr.get());

  auto ArchiveOrErr = Archive::create(MB->getMemBufferRef());
  if (auto EC = ArchiveOrErr.getError())
    return EC;
  std::unique_ptr<Archive> File = std::move(ArchiveOrErr.get());

  return std::unique_ptr<ArchiveFile>(
    new ArchiveFile(Path, std::move(File), std::move(MB)));
}

ArchiveFile::ArchiveFile(StringRef N, std::unique_ptr<Archive> F,
                         std::unique_ptr<MemoryBuffer> M)
    : InputFile(ArchiveKind), Name(N), File(std::move(F)), MB(std::move(M)) {
  for (const Archive::Symbol &Sym : File->symbols())
    if (Sym.getName() != "__NULL_IMPORT_DESCRIPTOR")
      Symbols.push_back(llvm::make_unique<CanBeDefined>(this, Sym));
}

ErrorOr<MemoryBufferRef>
ArchiveFile::getMember(const Archive::Symbol *Sym) {
  auto ItOrErr = Sym->getMember();
  if (auto EC = ItOrErr.getError())
    return EC;
  Archive::child_iterator It = ItOrErr.get();

  const char *StartAddr = It->getBuffer().data();
  if (Seen.count(StartAddr))
    return MemoryBufferRef();
  Seen.insert(StartAddr);

  auto MBRefOrErr = It->getMemoryBufferRef();
  if (auto EC = MBRefOrErr.getError())
    return EC;
  return MBRefOrErr.get();
}

ErrorOr<std::unique_ptr<ObjectFile>> ObjectFile::create(StringRef Path) {
  auto MBOrErr = MemoryBuffer::getFile(Path);
  if (auto EC = MBOrErr.getError())
    return EC;
  std::unique_ptr<MemoryBuffer> MB = std::move(MBOrErr.get());

  auto FileOrErr = create(Path, MB->getMemBufferRef());
  if (auto EC = FileOrErr.getError())
    return EC;
  std::unique_ptr<ObjectFile> File = std::move(FileOrErr.get());

  // Transfer the ownership
  File->MB = std::move(MB);
  return std::move(File);
}

ObjectFile::ObjectFile(StringRef N, std::unique_ptr<COFFObjectFile> F)
    : InputFile(ObjectKind), Name(N), COFFFile(std::move(F)) {
  initializeChunks();
  initializeSymbols();
}

void ObjectFile::initializeChunks() {
  uint32_t NumSections = COFFFile->getNumberOfSections();
  Chunks.resize(NumSections + 1);
  for (uint32_t I = 1; I < NumSections + 1; ++I) {
    const coff_section *Sec;
    StringRef Name;
    if (auto EC = COFFFile->getSection(I, Sec)) {
      llvm::errs() << "getSection failed: " << Name << ": " << EC.message() << "\n";
      return;
    }
    if (auto EC = COFFFile->getSectionName(Sec, Name)) {
      llvm::errs() << "getSectionName failed: " << Name << ": " << EC.message() << "\n";
      return;
    }
    if (Name.startswith(".debug"))
      continue;
    if (Name == ".drectve") {
      ArrayRef<uint8_t> Data;
      COFFFile->getSectionContents(Sec, Data);
      Directives = StringRef((char *)Data.data(), Data.size()).trim();
      continue;
    }
    Chunks[I].reset(new SectionChunk(this, Sec, I));
  }
}

void ObjectFile::initializeSymbols() {
  uint32_t NumSymbols = COFFFile->getNumberOfSymbols();
  SymbolRefs.resize(NumSymbols);
  for (uint32_t I = 0; I < NumSymbols; ++I) {
    // Get a COFFSymbolRef object.
    auto SrefOrErr = COFFFile->getSymbol(I);
    if (auto EC = SrefOrErr.getError()) {
      llvm::errs() << "broken object file: " << Name << ": " << EC.message() << "\n";
      break;
    }
    COFFSymbolRef Sref = SrefOrErr.get();

    // Get a symbol name.
    StringRef SymbolName;
    if (auto EC = COFFFile->getSymbolName(Sref, SymbolName)) {
      llvm::errs() << "broken object file: " << Name << ": " << EC.message() << "\n";
      break;
    }

    std::unique_ptr<Symbol> Sym;
    if (Sref.isUndefined()) {
      Sym.reset(new Undefined(SymbolName));
    } else if (Sref.isCommon()) {
      Chunk *C = new CommonChunk(Sref);
      Chunks.push_back(std::unique_ptr<Chunk>(C));
      Sym.reset(new DefinedRegular(this, SymbolName, Sref, C));
    } else if (Sref.getSectionNumber() == -1) {
      if (SymbolName != "@comp.id" && SymbolName != "@feat.00")
        Sym.reset(new DefinedAbsolute(SymbolName, Sref.getValue()));
    } else {
      if (std::unique_ptr<Chunk> &C = Chunks[Sref.getSectionNumber()])
        Sym.reset(new DefinedRegular(this, SymbolName, Sref, C.get()));
    }
    if (Sym) {
      Sym->setSymbolRefAddress(&SymbolRefs[I]);
      Symbols.push_back(std::move(Sym));
    }
    I += Sref.getNumberOfAuxSymbols();
  }
}

ErrorOr<std::unique_ptr<ObjectFile>>
ObjectFile::create(StringRef Path, MemoryBufferRef MBRef) {
  auto BinOrErr = createBinary(MBRef);
  if (auto EC = BinOrErr.getError())
    return EC;
  std::unique_ptr<Binary> Bin = std::move(BinOrErr.get());

  if (!isa<COFFObjectFile>(Bin.get()))
    return lld::make_dynamic_error_code(Twine(Path) + " is not a COFF file.");
  std::unique_ptr<COFFObjectFile> Obj(static_cast<COFFObjectFile *>(Bin.release()));
  auto File = std::unique_ptr<ObjectFile>(new ObjectFile(Path, std::move(Obj)));
  return std::move(File);
}

StringRef ImplibFile::getName() {
  return MBRef.getBufferIdentifier();
}

ImplibFile::ImplibFile(MemoryBufferRef M)
    : InputFile(ImplibKind), MBRef(M) {
  readImplib();
}

void ImplibFile::readImplib() {
  const char *Buf = MBRef.getBufferStart();
  const char *End = MBRef.getBufferEnd();

  // The size of the string that follows the header.
  uint32_t DataSize = read32le(Buf + offsetof(ImportHeader, SizeOfData));

  // Check if the total size is valid.
  if (size_t(End - Buf) != sizeof(ImportHeader) + DataSize) {
    llvm::errs() << "broken import library";
    return;
  }

  StringRef Name = Alloc.save(StringRef(Buf + sizeof(ImportHeader)));
  StringRef ImpName = Alloc.save(Twine("__imp_") + Name);
  StringRef DLLName(Buf + sizeof(ImportHeader) + Name.size() + 1);
  auto *ImpSym = new DefinedImportData(DLLName, ImpName, Name);
  Symbols.push_back(std::unique_ptr<DefinedImportData>(ImpSym));

  uint16_t TypeInfo = read16le(Buf + offsetof(ImportHeader, TypeInfo));
  int Type = TypeInfo & 0x3;
  if (Type == llvm::COFF::IMPORT_CODE)
    Symbols.push_back(llvm::make_unique<DefinedImportFunc>(Name, ImpSym));
}

SectionChunk::SectionChunk(ObjectFile *F, const coff_section *H, uint32_t SI)
    : File(F), Header(H), SectionIndex(SI) {
  File->COFFFile->getSectionName(Header, SectionName);
  if (!isBSS())
    File->COFFFile->getSectionContents(Header, Data);
  unsigned Shift = ((Header->Characteristics & 0x00F00000) >> 20) - 1;
  setAlign(uint32_t(1) << Shift);
}

const uint8_t *SectionChunk::getData() const {
  assert(!isBSS());
  return Data.data();
}

bool SectionChunk::isRoot() {
  return !(Header->Characteristics & llvm::COFF::IMAGE_SCN_CNT_CODE);
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
    assert(File->SymbolRefs[Rel->SymbolTableIndex]);
    auto *S = cast<Defined>(File->SymbolRefs[Rel->SymbolTableIndex]->Ptr);
    S->markLive();
  }
}

void SectionChunk::applyRelocations(uint8_t *Buffer) {
  DataRefImpl Ref;
  Ref.p = uintptr_t(Header);
  COFFObjectFile *FP = File->COFFFile.get();
  for (const auto &I : SectionRef(Ref, FP).relocations()) {
    const coff_relocation *Rel = FP->getCOFFRelocation(I);
    applyRelocation(Buffer, Rel);
  }
}

static void add16(uint8_t *L, int32_t V) { write16le(L, read16le(L) + V); }
static void add32(uint8_t *L, int32_t V) { write32le(L, read32le(L) + V); }
static void add64(uint8_t *L, int64_t V) { write64le(L, read64le(L) + V); }

void SectionChunk::applyRelocation(uint8_t *Buffer, const coff_relocation *Rel) {
  using namespace llvm::COFF;

  uint8_t *Off = Buffer + getFileOff() + Rel->VirtualAddress;
  auto *Sym = cast<Defined>(File->SymbolRefs[Rel->SymbolTableIndex]->Ptr);
  uint64_t S = Sym->getRVA();
  uint64_t P = getRVA() + Rel->VirtualAddress;
  if (Rel->Type == IMAGE_REL_AMD64_ADDR32) {
    add32(Off, ImageBase + S);
  } else if (Rel->Type == IMAGE_REL_AMD64_ADDR64) {
    add64(Off, ImageBase + S);
  } else if (Rel->Type == IMAGE_REL_AMD64_ADDR32NB) {
    add32(Off, S);
  } else if (Rel->Type == IMAGE_REL_AMD64_REL32) {
    add32(Off, S - P - 4);
  } else if (Rel->Type == IMAGE_REL_AMD64_REL32_1) {
    add32(Off, S - P - 5);
  } else if (Rel->Type == IMAGE_REL_AMD64_REL32_2) {
    add32(Off, S - P - 6);
  } else if (Rel->Type == IMAGE_REL_AMD64_REL32_3) {
    add32(Off, S - P - 7);
  } else if (Rel->Type == IMAGE_REL_AMD64_REL32_4) {
    add32(Off, S - P - 8);
  } else if (Rel->Type == IMAGE_REL_AMD64_REL32_5) {
    add32(Off, S - P - 9);
  } else if (Rel->Type == IMAGE_REL_AMD64_SECTION) {
    add16(Off, getOutputSection()->getSectionIndex());
  } else if (Rel->Type == IMAGE_REL_AMD64_SECREL) {
    add32(Off, S - getOutputSection()->getRVA());
  } else {
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
  uint32_t Operand = ImpSymbol->getRVA() - getRVA() - Data.size();
  write32le(Buffer + getFileOff() + 2, Operand);
}

OutputSection::OutputSection(StringRef N, uint32_t SI)
    : Name(N), SectionIndex(SI) {
  memset(&Header, 0, sizeof(Header));
  strncpy(Header.Name, Name.data(), std::min(Name.size(), size_t(8)));
}

void OutputSection::setRVA(uint64_t RVA) {
  Header.VirtualAddress = RVA;
  for (Chunk *C : Chunks)
    C->setRVA(C->getRVA() + RVA);
}

void OutputSection::setFileOffset(uint64_t Off) {
  Header.PointerToRawData = Off;
  for (Chunk *C : Chunks)
    C->setFileOff(C->getFileOff() + Off);
}

void OutputSection::addChunk(Chunk *C) {
  Chunks.push_back(C);
  uint64_t Off = Header.VirtualSize;
  Off = RoundUpToAlignment(Off, C->getAlign());
  C->setRVA(Off);
  C->setFileOff(Off);
  Off += C->getSize();
  Header.VirtualSize = Off;
  if (!C->isBSS())
    Header.SizeOfRawData = RoundUpToAlignment(Off, FileAlignment);
}

void OutputSection::addPermissions(uint32_t C) {
  Header.Characteristics = Header.Characteristics | (C & PermMask);
}

}
}
