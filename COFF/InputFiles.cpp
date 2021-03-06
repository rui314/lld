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

// Returns the last element of a path, which is supposed to be a filename.
static StringRef getBasename(StringRef Path) {
  size_t Pos = Path.rfind('\\');
  if (Pos == StringRef::npos)
    return Path;
  return Path.substr(Pos + 1);
}

// Returns a string in the format of "foo.obj" or "foo.obj(bar.lib)".
std::string InputFile::getShortName() {
  if (ParentName == "")
    return getName().lower();
  std::string Res = (getBasename(ParentName) + "(" +
                     getBasename(getName()) + ")").str();
  return StringRef(Res).lower();
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

  // Allocate a buffer for Lazy objects.
  size_t BufSize = File->getNumberOfSymbols() * sizeof(Lazy);
  Lazy *Buf = (Lazy *)Alloc.Allocate(BufSize, llvm::alignOf<Lazy>());

  // Read the symbol table to construct Lazy objects.
  uint32_t I = 0;
  for (const Archive::Symbol &Sym : File->symbols()) {
    // Skip special symbol exists in import library files.
    if (Sym.getName() == "__NULL_IMPORT_DESCRIPTOR")
      continue;
    SymbolBodies.push_back(new (&Buf[I++]) Lazy(this, Sym));
  }
  return std::error_code();
}

// Returns a buffer pointing to a member file containing a given symbol.
ErrorOr<MemoryBufferRef> ArchiveFile::getMember(const Archive::Symbol *Sym) {
  auto ItOrErr = Sym->getMember();
  if (auto EC = ItOrErr.getError())
    return EC;
  Archive::child_iterator It = ItOrErr.get();

  // Return an empty buffer if we have already returned the same buffer.
  const char *StartAddr = It->getBuffer().data();
  auto Pair = Seen.insert(StartAddr);
  if (!Pair.second)
    return MemoryBufferRef();
  return It->getMemoryBufferRef();
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

  if (auto *Obj = dyn_cast<COFFObjectFile>(Bin.get())) {
    Bin.release();
    COFFObj.reset(Obj);
  } else {
    return make_dynamic_error_code(Twine(Name) + " is not a COFF file.");
  }

  // Read section and symbol tables.
  if (auto EC = initializeChunks())
    return EC;
  return initializeSymbols();
}

SymbolBody *ObjectFile::getSymbolBody(uint32_t SymbolIndex) {
  return SparseSymbolBodies[SymbolIndex]->getReplacement();
}

std::error_code ObjectFile::initializeChunks() {
  uint32_t NumSections = COFFObj->getNumberOfSections();
  Chunks.reserve(NumSections);
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
    auto *C = new (Alloc) SectionChunk(this, Sec, I);
    Chunks.push_back(C);
    SparseChunks[I] = C;
  }
  return std::error_code();
}

std::error_code ObjectFile::initializeSymbols() {
  uint32_t NumSymbols = COFFObj->getNumberOfSymbols();
  SymbolBodies.reserve(NumSymbols);
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

    SymbolBody *Body = createSymbolBody(SymbolName, Sym, AuxP, IsFirst);
    if (Body) {
      SymbolBodies.push_back(Body);
      SparseSymbolBodies[I] = Body;
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
    Chunk *C = new (Alloc) CommonChunk(Sym);
    Chunks.push_back(C);
    return new (Alloc) DefinedRegular(Name, Sym, C);
  }
  if (Sym.isAbsolute())
    return new (Alloc) DefinedAbsolute(Name, Sym.getValue());
  // TODO: Handle IMAGE_WEAK_EXTERN_SEARCH_ALIAS
  if (Sym.isWeakExternal()) {
    auto *Aux = (const coff_aux_weak_external *)AuxP;
    return new (Alloc) Undefined(Name, &SparseSymbolBodies[Aux->TagIndex]);
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
    return new (Alloc) DefinedRegular(Name, Sym, C);
  return nullptr;
}

std::error_code ImportFile::parse() {
  const char *Buf = MBRef.getBufferStart();
  const char *End = MBRef.getBufferEnd();
  const auto *Hdr = reinterpret_cast<const coff_import_header *>(Buf);

  // Check if the total size is valid.
  if (End - Buf != sizeof(*Hdr) + Hdr->SizeOfData)
    return make_dynamic_error_code("broken import library");

  // Read names and create an __imp_ symbol.
  StringRef Name = StringAlloc.save(StringRef(Buf + sizeof(*Hdr)));
  StringRef ImpName = StringAlloc.save(Twine("__imp_") + Name);
  StringRef DLLName(Buf + sizeof(coff_import_header) + Name.size() + 1);
  auto *ImpSym = new (Alloc) DefinedImportData(DLLName, ImpName, Name);
  SymbolBodies.push_back(ImpSym);

  // If type is function, we need to create a thunk which jump to an
  // address pointed by the __imp_ symbol. (This allows you to call
  // DLL functions just like regular non-DLL functions.)
  if (Hdr->getType() == llvm::COFF::IMPORT_CODE)
    SymbolBodies.push_back(new (Alloc) DefinedImportThunk(Name, ImpSym));
  return std::error_code();
}

} // namespace coff
} // namespace lld
