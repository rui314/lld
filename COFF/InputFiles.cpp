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

ErrorOr<std::unique_ptr<InputFile>> CanBeDefined::getMember() {
  auto MBRefOrErr = File->getMember(&Sym);
  if (auto EC = MBRefOrErr.getError())
    return EC;
  MemoryBufferRef MBRef = MBRefOrErr.get();

  // getMember returns an empty buffer if the member was already
  // read from the library.
  if (MBRef.getBuffer().empty())
    return nullptr;

  file_magic Magic = identify_magic(MBRef.getBuffer());
  if (Magic == file_magic::coff_import_library)
    return llvm::make_unique<ImportFile>(MBRef);

  if (Magic != file_magic::coff_object)
    return make_dynamic_error_code("unknown file type");

  StringRef Filename = MBRef.getBufferIdentifier();
  ErrorOr<std::unique_ptr<ObjectFile>> FileOrErr = ObjectFile::create(Filename, MBRef);
  if (auto EC = FileOrErr.getError())
    return EC;
  return std::move(FileOrErr.get());
}

bool Undefined::replaceWeakExternal() {
  if (!WeakExternal)
    return false;
  getSymbolRef()->Ptr = WeakExternal->getSymbolRef()->Ptr;
  return true;
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
    return Name.lower();
  return StringRef((basename(ParentName) + "(" + basename(Name) + ")").str()).lower();
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
    if (Name == ".drectve") {
      ArrayRef<uint8_t> Data;
      COFFFile->getSectionContents(Sec, Data);
      Directives = StringRef((char *)Data.data(), Data.size()).trim();
      continue;
    }
    if (Name.startswith(".debug"))
      continue;
    if (Sec->Characteristics & llvm::COFF::IMAGE_SCN_LNK_REMOVE)
      continue;
    Chunks[I].reset(new SectionChunk(this, Sec, I));
  }
}

void ObjectFile::initializeSymbols() {
  uint32_t NumSymbols = COFFFile->getNumberOfSymbols();
  SymbolRefs.resize(NumSymbols);
  std::vector<Symbol *> CreatedSyms(NumSymbols);
  int32_t LastSectionNumber = 0;
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
    if (SymbolName == "@comp.id" || SymbolName == "@feat.00")
      continue;

    const void *AuxP = nullptr;
    if (Sref.getNumberOfAuxSymbols())
      AuxP = COFFFile->getSymbol(I + 1)->getRawPtr();

    std::unique_ptr<Symbol> Sym;
    if (Sref.isUndefined()) {
      Sym.reset(new Undefined(SymbolName));
    } else if (Sref.isCommon()) {
      Chunk *C = new CommonChunk(Sref);
      Chunks.push_back(std::unique_ptr<Chunk>(C));
      Sym.reset(new DefinedRegular(this, SymbolName, Sref, C));
    } else if (Sref.getSectionNumber() == -1) {
      Sym.reset(new DefinedAbsolute(SymbolName, Sref.getValue()));
    } else if (Sref.isWeakExternal()) {
      auto *Aux = (const coff_aux_weak_external *)AuxP;
      Sym.reset(new Undefined(SymbolName, CreatedSyms[Aux->TagIndex]));
    } else {
      bool IsFirst = (LastSectionNumber != Sref.getSectionNumber());
      if (IsFirst && AuxP) {
        if (std::unique_ptr<Chunk> &C = Chunks[Sref.getSectionNumber()]) {
          auto *SC = (SectionChunk *)C.get();
          auto *Aux = (coff_aux_section_definition *)AuxP;
          auto *Parent = (SectionChunk *)(Chunks[Aux->getNumber(Sref.isBigObj())].get());
          if (Parent)
            Parent->addAssociative(SC);
        }
      }
      if (std::unique_ptr<Chunk> &C = Chunks[Sref.getSectionNumber()])
        Sym.reset(new DefinedRegular(this, SymbolName, Sref, C.get()));
    }
    if (Sym) {
      Sym->setSymbolRefAddress(&SymbolRefs[I]);
      CreatedSyms[I] = Sym.get();
      Symbols.push_back(std::move(Sym));
    }
    I += Sref.getNumberOfAuxSymbols();
    LastSectionNumber = Sref.getSectionNumber();
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

StringRef ImportFile::getName() {
  return MBRef.getBufferIdentifier();
}

ImportFile::ImportFile(MemoryBufferRef M)
    : InputFile(ImplibKind), MBRef(M) {
  readImplib();
}

void ImportFile::readImplib() {
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

}
}
