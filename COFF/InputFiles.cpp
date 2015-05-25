//===- InputFiles.cpp -----------------------------------------------------===//
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
using llvm::sys::fs::identify_magic;
using llvm::sys::fs::file_magic;

namespace lld {
namespace coff {

static StringRef basename(StringRef Path) {
  size_t Pos = Path.rfind('\\');
  if (Pos == StringRef::npos)
    return Path;
  return Path.substr(Pos + 1);
}

std::string InputFile::getShortName() {
  StringRef Name = getName();
  if (ParentName == "")
    return Name.lower();
  return StringRef((basename(ParentName) + "(" + basename(Name) + ")").str())
      .lower();
}

std::error_code ArchiveFile::parse() {
  // Get a memory buffer.
  auto MBOrErr = MemoryBuffer::getFile(Name);
  if (auto EC = MBOrErr.getError())
    return EC;
  MB = std::move(MBOrErr.get());

  // Parse a memory buffer as an archive file.
  auto ArchiveOrErr = Archive::create(MB->getMemBufferRef());
  if (auto EC = ArchiveOrErr.getError())
    return EC;
  File = std::move(ArchiveOrErr.get());

  // Read the symbol table to construct CanBeDefined symbols.
  for (const Archive::Symbol &Sym : File->symbols()) {
    // Skip special symbol exists in import library files.
    if (Sym.getName() == "__NULL_IMPORT_DESCRIPTOR")
      continue;
    SymbolBodies.push_back(llvm::make_unique<CanBeDefined>(this, Sym));
  }
  return std::error_code();
}

ErrorOr<MemoryBufferRef> ArchiveFile::getMember(const Archive::Symbol *Sym) {
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

std::error_code ObjectFile::parse() {
  // MBRef is not initialized if this is not an archive member.
  if (MBRef.getBuffer().empty()) {
    auto MBOrErr = MemoryBuffer::getFile(Name);
    if (auto EC = MBOrErr.getError())
      return EC;
    MB = std::move(MBOrErr.get());
    MBRef = MB->getMemBufferRef();
  }

  // Parse a memory buffer as a COFF file.
  auto BinOrErr = createBinary(MBRef);
  if (auto EC = BinOrErr.getError())
    return EC;
  std::unique_ptr<Binary> Bin = std::move(BinOrErr.get());

  if (!isa<COFFObjectFile>(Bin.get()))
    return make_dynamic_error_code(Twine(Name) + " is not a COFF file.");
  COFFObj.reset(cast<COFFObjectFile>(Bin.release()));

  // Read section and symbol tables.
  if (auto EC = initializeChunks())
    return EC;
  if (auto EC = initializeSymbols())
    return EC;
  return std::error_code();
}

Symbol *ObjectFile::getSymbol(uint32_t SymbolIndex) {
  return SparseSymbolBodies[SymbolIndex]->getSymbol();
}

std::error_code ObjectFile::initializeChunks() {
  uint32_t NumSections = COFFObj->getNumberOfSections();
  SparseChunks.resize(NumSections + 1);
  for (uint32_t I = 1; I < NumSections + 1; ++I) {
    const coff_section *Sec;
    StringRef Name;
    if (auto EC = COFFObj->getSection(I, Sec))
      return make_dynamic_error_code(Twine("getSection failed: ") + Name +
                                     ": " + EC.message());
    if (auto EC = COFFObj->getSectionName(Sec, Name))
      return make_dynamic_error_code(Twine("getSectionName failed: ") + Name +
                                     ": " + EC.message());
    if (Name == ".drectve") {
      ArrayRef<uint8_t> Data;
      COFFObj->getSectionContents(Sec, Data);
      Directives = StringRef((char *)Data.data(), Data.size()).trim();
      continue;
    }
    if (Name.startswith(".debug"))
      continue;
    if (Sec->Characteristics & llvm::COFF::IMAGE_SCN_LNK_REMOVE)
      continue;
    auto *C = new SectionChunk(this, Sec, I);
    Chunks.push_back(std::unique_ptr<SectionChunk>(C));
    SparseChunks[I] = C;
  }
  return std::error_code();
}

std::error_code ObjectFile::initializeSymbols() {
  uint32_t NumSymbols = COFFObj->getNumberOfSymbols();
  SparseSymbolBodies.resize(NumSymbols);
  int32_t LastSectionNumber = 0;
  for (uint32_t I = 0; I < NumSymbols; ++I) {
    // Get a COFFSymbolRef object.
    auto SymOrErr = COFFObj->getSymbol(I);
    if (auto EC = SymOrErr.getError())
      return make_dynamic_error_code(Twine("broken object file: ") + Name +
                                     ": " + EC.message());
    COFFSymbolRef Sym = SymOrErr.get();

    // Get a symbol name.
    StringRef SymbolName;
    if (auto EC = COFFObj->getSymbolName(Sym, SymbolName))
      return make_dynamic_error_code(Twine("broken object file: ") + Name +
                                     ": " + EC.message());
    // Skip special symbols.
    if (SymbolName == "@comp.id" || SymbolName == "@feat.00")
      continue;

    const void *AuxP = nullptr;
    if (Sym.getNumberOfAuxSymbols())
      AuxP = COFFObj->getSymbol(I + 1)->getRawPtr();
    bool IsFirst = (LastSectionNumber != Sym.getSectionNumber());

    std::unique_ptr<SymbolBody> Body(
        createSymbolBody(SymbolName, Sym, AuxP, IsFirst));
    if (Body) {
      SparseSymbolBodies[I] = Body.get();
      SymbolBodies.push_back(std::move(Body));
    }
    I += Sym.getNumberOfAuxSymbols();
    LastSectionNumber = Sym.getSectionNumber();
  }
  return std::error_code();
}

SymbolBody *ObjectFile::createSymbolBody(StringRef Name, COFFSymbolRef Sym,
                                         const void *AuxP, bool IsFirst) {
  if (Sym.isUndefined())
    return new Undefined(Name);
  if (Sym.isCommon()) {
    Chunk *C = new CommonChunk(Sym);
    Chunks.push_back(std::unique_ptr<Chunk>(C));
    return new DefinedRegular(this, Name, Sym, C);
  }
  if (Sym.getSectionNumber() == -1) {
    return new DefinedAbsolute(Name, Sym.getValue());
  }
  if (Sym.isWeakExternal()) {
    auto *Aux = (const coff_aux_weak_external *)AuxP;
    return new Undefined(Name, &SparseSymbolBodies[Aux->TagIndex]);
  }
  if (IsFirst && AuxP) {
    if (Chunk *C = SparseChunks[Sym.getSectionNumber()]) {
      auto *Aux = (coff_aux_section_definition *)AuxP;
      auto *Parent =
          (SectionChunk *)(SparseChunks[Aux->getNumber(Sym.isBigObj())]);
      if (Parent)
        Parent->addAssociative((SectionChunk *)C);
    }
  }
  if (Chunk *C = SparseChunks[Sym.getSectionNumber()])
    return new DefinedRegular(this, Name, Sym, C);
  return nullptr;
}

void ImportFile::readImports() {
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
  SymbolBodies.push_back(std::unique_ptr<DefinedImportData>(ImpSym));

  uint16_t TypeInfo = read16le(Buf + offsetof(ImportHeader, TypeInfo));
  int Type = TypeInfo & 0x3;
  if (Type == llvm::COFF::IMPORT_CODE)
    SymbolBodies.push_back(llvm::make_unique<DefinedImportFunc>(Name, ImpSym));
}
}
}
