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
  addInitialSymbol(new DefinedAbsolute("__ImageBase", Config->ImageBase));
  addInitialSymbol(new Undefined(Config->EntryName));
}

void SymbolTable::addInitialSymbol(SymbolBody *Body) {
  OwnedSymbols.push_back(std::unique_ptr<SymbolBody>(Body));
  Symtab[Body->getName()] = new (Alloc) Symbol(Body);
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

  // If an object file contains .drectve section, read it and add
  // files listed in the section.
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

// This function resolves conflicts if there's an existing symbol with
// the same name. Decisions are made based on symbol types.
// (This function is designed to be easily parallelizable using
// pointer compare-and-swap.)
std::error_code SymbolTable::resolve(SymbolBody *New, Symbol **SymP) {
  StringRef Name = New->getName();
  auto *NewSym = new (Alloc) Symbol(nullptr);
  auto Res = Symtab.insert(std::make_pair(Name, NewSym));
  Symbol *Sym = Res.second ? NewSym : Res.first->second;

  // SymP is not significant in this function. It's here to reduce the
  // number of hash table lookups in the caller.
  if (SymP)
    *SymP = Sym;

  // If nothing exists yet, just add a new one.
  if (Sym->Body == nullptr) {
    Sym->Body = New;
    return std::error_code();
  }

  SymbolBody *Existing = Sym->Body;
  int comp = Existing->compare(New);
  if (comp < 0)
    Sym->Body = New;
  if (comp == 0)
    return make_dynamic_error_code(Twine("duplicate symbol: ") + Name);

  if (isa<Undefined>(Existing) || isa<Undefined>(New))
    if (auto *B = dyn_cast<CanBeDefined>(Sym->Body))
      return addMemberFile(B);
  return std::error_code();
}

// Reads an archive member file pointed by a given symbol.
std::error_code SymbolTable::addMemberFile(CanBeDefined *Body) {
  auto FileOrErr = Body->getMember();
  if (auto EC = FileOrErr.getError())
    return EC;
  std::unique_ptr<InputFile> File = std::move(FileOrErr.get());

  // getMember returns an empty buffer if the member was already
  // read from the library.
  if (!File)
    return std::error_code();
  if (Config->Verbose)
    llvm::dbgs() << "Loaded " << File->getShortName() << " for "
                 << Body->getName() << "\n";
  return addFile(std::move(File));
}

std::vector<Chunk *> SymbolTable::getChunks() {
  std::vector<Chunk *> Res;
  for (std::unique_ptr<ObjectFile> &File : ObjectFiles)
    for (std::unique_ptr<Chunk> &C : File->getChunks())
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
      llvm::dbgs() << Twine::utohexstr(Config->ImageBase + Body->getRVA())
                   << " " << Body->getName() << "\n";
  }
}

} // namespace coff
} // namespace lld
