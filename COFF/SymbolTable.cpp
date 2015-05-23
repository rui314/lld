//===- SymbolTable.cpp ----------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Config.h"
#include "Driver.h"
#include "SymbolTable.h"
#include "lld/Core/Error.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

namespace lld {
namespace coff {

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
  if (auto *F = dyn_cast<ImportFile>(P))
    return addFile(F);
  llvm_unreachable("unknown file type");
}

std::error_code SymbolTable::addFile(ObjectFile *File) {
  ObjectFiles.emplace_back(File);
  for (std::unique_ptr<SymbolBody> &Body : File->getSymbols()) {
    if (Body->isExternal()) {
      // Only externally-visible symbols are subjects of symbol
      // resolution.
      Symbol *Sym;
      if (auto EC = resolve(Body.get(), &Sym))
        return EC;
      Body->setBackref(Sym);
    } else {
      Body->setBackref(new (Alloc) Symbol(Body.get()));
    }
  }

  StringRef Dir = File->getDirectives();
  if (!Dir.empty()) {
    std::vector<std::unique_ptr<InputFile>> Libs;
    if (auto EC = parseDirectives(Dir, &Libs, &StringAlloc))
      return EC;
    for (std::unique_ptr<InputFile> &L : Libs)
      addFile(std::move(L));
  }
  return std::error_code();
}

std::error_code SymbolTable::addFile(ArchiveFile *File) {
  ArchiveFiles.emplace_back(File);
  for (std::unique_ptr<SymbolBody> &Body : File->getSymbols())
    if (auto EC = resolve(Body.get(), nullptr))
      return EC;
  return std::error_code();
}

std::error_code SymbolTable::addFile(ImportFile *File) {
  ImportFiles.emplace_back(File);
  for (std::unique_ptr<SymbolBody> &Body : File->getSymbols())
    if (auto EC = resolve(Body.get(), nullptr))
      return EC;
  return std::error_code();
}

bool SymbolTable::reportRemainingUndefines() {
  for (auto &I : Symtab) {
    Symbol *Sym = I.second;
    if (auto *Undef = dyn_cast<Undefined>(Sym->Body)) {
      if (SymbolBody *Alias = Undef->getWeakAlias()) {
        Sym->Body = Alias->getSymbol()->Body;
        continue;
      }
      llvm::errs() << "undefined symbol: " << Sym->Body->getName() << "\n";
      return true;
    }
  }
  return false;
}

// This function resolves conflicts if a given symbol has the same
// name as an existing symbol. Decisions are made based on symbol
// types.
std::error_code SymbolTable::resolve(SymbolBody *Body, Symbol **RefP) {
  StringRef Name = Body->getName();
  auto *NewVal = new (Alloc) Symbol();
  auto Res = Symtab.insert(std::make_pair(Name, NewVal));
  Symbol *Ref = Res.second ? NewVal : Res.first->second;

  // RefP is not significant in this function. It's here to reduce the
  // number of hash table lookups in the caller.
  if (RefP)
    *RefP = Ref;

  // If nothing exists yet, just add a new one.
  if (Ref->Body == nullptr) {
    Ref->Body = Body;
    return std::error_code();
  }

  if (isa<Undefined>(Ref->Body)) {
    // Undefined and Undefined: There are two object files referencing
    // the same undefined symbol. Undefined symbols don't have much
    // identity, so a selection is arbitrary. We choose the existing
    // one.
    if (auto *New = dyn_cast<Undefined>(Body)) {
      if (New->getWeakAlias())
        Ref->Body = New;
      return std::error_code();
    }

    // CanBeDefined and Undefined: We read an archive member file
    // pointed by the CanBeDefined symbol to resolve the Undefined
    // symbol.
    if (auto *New = dyn_cast<CanBeDefined>(Body))
      return addMemberFile(New);

    // Undefined and Defined: An undefined symbol is now being
    // resolved. Select the Defined symbol.
    assert(isa<Defined>(Body));
    Ref->Body = Body;
    return std::error_code();
  }

  if (auto *Existing = dyn_cast<CanBeDefined>(Ref->Body)) {
    if (isa<Defined>(Body)) {
      Ref->Body = Body;
      return std::error_code();
    }

    // CanBeDefined and CanBeDefined: We have two libraries having the
    // same symbol. We probably should print a warning message.
    if (isa<CanBeDefined>(Body))
      return std::error_code();

    auto *New = cast<Undefined>(Body);
    if (New->getWeakAlias()) {
      Ref->Body = Body;
      return std::error_code();
    }
    return addMemberFile(Existing);
  }

  // Both symbols are defined symbols. Select one of them if they are
  // Common or COMDAT symbols.
  Defined *Existing = cast<Defined>(Ref->Body);
  if (isa<Undefined>(Body) || isa<CanBeDefined>(Body))
    return std::error_code();
  Defined *New = cast<Defined>(Body);

  // Common symbols
  if (Existing->isCommon()) {
    if (New->isCommon()) {
      if (Existing->getCommonSize() < New->getCommonSize())
        Ref->Body = New;
      return std::error_code();
    }
    Ref->Body = New;
    return std::error_code();
  }
  if (New->isCommon())
    return std::error_code();

  // COMDAT symbols
  if (Existing->isCOMDAT() && New->isCOMDAT())
      return std::error_code();
  return make_dynamic_error_code(Twine("duplicate symbol: ") + Ref->Body->getName());
}

std::error_code SymbolTable::addMemberFile(CanBeDefined *Body) {
  auto FileOrErr = Body->getMember();
  if (auto EC = FileOrErr.getError())
    return EC;
  std::unique_ptr<InputFile> File = std::move(FileOrErr.get());

  // getMember returns an empty buffer if the member was already
  // read from the library.
  if (!File)
    return std::error_code();
  File->setParentName(Body->File->Name);
  if (Config->Verbose)
    llvm::dbgs() << "Loaded " << File->getShortName() << " for " << Body->getName() << "\n";
  return addFile(std::move(File));
}

std::vector<Chunk *> SymbolTable::getChunks() {
  std::vector<Chunk *> Res;
  for (std::unique_ptr<ObjectFile> &File : ObjectFiles)
    for (std::unique_ptr<Chunk> &C : File->Chunks)
      if (C)
        Res.push_back(C.get());
  return Res;
}

SymbolBody *SymbolTable::find(StringRef Name) {
  auto It = Symtab.find(Name);
  if (It == Symtab.end())
    return nullptr;
  return It->second->Body;
}

void SymbolTable::dump() {
  for (auto &P : Symtab) {
    StringRef Name = P.first;
    Symbol *Ref = P.second;
    if (auto *Body = dyn_cast<Defined>(Ref->Body))
      llvm::dbgs() << "0x" << Twine::utohexstr(ImageBase + Body->getRVA())
                   << " " << Body->getName() << "\n";
  }
}

void SymbolTable::addInitialSymbol(SymbolBody *Body) {
  OwnedSymbols.push_back(std::unique_ptr<SymbolBody>(Body));
  Symtab[Body->getName()] = new (Alloc) Symbol(Body);
}

} // namespace coff
} // namespace lld
