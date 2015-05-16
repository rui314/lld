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
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using llvm::object::Binary;
using llvm::object::createBinary;

namespace lld {
namespace coff {

std::error_code Resolver::addFile(std::unique_ptr<ObjectFile> File) {
  ObjectFile *FileP = File.get();
  Files.push_back(std::move(File));
  COFFObjectFile *Obj = FileP->COFFFile.get();

  // Resolve symbols
  uint32_t NumSymbols = Obj->getNumberOfSymbols();
  FileP->Symbols.resize(NumSymbols);
  for (uint32_t I = 0; I < NumSymbols; ++I) {
    // Get a COFFSymbolRef object.
    auto SrefOrErr = Obj->getSymbol(I);
    if (auto EC = SrefOrErr.getError())
      return EC;
    COFFSymbolRef Sref = SrefOrErr.get();

    // Get a symbol name.
    StringRef Name;
    if (auto EC = Obj->getSymbolName(Sref, Name))
      return EC;
    
    // Only externally-visible symbols should be subjects of symbol
    // resolution. If it's internal, don't add that to the symbol
    // table.
    if (!Sref.isExternal()) {
      // We still want to add all symbols, including internal ones, to
      // the symbol vector because any symbols can be referred by
      // relocations.
      llvm::dbgs() << "Add " << Name << "\n";
      SymbolRef Ref(Name, nullptr);
      FileP->Symbols[I] = &Ref;
    } else {
      // We now have an externally-visible symbol. Create a symbol
      // wrapper object and add it to the symbol table if there's no
      // existing one. If there's an existing one, resolve the conflict.
      if (Symtab.count(Name) == 0)
	Symtab[Name] = SymbolRef(Name, nullptr);
      SymbolRef *Ref = &Symtab[Name];
      Symbol *Sym = createSymbol(FileP, Sref);
      if (auto EC = resolve(Ref, Sym))
	return EC;
      FileP->Symbols[I] = Ref;
    }
    I += Sref.getNumberOfAuxSymbols();
  }
  return std::error_code();
}

std::error_code Resolver::addFile(std::unique_ptr<ArchiveFile> File) {
  ArchiveFile *FileP = File.get();
  Archives.push_back(std::move(File));
  Archive *Arc = FileP->File.get();
  for (const Archive::Symbol &ArcSym : Arc->symbols()) {
    StringRef Name = ArcSym.getName();
    if (Symtab.count(Name) == 0)
      Symtab[Name] = SymbolRef(Name, nullptr);
    SymbolRef *Ref = &Symtab[Name];
    Symbol *Sym = new (Alloc) CanBeDefined(FileP, &ArcSym);
    if (auto EC = resolve(Ref, Sym))
      return EC;
  }
  return std::error_code();
}

bool Resolver::reportRemainingUndefines() {
  for (auto &I : Symtab) {
    SymbolRef Ref = I.second;
    if (!dyn_cast<Undefined>(Ref.Ptr))
      continue;
    llvm::errs() << "undefined symbol: " << Ref.Name << "\n";
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
std::error_code
Resolver::resolve(SymbolRef *Ref, Symbol *Sym) {
  llvm::dbgs() << "Resolving " << Ref->Name << "\n";
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
  if (Existing->IsCOMDAT() && New->IsCOMDAT())
      return std::error_code();
  return make_dynamic_error_code(Twine("Duplicate symbol: ") + Ref->Name);
}

std::error_code Resolver::addMemberFile(CanBeDefined *Sym) {
  ErrorOr<std::unique_ptr<ObjectFile>> MemberOrErr = Sym->getMember();
  if (auto EC = MemberOrErr.getError())
    return EC;
  std::unique_ptr<ObjectFile> MemberFile = std::move(MemberOrErr.get());
  if (!MemberFile) {
    // getMember returns a nullptr if the member was already read
    // from the library. We add symbols to the symbol table
    // one-by-one, and we could observe a transient state that an
    // in-library symbol is being resolved but not added to the
    // symbol table yet. If that's the case, we'll just let them
    // go. Do nothing.
    return std::error_code();
  }
  return addFile(std::move(MemberFile));
}

Symbol *Resolver::createSymbol(ObjectFile *File, COFFSymbolRef Sym) {
  if (Sym.isUndefined())
    return new (Alloc) Undefined(File);
  return new (Alloc) DefinedRegular(File, Sym);
}

uint64_t Resolver::getRVA(StringRef Symbol) {
  auto It = Symtab.find(Symbol);
  if (It == Symtab.end())
    return 0;
  SymbolRef Ref = It->second;
  return cast<DefinedRegular>(Ref.Ptr)->getRVA();
}

} // namespace coff
} // namespace lld
