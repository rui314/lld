//===- Reader.h -----------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_SYMBOL_H
#define LLD_COFF_SYMBOL_H

#include "lld/Core/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/COFF.h"
#include <map>
#include <memory>
#include <set>
#include <vector>

using llvm::object::Archive;
using llvm::object::COFFObjectFile;
using llvm::object::COFFSymbolRef;
using llvm::object::coff_relocation;
using llvm::object::coff_section;

namespace lld {
namespace coff {

class ArchiveFile;
class Chunk;
class InputSection;
class ObjectFile;
class OutputSection;

class Symbol {
public:
  enum Kind {
    DefinedRegularKind,
    DefinedImplibKind,
    UndefinedKind,
    CanBeDefinedKind,
  };
  Kind kind() const { return SymbolKind; }
  virtual ~Symbol() {}

protected:
  Symbol(Kind K) : SymbolKind(K) {}

private:
  const Kind SymbolKind;
};

class Defined : public Symbol {
public:
  Defined(Kind K) : Symbol(K) {}
  static bool classof(const Symbol *S) {
    Kind K = S->kind();
    return K == DefinedRegularKind || K == DefinedImplibKind;
  }
  virtual uint64_t getRVA() = 0;
  virtual uint64_t getFileOff() = 0;
  virtual bool isCOMDAT() const { return false; }
};

class DefinedRegular : public Defined {
public:
  DefinedRegular(ObjectFile *F, COFFSymbolRef SymRef);
  static bool classof(const Symbol *S) {
    return S->kind() == DefinedRegularKind;
  }

  uint64_t getRVA() override;
  uint64_t getFileOff() override;
  bool isCOMDAT() const override;

  ObjectFile *File;
  COFFSymbolRef Sym;
  InputSection *Section;
};

class DefinedImplib : public Defined {
public:
  DefinedImplib(StringRef D, StringRef N)
    : Defined(DefinedImplibKind), DLLName(D), Name((Twine("__imp_") + N).str()),
      ExpName(N) {}
  static bool classof(const Symbol *S) {
    return S->kind() == DefinedImplibKind;
  }

  uint64_t getRVA() override;
  uint64_t getFileOff() override;

  StringRef DLLName;
  std::string Name;
  std::string ExpName;
  Chunk *AddressTable = nullptr;
};

class CanBeDefined : public Symbol {
public:
  CanBeDefined(ArchiveFile *F, const Archive::Symbol *S)
    : Symbol(CanBeDefinedKind), File(F), Sym(S) {}
  static bool classof(const Symbol *S) { return S->kind() == CanBeDefinedKind; }
  ErrorOr<MemoryBufferRef> getMember();

  ArchiveFile *File;
  const Archive::Symbol *Sym;
};

class Undefined : public Symbol {
public:
  Undefined(ObjectFile *F) : Symbol(UndefinedKind), File(F) {}
  static bool classof(const Symbol *S) { return S->kind() == UndefinedKind; }

  ObjectFile *File;
};

struct SymbolRef {
  SymbolRef(StringRef N, Symbol *P) : Name(N), Ptr(P) {}
  SymbolRef() : Name(""), Ptr(nullptr) {}
  StringRef Name;
  Symbol *Ptr;
};

class ArchiveFile {
public:
  static ErrorOr<std::unique_ptr<ArchiveFile>> create(StringRef Path);

  std::string Name;
  std::unique_ptr<Archive> File;

  ErrorOr<MemoryBufferRef> getMember(const Archive::Symbol *Sym);

private:
  ArchiveFile(StringRef N, std::unique_ptr<Archive> F,
	      std::unique_ptr<MemoryBuffer> M)
    : Name(N), File(std::move(F)), MB(std::move(M)) {}

  std::unique_ptr<MemoryBuffer> MB;
  std::set<const char *> Seen;
};

class ObjectFile {
public:
  static ErrorOr<std::unique_ptr<ObjectFile>> create(StringRef Path);
  static ErrorOr<std::unique_ptr<ObjectFile>>
    create(StringRef Path, MemoryBufferRef MB);

  std::error_code initSections();

  std::string Name;
  std::vector<SymbolRef *> Symbols;
  std::vector<InputSection> Sections;
  std::unique_ptr<COFFObjectFile> COFFFile;

private:
  ObjectFile(StringRef N, std::unique_ptr<COFFObjectFile> F)
    : Name(N), COFFFile(std::move(F)) {}

  std::unique_ptr<MemoryBuffer> MB;
};

class Chunk {
public:
  virtual const uint8_t *getData() const { return Data.data(); }
  virtual size_t getSize() const { return Data.size(); }
  virtual bool isBSS() const { return false; }
  virtual void applyRelocations(uint8_t *Buffer) = 0;

  ArrayRef<uint8_t> Data;
  uint64_t RVA = 0;
  uint64_t FileOff = 0;
  uint64_t Align = 1;
};

class SectionChunk : public Chunk {
public:
  SectionChunk(InputSection *S) : Section(S) {}
  void applyRelocations(uint8_t *Buffer) override;
  size_t getSize() const override;
  bool isBSS() const override;

private:
  InputSection *Section;
};

class StringChunk : public Chunk {
public:
  StringChunk(StringRef S) : Array(S.size() + 1) {
    memcpy(Array.data(), S.data(), S.size());
    Array[S.size()] = 0;
    Data = Array;
  }

  void applyRelocations(uint8_t *Buffer) override {}

private:
  std::vector<uint8_t> Array;
};

class InputSection {
public:
  InputSection(ObjectFile *F, const coff_section *H)
    : File(F), Header(H), Chunk(this) {
    F->COFFFile->getSectionName(H, Name);
    if (!Chunk.isBSS())
      F->COFFFile->getSectionContents(H, Chunk.Data);
    Chunk.Align = getAlign();
  }

  bool isCOMDAT() const;
  void applyRelocations(uint8_t *Buffer);

  ObjectFile *File;
  const coff_section *Header;
  SectionChunk Chunk;
  StringRef Name;
  OutputSection *Out = nullptr;

private:
  void applyRelocation(uint8_t *Buffer, const coff_relocation *Rel);
  uint64_t getAlign() const;
};

class OutputSection {
public:
  OutputSection(StringRef N, uint32_t SI);
  void setRVA(uint64_t);
  void setFileOffset(uint64_t);
  void addChunk(Chunk *C);

  StringRef Name;
  uint32_t SectionIndex;
  llvm::object::coff_section Header;
  std::vector<Chunk *> Chunks;
};

typedef std::map<llvm::StringRef, SymbolRef> SymbolTable;

ErrorOr<DefinedImplib *>
readImplib(MemoryBufferRef MBRef, llvm::BumpPtrAllocator *Alloc);

} // namespace coff
} // namespace lld

#endif
