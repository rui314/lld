//===- Symbol.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Symbol.h"

using llvm::object::Binary;
using llvm::object::createBinary;

namespace lld {

static ErrorOr<std::unique_ptr<ArchiveFile>> ArchiveFile::create(StringRef Path) {
  auto MBOrErr = MemoryBuffer::getFile(Path);
  if (auto EC = MBOrErr.getError())
    return EC;
  std::unique_ptr<MemoryBuffer> MB = std::move(MBOrErr.get());

  auto ArchiveOrErr = Archive::create(MB->getMemBufferRef());
  if (auto EC = ArchiveOrErr.getError())
    return EC;
  std::unique<Archive> File = std::move(ArchiveOrErr.get());

  return llvm::make_unique<ArchiveFile>(Path, std::move(File), std::move(MB));
}

ErrorOr<ObjectFile *> ArchiveFile::getMember(Archive::Symbol *Sym) {
  auto ItOrErr = Sym.getMember();
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
  MemoryBufferRef MBRef = MBOrErr.get();

  auto FileOrErr = ObjectFile::create(It->getName, MBRef);
  if (auto EC = FileOrErr.getError())
    return EC;
  std::unique_ptr<ObjectFile> File = std::move(FileOrErr.get());

  ObjectFile *P = File.get();
  Members.push_back(std::move(File));
  return P;
}

static ErrorOr<std::unique_ptr<ObjectFile>> ObjectFile::create(StringRef Path) {
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

  return File;
}

static ErrorOr<std::unique_ptr<ObjectFile>>
ObjectFile::create(StringRef Path, MemoryBufferRef MBRef) {
  auto BinOrErr = createBinary(MBRef);
  if (auto EC = BinOrErr.getError())
    return EC;
  std::unique_ptr<Binary> Bin = std::move(BinOrErr.get());

  if (!isa<COFFObjectFile>(Bin.get()))
    return lld::make_dynamic_error_code(Twine(Path) + " is not a COFF file.");
  std::unique_ptr<COFFObjectFile> File(static_cast<COFFObjectFile *>(Bin.release()));
  return llvm::make_unique<ObjectFile>(Path, std::move(File)));
}

}
