//===- InputFiles.h -------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_INPUT_FILES_H
#define LLD_COFF_INPUT_FILES_H

#include "Chunks.h"
#include "Memory.h"
#include "Symbols.h"
#include "lld/Core/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/COFF.h"
#include <memory>
#include <set>
#include <vector>

using llvm::object::Archive;
using llvm::object::COFFObjectFile;

namespace lld {
namespace coff {

// The root class of input files.
class InputFile {
public:
  enum Kind { ArchiveKind, ObjectKind, ImportKind };
  Kind kind() const { return FileKind; }
  virtual ~InputFile() {}

  // Returns the filename.
  virtual StringRef getName() = 0;

  // Returns symbols defined by this file.
  virtual std::vector<std::unique_ptr<SymbolBody>> &getSymbols() = 0;

  // Reads a file (constructors don't do that). Returns an error if a
  // file is broken.
  virtual std::error_code parse() { return std::error_code(); }

  // Returns a short, human-friendly filename. If this is a member of
  // an archive file, a returned value includes parent's filename.
  // Used for logging or debugging.
  std::string getShortName();

  // Sets a parent filename if this file is created from an archive.
  void setParentName(StringRef N) { ParentName = N; }

protected:
  InputFile(Kind K) : FileKind(K) {}

private:
  const Kind FileKind;
  StringRef ParentName;
};

// .lib or .a file.
class ArchiveFile : public InputFile {
public:
  static bool classof(const InputFile *F) { return F->kind() == ArchiveKind; }
  static ErrorOr<std::unique_ptr<ArchiveFile>> create(StringRef Path);

  StringRef getName() override { return Name; }

  // Returns a memory buffer for a given symbol. An empty memory
  // buffer is returned if we have already returned the same memory
  // buffer. (So that we don't instantiate same members more than
  // once.)
  ErrorOr<MemoryBufferRef> getMember(const Archive::Symbol *Sym);

  // NB: All symbols returned by ArchiveFiles are of CanBeDefined type.
  std::vector<std::unique_ptr<SymbolBody>> &getSymbols() override {
    return SymbolBodies;
  }

private:
  ArchiveFile(StringRef Name, std::unique_ptr<Archive> File,
              std::unique_ptr<MemoryBuffer> Mem);

  std::unique_ptr<Archive> File;
  std::string Name;
  std::unique_ptr<MemoryBuffer> MB;
  std::vector<std::unique_ptr<SymbolBody>> SymbolBodies;
  std::set<const char *> Seen;
};

// .obj or .o file. This may be a member of an archive file.
class ObjectFile : public InputFile {
public:
  static bool classof(const InputFile *F) { return F->kind() == ObjectKind; }

  static ErrorOr<std::unique_ptr<ObjectFile>> create(StringRef Path);
  static ErrorOr<std::unique_ptr<ObjectFile>> create(StringRef Path,
                                                     MemoryBufferRef MB);

  StringRef getName() override { return Name; }

  // Returns a Symbol object for the SymbolIndex'th symbol in the
  // underlying object file.
  Symbol *getSymbol(uint32_t SymbolIndex);

  std::vector<std::unique_ptr<Chunk>> &getChunks() { return Chunks; }

  // Returns .drectve section contents if exist.
  StringRef getDirectives() { return Directives; }

  // Returns the underying COFF file.
  COFFObjectFile *getCOFFObj() { return COFFObj.get(); }

  std::vector<std::unique_ptr<SymbolBody>> &getSymbols() override {
    return SymbolBodies;
  }

private:
  ObjectFile(StringRef Name, std::unique_ptr<COFFObjectFile> File);
  void initializeChunks();
  void initializeSymbols();

  SymbolBody *createSymbolBody(StringRef Name, COFFSymbolRef Sym,
                               const void *Aux, bool IsFirst);

  std::string Name;
  std::unique_ptr<COFFObjectFile> COFFObj;
  std::unique_ptr<MemoryBuffer> MB;
  StringRef Directives;

  // List of all chunks defined by this file. The first chunks
  // represents sections which may be followed by other non-section
  // chunks such as common symbols.
  std::vector<std::unique_ptr<Chunk>> Chunks;

  // This vector contains the same chunks as Chunks, but they are
  // indexed such that you can get a SectionChunk by section
  // index. Nonexistent section indices are filled with null pointers.
  // (Because section number is 1-based, the first slot is always a
  // null pointer.)
  std::vector<Chunk *> SparseChunks;

  // List of all symbols referenced or defined by this file.
  std::vector<std::unique_ptr<SymbolBody>> SymbolBodies;

  // This vector contains the same symbols as SymbolBodies, but they
  // are indexed such that you can get a SymbolBody by symbol
  // index. Nonexistent indices (which are occupied by auxiliary
  // symbols in the real symbol table) are filled by null pointers.
  std::vector<SymbolBody *> SparseSymbolBodies;
};

// This type represents import library members that contain DLL names
// and symbols exported from the DLLs. See Microsoft PE/COFF spec. 7
// for details about the format.
class ImportFile : public InputFile {
public:
  explicit ImportFile(MemoryBufferRef M) : InputFile(ImportKind), MBRef(M) {
    readImports();
  }

  static bool classof(const InputFile *F) { return F->kind() == ImportKind; }
  StringRef getName() override { return MBRef.getBufferIdentifier(); }

  std::vector<std::unique_ptr<SymbolBody>> &getSymbols() override {
    return SymbolBodies;
  }

private:
  void readImports();

  MemoryBufferRef MBRef;
  std::vector<std::unique_ptr<SymbolBody>> SymbolBodies;
  StringAllocator Alloc;
};

} // namespace coff
} // namespace lld

#endif
