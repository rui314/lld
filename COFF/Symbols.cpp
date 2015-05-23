//===- Symbols.cpp --------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "InputFiles.h"
#include "Symbols.h"
#include "lld/Core/Error.h"
#include "llvm/ADT/STLExtras.h"

using namespace llvm::object;
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
  if (!WeakExternal || !*WeakExternal)
    return false;
  getSymbolRef()->Ptr = (*WeakExternal)->getSymbolRef()->Ptr;
  return true;
}

}
}
