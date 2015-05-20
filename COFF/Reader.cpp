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

DefinedRegular::DefinedRegular(ObjectFile *F, StringRef N, COFFSymbolRef SymRef)
  : Defined(DefinedRegularKind), File(F), Name(N), Sym(SymRef),
    Section(&File->Sections[Sym.getSectionNumber() - 1]) {}

bool DefinedRegular::isCOMDAT() const {
  return Section->isCOMDAT();
}

uint64_t DefinedRegular::getRVA() {
  return Section->getChunk()->getRVA() + Sym.getValue();
}

uint64_t DefinedRegular::getFileOff() {
  return Section->getChunk()->getFileOff() + Sym.getValue();
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

std::vector<Symbol *> ArchiveFile::getSymbols() {
  std::vector<Symbol *> Ret;
  for (const Archive::Symbol &Sym : File->symbols())
    Ret.push_back(new CanBeDefined(this, Sym));
  return Ret;
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

  if (auto EC = File->initSections())
    return EC;
  File->Symbols.resize(File->COFFFile->getNumberOfSymbols());
  return std::move(File);
}

std::vector<Symbol *> ObjectFile::getSymbols() {
  std::vector<Symbol *> Ret;
  for (uint32_t I = 0, E = COFFFile->getNumberOfSymbols(); I < E; ++I) {
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

    Symbol *P = nullptr;
    if (Sref.isUndefined()) {
      P = new Undefined(this, SymbolName);
    } else if (Sref.getSectionNumber() == -1) {
      // absolute symbol
    } else {
      P = new DefinedRegular(this, SymbolName, Sref);
    }
    if (P) {
      P->setSymbolRefAddress(&Symbols[I]);
      Ret.push_back(P);
    }
    I += Sref.getNumberOfAuxSymbols();
  }
  return Ret;
}

std::error_code ObjectFile::initSections() {
  COFFObjectFile *Obj = COFFFile.get();
  uint32_t NumSections = Obj->getNumberOfSections();
  Sections.reserve(NumSections);
  for (uint32_t I = 1; I < NumSections + 1; ++I) {
    const coff_section *Sec;
    if (auto EC = Obj->getSection(I, Sec))
      return EC;
    Sections.emplace_back(this, Sec);
  }
  return std::error_code();
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

  std::string Name = StringRef(Buf + sizeof(ImportHeader));
  StringRef DLLName(Buf + sizeof(ImportHeader) + Name.size() + 1);
  Symbols.push_back(new DefinedImplib(DLLName, *new std::string(Name)));
}

SectionChunk::SectionChunk(InputSection *S) : Section(S) {
  if (!isBSS())
    Section->getSectionContents(&Data);
  setAlign(S->getAlign());
}

const uint8_t *SectionChunk::getData() const {
  assert(!isBSS());
  return Data.data();
}

void SectionChunk::applyRelocations(uint8_t *Buffer) {
  Section->applyRelocations(Buffer);
}

bool SectionChunk::isBSS() const {
  return Section->isBSS();
}

size_t SectionChunk::getSize() const {
  return Section->getRawSize();
}

bool InputSection::isCOMDAT() const {
  return Header->Characteristics & llvm::COFF::IMAGE_SCN_LNK_COMDAT;
}

uint32_t InputSection::getAlign() const {
  unsigned Shift = ((Header->Characteristics & 0x00F00000) >> 20) - 1;
  return uint32_t(1) << Shift;
}

bool InputSection::isBSS() const {
  return Header->Characteristics & llvm::COFF::IMAGE_SCN_CNT_UNINITIALIZED_DATA;
}

void InputSection::applyRelocations(uint8_t *Buffer) {
  DataRefImpl Ref;
  Ref.p = uintptr_t(Header);
  COFFObjectFile *FP = File->COFFFile.get();
  for (const auto &I : SectionRef(Ref, FP).relocations()) {
    const coff_relocation *Rel = FP->getCOFFRelocation(I);
    applyRelocation(Buffer, Rel);
  }
}

void InputSection::getSectionContents(ArrayRef<uint8_t> *Data) {
  File->COFFFile->getSectionContents(Header, *Data);
}

static void add16(uint8_t *L, int32_t V) { write16le(L, read16le(L) + V); }
static void add32(uint8_t *L, int32_t V) { write32le(L, read32le(L) + V); }
static void add64(uint8_t *L, int64_t V) { write64le(L, read64le(L) + V); }

void InputSection::applyRelocation(uint8_t *Buffer, const coff_relocation *Rel) {
  using namespace llvm::COFF;
  const uint64_t ImageBase = 0x140000000;

  uint8_t *Off = Buffer + Chunk.getFileOff() + Rel->VirtualAddress;
  auto *Sym = cast<Defined>(File->Symbols[Rel->SymbolTableIndex]->Ptr);
  uint64_t S = Sym->getRVA();
  uint64_t P = Chunk.getRVA() + Rel->VirtualAddress;
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
    add16(Off, Out->getSectionIndex());
  } else if (Rel->Type == IMAGE_REL_AMD64_SECREL) {
    add32(Off, S - Out->getRVA());
  } else {
    llvm::report_fatal_error("Unsupported relocation type");
  }
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
  const int FileAlignment = 512;
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

void OutputSection::addPermission(uint32_t C) {
  Header.Characteristics = Header.Characteristics | (C & PermMask);
}

}
}
