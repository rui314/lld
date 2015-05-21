//===- SymbolTable.cpp ----------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "SymbolTable.h"
#include "lld/Core/Error.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

namespace lld {
namespace coff {

bool parseDirectives(StringRef S, std::vector<std::unique_ptr<InputFile>> *Res);

SymbolTable::SymbolTable() {
  Symtab["__ImageBase"] = new SymbolRef(new DefinedAbsolute("__ImageBase", ImageBase));
  Symtab["mainCRTStartup"] = new SymbolRef(new Undefined("mainCRTStartup"));
}

std::error_code SymbolTable::addFile(std::unique_ptr<InputFile> File) {
  InputFile *P = File.release();
  if (auto *F = dyn_cast<ObjectFile>(P))
    return addFile(F);
  if (auto *F = dyn_cast<ArchiveFile>(P))
    return addFile(F);
  if (auto *F = dyn_cast<ImplibFile>(P))
    return addFile(F);
  llvm_unreachable("unknown file type");
}

std::error_code SymbolTable::addFile(ObjectFile *File) {
  ObjectFiles.emplace_back(File);
  for (Symbol *Sym : File->getSymbols()) {
    if (Sym->isExternal()) {
      // Only externally-visible symbols are subjects of symbol
      // resolution.
      if (auto EC = resolve(Sym))
        return EC;
      Sym->setSymbolRef(Symtab[Sym->getName()]);
    } else {
      Sym->setSymbolRef(new SymbolRef(Sym));
    }
  }

  StringRef Dir = File->getDirectives();
  if (!Dir.empty()) {
    std::vector<std::unique_ptr<InputFile>> Libs;
    if (!parseDirectives(Dir, &Libs))
      return std::error_code();
    for (std::unique_ptr<InputFile> &L : Libs)
      addFile(std::move(L));
  }
  
  return std::error_code();
}

std::error_code SymbolTable::addFile(ArchiveFile *File) {
  ArchiveFiles.emplace_back(File);
  for (Symbol *Sym : File->getSymbols())
    if (auto EC = resolve(Sym))
      return EC;
  return std::error_code();
}

std::error_code SymbolTable::addFile(ImplibFile *File) {
  ImplibFiles.emplace_back(File);
  for (Symbol *Sym : File->getSymbols())
    if (auto EC = resolve(Sym))
      return EC;
  return std::error_code();
}

bool SymbolTable::reportRemainingUndefines() {
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
// Select one of them if they are Common or COMDAT symbols.
std::error_code SymbolTable::resolve(Symbol *Sym) {
  StringRef Name = Sym->getName();
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
      // llvm::errs() << "Two or more library files define the same symbol: "
      //              << Sym->getName() << "\n";
      return std::error_code();
    }
    assert(isa<Undefined>(Sym));
    return addMemberFile(Existing);
  }

  Defined *Existing = cast<Defined>(Ref->Ptr);
  if (isa<Undefined>(Sym) || isa<CanBeDefined>(Sym))
    return std::error_code();
  Defined *New = cast<Defined>(Sym);

  // Common symbols
  if (Existing->isCommon()) {
    if (New->isCommon()) {
      if (Existing->getCommonSize() < New->getCommonSize())
        Ref->Ptr = New;
      return std::error_code();
    }
    Ref->Ptr = New;
    return std::error_code();
  }
  if (New->isCommon())
    return std::error_code();

  // COMDAT symbols
  if (Existing->isCOMDAT() && New->isCOMDAT())
      return std::error_code();
  return make_dynamic_error_code(Twine("duplicate symbol: ") + Ref->Ptr->getName());
}

std::error_code SymbolTable::addMemberFile(CanBeDefined *Sym) {
  auto FileOrErr = Sym->getMember();
  if (auto EC = FileOrErr.getError())
    return EC;
  std::unique_ptr<InputFile> File = std::move(FileOrErr.get());

  // getMember returns an empty buffer if the member was already
  // read from the library.
  if (!File)
    return std::error_code();
  return addFile(std::move(File));
}

uint64_t SymbolTable::getRVA(StringRef Symbol) {
  auto It = Symtab.find(Symbol);
  if (It == Symtab.end())
    return 0;
  SymbolRef *Ref = It->second;
  return cast<Defined>(Ref->Ptr)->getRVA();
}

void SymbolTable::dump() {
  for (auto &P : Symtab) {
    StringRef Name = P.first;
    SymbolRef *Ref = P.second;
    if (auto *Sym = dyn_cast<Defined>(Ref->Ptr))
      llvm::dbgs() << "0x" << Twine::utohexstr(ImageBase + Sym->getRVA()) << " " << Sym->getName() << "\n";
  }
}

} // namespace coff
} // namespace lld
