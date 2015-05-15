//===- Resolver.cpp -------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Resolver.h"

using llvm::object::Binary;
using llvm::object::createBinary;

namespace lld {
namespace coff {

std::error_code Resolver::addFile(ObjectFile *File) {
  // Initialize File->Sections.
  COFFObjectFile *Obj = File->COFFFile;
  uint32_t NumSections = Obj->getNumberOfSections();
  Sections.reserve(NumSections + 1);
  // Sections are 1-based. Add a null entry to the beginning.
  Sections.emplace_back(nullptr, nullptr);
  for (int32_t I = 1; I < NumSections + 1; ++I) {
    const coff_section *Sec;
    if (auto EC = Obj->getSection(I, Sec))
      return EC;
    Sections.emplace_back(Obj, Sec);
  }

  // Resolve symbols
  uint32_t NumSymbols = Obj->getNumberOfSymbols();
  Symbols.reserve(NumSymbols);
  for (uint32_t I = 0; I < NumSymbols; ++I) {
    // Get a COFFSymbolRef object.
    COFFObjectFile *Obj = File->COFFFile;
    auto SymOrErr = File.getSymbol(SymbolIndex);
    if (auto EC = SymOrErr.getError())
      return EC;
    COFFSymbolRef Sym = SymOrErr.get();

    // Get a symbol name.
    StringRef Name;
    if (auto EC = Obj->getSymbolName(Sym, Name))
      return EC;
    
    // Only externally-visible symbols should be subjects of symbol
    // resolution. If it's internal, don't add that to the symbol
    // table.
    if (!Sym->isExternal()) {
      // We still want to add all symbols, including internal ones, to
      // the symbol vector because any symbols can be referred by
      // relocations.
      SymbolRef Ref = { Name, nullptr };
      File->Symbols.push_back(Ref);
      continue;
    }

    // We now have an externally-visible symbol. Create a symbol
    // wrapper object and add it to the symbol table if there's no
    // existing one. If there's an existing one, resolve the conflict.
    auto SymOrErr = createSymbol(FileP, I);
    if (auto EC = SymOrErr.getError())
      return EC;
    Symbol *Sym = SymOrErr.get();

    SymbolRef *Ref = &Symtab[Name];
    if (auto EC = resolve(Ref, Sym))
      return EC;
    File->Symbols.push_back(Ref);
  }

  Files.push_back(std::move(ObjFile));
}

bool Resolver::reportRemainingUndefines() {
  for (auto &I : Symtab) {
    SymbolRef Ref = I.second;
    if (!dyn_cast<Undefined>(Ref.Ptr))
      continue;
    llvm::err() << "undefined symbol: " << Ref.Name << "\n";
    return false;
  }
  return true;
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
std::error_code resolve(SymbolRef *Ref, Symbol *Sym) {
  // If nothing exists yet, just add a new one.
  if (Ref.Ptr == nullptr) {
    Ref.Ptr = Sym;
    return std::error_code();
  }

  if (isa<Undefined>(Ref.Ptr)) {
    if (isa<Undefined>(Sym))
      return std::error_code();
    if (isa<CanBeDefined>(Sym)) {
      ObjectFile *MemberFile = Sym->getMember();
      if (!MemberFile) {
	// getMember returns a nullptr if the member was already read
	// from the library. We add symbols to the symbol table
	// one-by-one, and we could observe a transient state that an
	// in-library symbol is being resolved but not added to the
	// symbol table yet. If that's the case, we'll just let them
	// go. Do nothing.
	return std::error_code();
      }
      return addFile(MemberFile);
    }
    assert(isa<Defined>(Sym));
    Ref.Ptr = Sym;
    return std::error_code();
  }

  if (CanBeDefined *Existing = dyn_cast<CanBeDefined>(Ref.Ptr)) {
    if (isa<Defined>(Sym)) {
      Ref.Ptr = Sym;
      return std::error_code();
    }
    if (isa<CanBeDefined>(Sym)) {
      llvm::err() << "Two or more library files define the same symbol\n";
      return std::error_code();
    }
    assert(isa<Undefined>(Sym));
    ObjectFile *MemberFile = Existing->getMember();
    if (!MemberFile)
      return std::error_code();
    return addFile(MemberFile);
  }

  Defined *Existing = cast<Defined>(Ref.Ptr);
  if (isa<Undefined>(Sym) || isa<CanBeDefined>(Sym))
    return std::error_code();
  Defined *New = cast<Defined>(Sym);
  if (Existing->IsCOMDAT() && New->IsCOMDAT)
      return std::error_code();
  return make_dynamic_error_code(Twine("Duplicate symbol: ") + Ref->Name);
}

ErrorOr<SymbolRef *> createSymbol(ObjectFile *File, COFFSymbolRef Sym) {
  if (Sym.isUndefined())
    return new (Alloc) Undefined(File);
  return new (Alloc) Defined(File, Sym);
}

} // namespace coff
} // namespace lld
