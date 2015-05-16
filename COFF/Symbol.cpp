//===- Symbol.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Symbol.h"
#include "lld/Core/Error.h"
#include "llvm/ADT/STLExtras.h"

using llvm::object::Binary;
using llvm::object::createBinary;

namespace lld {
namespace coff {

Defined::Defined(ObjectFile *F, COFFSymbolRef SymRef)
  : Symbol(DefinedKind), File(F), Sym(SymRef),
    Section(&File->Sections[Sym.getSectionNumber() - 1]) {}

bool Defined::IsCOMDAT() const {
  return Section && Section->IsCOMDAT();
}

ErrorOr<std::unique_ptr<ObjectFile>> CanBeDefined::getMember() {
  return File->getMember(Sym);
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

ErrorOr<std::unique_ptr<ObjectFile>>
ArchiveFile::getMember(Archive::Symbol *Sym) {
  auto ItOrErr = Sym->getMember();
  if (auto EC = ItOrErr.getError())
    return EC;
  Archive::child_iterator It = ItOrErr.get();

  const char *StartAddr = It->getBuffer().data();
  if (Seen.count(StartAddr))
    return nullptr;
  Seen.insert(StartAddr);

  auto MBRefOrErr = It->getMemoryBufferRef();
  if (auto EC = MBRefOrErr.getError())
    return EC;
  MemoryBufferRef MBRef = MBRefOrErr.get();

  ErrorOr<StringRef> NameOrErr = It->getName();
  if (auto EC = NameOrErr.getError())
    return EC;
  auto FileOrErr = ObjectFile::create(NameOrErr.get(), MBRef);
  if (auto EC = FileOrErr.getError())
    return EC;
  return std::move(FileOrErr.get());
}

ErrorOr<std::unique_ptr<ObjectFile>> ObjectFile::create(StringRef Path) {
  auto MBOrErr = MemoryBuffer::getFile(Path);
  if (auto EC = MBOrErr.getError())
    return EC;
  std::unique_ptr<MemoryBuffer> MB = std::move(MBOrErr.get());

  auto ObjectFileOrErr = create(Path, MB->getMemBufferRef());
  if (auto EC = ObjectFileOrErr.getError())
    return EC;
  std::unique_ptr<ObjectFile> File = std::move(ObjectFileOrErr.get());

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
  std::unique_ptr<COFFObjectFile> File(static_cast<COFFObjectFile *>(Bin.release()));
  return std::unique_ptr<ObjectFile>(new ObjectFile(Path, std::move(File)));
}

}
}
