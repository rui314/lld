//===- Symbol.h -----------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_SYMBOL_H
#define LLD_COFF_SYMBOL_H

#include "Section.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <map>

namespace lld {
namespace coff {

class Symbol {
public:
  enum Definition { Defined, Undefined, Library };
  Definition getDefinition() const { return Def; }

protected:
  Symbol(Definition D) : Def(D) {}

private:
  Definition Def;
};

class DefinedSymbol : public Symbol {
public:
  DefinedSymbol() : Symbol(Defined) {}
  static bool classof(const Symbol *S) { return S->getDefinition() == Defined; }

  Section *Sec;
};

class UndefinedSymbol : public Symbol {
public:
  static bool classof(const Symbol *S) { return S->getDefinition() == Undefined; }
};

class LibrarySymbol : public Symbol {
public:
  static bool classof(const Symbol *S) { return S->getDefinition() == Library; }
};

struct SymbolRef {
  llvm::StringRef Name;
  Symbol *Sym;
};

class SymbolTable {
public:
  SymbolRef *intern(llvm::StringRef S) { return &Table[S]; }

  bool checkUndefined() {
    bool ok = true;
    for (auto &V : Table) {
      llvm::StringRef Name = V.first;
      SymbolRef &Ref = V.second;
      if (!llvm::isa<UndefinedSymbol>(Ref.Sym))
	continue;
      llvm::errs() << "Undefined symbol " << Name << "\n";
      ok = false;
    }
    return ok;
  }

private:
  std::map<llvm::StringRef, SymbolRef> Table;
};

} // namespace pecoff
} // namespace lld

#endif
