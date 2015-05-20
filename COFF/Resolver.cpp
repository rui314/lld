//===- Resolver.cpp -------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Resolver.h"
#include "lld/Core/Error.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/raw_ostream.h"

using llvm::object::Binary;
using llvm::object::createBinary;
using llvm::sys::fs::file_magic;
using llvm::sys::fs::identify_magic;

namespace lld {
namespace coff {

std::error_code Resolver::addFile(std::unique_ptr<InputFile> File) {
  InputFile *P = File.release();
  if (auto *F = dyn_cast<ObjectFile>(P))
    return addFile(F);
  if (auto *F = dyn_cast<ArchiveFile>(P))
    return addFile(F);
  llvm_unreachable("unknown file type");
}

std::error_code Resolver::addFile(ObjectFile *File) {
  ObjectFiles.emplace_back(File);
  for (Symbol *Sym : File->getSymbols()) {
    StringRef Name = Sym->getName();
    if (Sym->isExternal()) {
      // Only externally-visible symbols are subjects of symbol
      // resolution.
      if (auto EC = resolve(Name, Sym))
	return EC;
      *Sym->SymbolRefPP = Symtab[Name];
    } else {
      *Sym->SymbolRefPP = new SymbolRef(Sym);
    }
  }
  return std::error_code();
}

std::error_code Resolver::addFile(ArchiveFile *File) {
  ArchiveFiles.emplace_back(File);
  for (Symbol *Sym : File->getSymbols())
    if (auto EC = resolve(Sym->getName(), Sym))
      return EC;
  return std::error_code();
}

bool Resolver::reportRemainingUndefines() {
  for (auto &I : Symtab) {
    SymbolRef *Ref = I.second;
    if (!dyn_cast<Undefined>(Ref->Ptr))
      continue;
    llvm::errs() << "undefined symbol: " << Ref->Ptr->getName() << "\n";
    return true;
  }
  return false;
}

// This function takes two arguments, an existing symbol and a new
// one, to resolve the conflict. Here is the decision table.
//
// [Undefined Undefined]
// There are two object files referencing the same undefined symbol.
// Undefined symbols don't have much identity, so a selection is
// arbitrary. We choose the existing one.
//
// [Undefined CanBeDefnied]
// There was a library file that could define a symbol, and we find an
// object file referencing the symbol as an undefined one. Read a
// member file from the library to make the can-be-defined symbol
// into an defined symbol.
//
// [Undefiend Defined]
// There was an object file referencing an undefined symbol, and the
// undefined symbol is now being resolved. Select the defined symbol.
//
// [CanBeDefined CanBeDefined]
// We have two libraries having the same symbol. We warn on that and
// choose the existing one.
//
// [CanBeDefined Defined]
// A symbol in an archive file is now resolved. Select the defined
// symbol.
//
// [Defined Defined]
// Resolve them according to COMDAT values if both are COMDAT symbols.
// It's an error if they are not COMDAT.
std::error_code Resolver::resolve(StringRef Name, Symbol *Sym) {
  if (Symtab.count(Name) == 0)
    Symtab[Name] = new SymbolRef();
  SymbolRef *Ref = Symtab[Name];

  // If nothing exists yet, just add a new one.
  if (Ref->Ptr == nullptr) {
    Ref->Ptr = Sym;
    return std::error_code();
  }

  if (isa<Undefined>(Ref->Ptr)) {
    if (isa<Undefined>(Sym))
      return std::error_code();
    if (auto *New = dyn_cast<CanBeDefined>(Sym))
      return addMemberFile(New);
    assert(isa<Defined>(Sym));
    Ref->Ptr = Sym;
    return std::error_code();
  }

  if (auto *Existing = dyn_cast<CanBeDefined>(Ref->Ptr)) {
    if (isa<Defined>(Sym)) {
      Ref->Ptr = Sym;
      return std::error_code();
    }
    if (isa<CanBeDefined>(Sym)) {
      llvm::errs() << "Two or more library files define the same symbol\n";
      return std::error_code();
    }
    assert(isa<Undefined>(Sym));
    return addMemberFile(Existing);
  }

  Defined *Existing = cast<Defined>(Ref->Ptr);
  if (isa<Undefined>(Sym) || isa<CanBeDefined>(Sym))
    return std::error_code();
  Defined *New = cast<Defined>(Sym);
  if (Existing->isCOMDAT() && New->isCOMDAT())
      return std::error_code();
  return make_dynamic_error_code(Twine("duplicate symbol: ") + Ref->Ptr->getName());
}

std::error_code Resolver::addMemberFile(CanBeDefined *Sym) {
  auto MBRefOrErr = Sym->getMember();
  if (auto EC = MBRefOrErr.getError())
    return EC;
  MemoryBufferRef MBRef = MBRefOrErr.get();

  // getMember returns an empty buffer if the member was already
  // read from the library.
  if (MBRef.getBuffer().empty())
    return std::error_code();

  file_magic Magic = identify_magic(StringRef(MBRef.getBuffer()));
  if (Magic == file_magic::coff_import_library) {
    auto SymOrErr = readImplib(MBRef);
    if (auto EC = SymOrErr.getError())
      return EC;
    DefinedImplib *Imp = SymOrErr.get();
    ImpSyms.push_back(Imp);
    return resolve(Imp->Name, Imp);
  }

  if (Magic != file_magic::coff_object)
    return make_dynamic_error_code("unknown file type");

  StringRef Filename = MBRef.getBufferIdentifier();
  auto FileOrErr = ObjectFile::create(Filename, MBRef);
  if (auto EC = FileOrErr.getError())
    return EC;
  std::unique_ptr<ObjectFile> Obj = std::move(FileOrErr.get());
  return addFile(std::move(Obj));
}

uint64_t Resolver::getRVA(StringRef Symbol) {
  auto It = Symtab.find(Symbol);
  if (It == Symtab.end())
    return 0;
  SymbolRef *Ref = It->second;
  return cast<DefinedRegular>(Ref->Ptr)->getRVA();
}

} // namespace coff
} // namespace lld
