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
  addInitialSymbol(new DefinedAbsolute("__ImageBase", ImageBase));
  addInitialSymbol(new Undefined("mainCRTStartup"));
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
  for (std::unique_ptr<Symbol> &Sym : File->getSymbols()) {
    if (Sym->isExternal()) {
      // Only externally-visible symbols are subjects of symbol
      // resolution.
      SymbolRef *Ref;
      if (auto EC = resolve(Sym.get(), &Ref))
        return EC;
      Sym->setSymbolRef(Ref);
    } else {
      Sym->setSymbolRef(new (Alloc) SymbolRef(Sym.get()));
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
  for (std::unique_ptr<Symbol> &Sym : File->getSymbols())
    if (auto EC = resolve(Sym.get(), nullptr))
      return EC;
  return std::error_code();
}

std::error_code SymbolTable::addFile(ImplibFile *File) {
  ImplibFiles.emplace_back(File);
  for (std::unique_ptr<Symbol> &Sym : File->getSymbols())
    if (auto EC = resolve(Sym.get(), nullptr))
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

// This function resolves conflicts if a given symbol has the same
// name as an existing symbol. Decisions are made based on symbol
// types.
std::error_code SymbolTable::resolve(Symbol *Sym, SymbolRef **RefP) {
  StringRef Name = Sym->getName();
  auto It = Symtab.find(Name);
  if (It == Symtab.end())
    It = Symtab.insert(It, std::make_pair(Name, new (Alloc) SymbolRef()));
  SymbolRef *Ref = It->second;

  // RefP is not significant in this function. It's here to reduce the
  // number of hash table lookup in the caller.
  if (RefP)
    *RefP = Ref;

  // If nothing exists yet, just add a new one.
  if (Ref->Ptr == nullptr) {
    Ref->Ptr = Sym;
    return std::error_code();
  }

  if (isa<Undefined>(Ref->Ptr)) {
    // Undefined and Undefined: There are two object files referencing
    // the same undefined symbol. Undefined symbols don't have much
    // identity, so a selection is arbitrary. We choose the existing
    // one.
    if (isa<Undefined>(Sym))
      return std::error_code();

    // CanBeDefined and Undefined: We read an archive member file
    // pointed by the CanBeDefined symbol to resolve the Undefined
    // symbol.
    if (auto *New = dyn_cast<CanBeDefined>(Sym))
      return addMemberFile(New);

    // Undefined and Defined: An undefined symbol is now being
    // resolved. Select the Defined symbol.
    assert(isa<Defined>(Sym));
    Ref->Ptr = Sym;
    return std::error_code();
  }

  if (auto *Existing = dyn_cast<CanBeDefined>(Ref->Ptr)) {
    if (isa<Defined>(Sym)) {
      Ref->Ptr = Sym;
      return std::error_code();
    }

    // CanBeDefined and CanBeDefined: We have two libraries having the
    // same symbol. We probably should print a warning message.
    if (isa<CanBeDefined>(Sym))
      return std::error_code();

    assert(isa<Undefined>(Sym));
    return addMemberFile(Existing);
  }

  // Both symbols are defined symbols. Select one of them if they are
  // Common or COMDAT symbols.
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

void SymbolTable::addInitialSymbol(Symbol *Sym) {
  Symbols.push_back(std::unique_ptr<Symbol>(Sym));
  Symtab[Sym->getName()] = new (Alloc) SymbolRef(Sym);
}

} // namespace coff
} // namespace lld
