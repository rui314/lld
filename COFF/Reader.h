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
#include "llvm/Support/FileUtilities.h"
#include <map>
#include <memory>
#include <set>
#include <vector>

using llvm::object::Archive;
using llvm::object::COFFObjectFile;
using llvm::object::COFFSymbolRef;
using llvm::object::coff_relocation;
using llvm::object::coff_section;
using llvm::sys::fs::file_magic;
using llvm::sys::fs::identify_magic;

namespace lld {
namespace coff {

class ArchiveFile;
class Chunk;
class InputFile;
class InputSection;
class ObjectFile;
class OutputSection;
struct SymbolRef;

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

  virtual bool isExternal() { return true; }
  virtual StringRef getName() {
    llvm::report_fatal_error("not implemented");
  }

  SymbolRef **SymbolRefPP = nullptr;

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
  DefinedRegular(ObjectFile *F, StringRef N, COFFSymbolRef SymRef);
  static bool classof(const Symbol *S) {
    return S->kind() == DefinedRegularKind;
  }

  StringRef getName() override { return Name; }
  uint64_t getRVA() override;
  uint64_t getFileOff() override;
  bool isCOMDAT() const override;
  bool isExternal() override { return Sym.isExternal(); }

  ObjectFile *File;
  StringRef Name;
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

  StringRef getName() override { return Name; }
  uint64_t getRVA() override;
  uint64_t getFileOff() override;

  StringRef DLLName;
  std::string Name;
  std::string ExpName;
  Chunk *AddressTable = nullptr;
};

class CanBeDefined : public Symbol {
public:
  CanBeDefined(ArchiveFile *F, const Archive::Symbol S)
    : Symbol(CanBeDefinedKind), Name(S.getName()), File(F), Sym(S) {}

  static bool classof(const Symbol *S) { return S->kind() == CanBeDefinedKind; }
  ErrorOr<std::unique_ptr<InputFile>> getMember();

  StringRef getName() override { return Name; }

  StringRef Name;
  ArchiveFile *File;
  const Archive::Symbol Sym;
};

class Undefined : public Symbol {
public:
 Undefined(ObjectFile *F, StringRef N)
   : Symbol(UndefinedKind), File(F), Name(N) {}
  static bool classof(const Symbol *S) { return S->kind() == UndefinedKind; }

  StringRef getName() override { return Name; }

  ObjectFile *File;
  StringRef Name;
};

struct SymbolRef {
  SymbolRef(Symbol *P) : Ptr(P) {}
  SymbolRef() : Ptr(nullptr) {}
  Symbol *Ptr;
};

class InputFile {
public:
  enum Kind { ArchiveKind, ObjectKind, ImplibKind };
  Kind kind() const { return FileKind; }
  virtual ~InputFile() {}

  virtual StringRef getName() = 0;
  virtual std::vector<Symbol *> getSymbols() = 0;

protected:
  InputFile(Kind K) : FileKind(K) {}

private:
  const Kind FileKind;
};

class ArchiveFile : public InputFile {
public:
  static bool classof(const InputFile *F) { return F->kind() == ArchiveKind; }

  static ErrorOr<std::unique_ptr<ArchiveFile>> create(StringRef Path);

  StringRef getName() override { return Name; }
  std::vector<Symbol *> getSymbols() override;

  std::string Name;
  std::unique_ptr<Archive> File;

  ErrorOr<MemoryBufferRef> getMember(const Archive::Symbol *Sym);

private:
  ArchiveFile(StringRef N, std::unique_ptr<Archive> F,
	      std::unique_ptr<MemoryBuffer> M)
    : InputFile(ArchiveKind), Name(N), File(std::move(F)), MB(std::move(M)) {}

  std::unique_ptr<MemoryBuffer> MB;
  std::set<const char *> Seen;
};

class ObjectFile : public InputFile {
public:
  static bool classof(const InputFile *F) { return F->kind() == ObjectKind; }

  static ErrorOr<std::unique_ptr<ObjectFile>> create(StringRef Path);
  static ErrorOr<std::unique_ptr<ObjectFile>>
    create(StringRef Path, MemoryBufferRef MB);

  StringRef getName() override { return Name; }
  std::vector<Symbol *> getSymbols() override;

  std::error_code initSections();

  std::string Name;
  std::vector<SymbolRef *> Symbols;
  std::vector<InputSection> Sections;
  std::unique_ptr<COFFObjectFile> COFFFile;

private:
  ObjectFile(StringRef N, std::unique_ptr<COFFObjectFile> F)
    : InputFile(ObjectKind), Name(N), COFFFile(std::move(F)) {}

  std::unique_ptr<MemoryBuffer> MB;
};

class ImplibFile : public InputFile {
public:
  ImplibFile(MemoryBufferRef M);

  static bool classof(const InputFile *F) { return F->kind() == ImplibKind; }

  StringRef getName() override;
  std::vector<Symbol *> getSymbols() override { return Symbols; }

private:
  MemoryBufferRef MBRef;
  std::vector<Symbol *> Symbols;
};

class Chunk {
public:
  virtual const uint8_t *getData() const = 0;
  virtual size_t getSize() const = 0;
  virtual void applyRelocations(uint8_t *Buffer) = 0;
  virtual bool isBSS() const { return false; }

  uint64_t RVA = 0;
  uint64_t FileOff = 0;
  uint64_t Align = 1;
};

class SectionChunk : public Chunk {
public:
  SectionChunk(InputSection *S);
  const uint8_t *getData() const override;
  size_t getSize() const override;
  void applyRelocations(uint8_t *Buffer) override;
  bool isBSS() const override;

private:
  ArrayRef<uint8_t> Data;
  InputSection *Section;
};

class StringChunk : public Chunk {
public:
  StringChunk(StringRef S) : Data(S.size() + 1) {
    memcpy(Data.data(), S.data(), S.size());
    Data[S.size()] = 0;
  }

  const uint8_t *getData() const override { return &Data[0]; }
  size_t getSize() const override { return Data.size(); }
  void applyRelocations(uint8_t *Buffer) override {}

private:
  std::vector<uint8_t> Data;
};

class InputSection {
public:
  InputSection(ObjectFile *F, const coff_section *H)
    : File(F), Header(H), Chunk(this) {
    F->COFFFile->getSectionName(H, Name);
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

typedef std::map<llvm::StringRef, SymbolRef *> SymbolTable;

ErrorOr<DefinedImplib *> readImplib(MemoryBufferRef MBRef);

} // namespace coff
} // namespace lld

#endif
