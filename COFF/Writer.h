//===- Writer.h -----------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_WRITER_H
#define LLD_COFF_WRITER_H

#include "InputFiles.h"
#include "SymbolTable.h"
#include "llvm/Support/FileOutputBuffer.h"
#include <memory>
#include <vector>

namespace lld {
namespace coff {

const uint32_t PermMask = 0xF00000F0;

// OutputSection represents a section in an output file. It's a
// container of chunks. OutputSection and Chunk are 1:N relationship.
// Chunks cannot belong to more than one OutputSections. The writer
// creates multiple OutputSections and assign them unique,
// non-overlapping file offsets and RVAs.
class OutputSection {
public:
  OutputSection(StringRef Nam, uint32_t SectionIndex);
  void setRVA(uint64_t);
  void setFileOffset(uint64_t);
  void addChunk(Chunk *C);
  StringRef getName() { return Name; }
  uint64_t getSectionIndex() { return SectionIndex; }
  std::vector<Chunk *> &getChunks() { return Chunks; }

  const llvm::object::coff_section *getHeader() { return &Header; }
  void addPermissions(uint32_t C);
  uint32_t getPermissions() { return Header.Characteristics & PermMask; }
  uint32_t getCharacteristics() { return Header.Characteristics; }
  uint64_t getRVA() { return Header.VirtualAddress; }
  uint64_t getFileOff() { return Header.PointerToRawData; }
  uint64_t getVirtualSize() { return Header.VirtualSize; }
  uint64_t getRawSize() { return Header.SizeOfRawData; }

private:
  llvm::object::coff_section Header;
  StringRef Name;
  uint32_t SectionIndex;
  std::vector<Chunk *> Chunks;
};

// The writer writes a SymbolTable result to a file.
class Writer {
public:
  explicit Writer(SymbolTable *T) : Symtab(T) {}
  std::error_code write(StringRef Path);

private:
  void markLive();
  void createSections();
  void createImportTables();
  void assignAddresses();
  void removeEmptySections();
  std::error_code openFile(StringRef OutputPath);
  void writeHeader();
  void writeSections();
  void applyRelocations();

  OutputSection *findSection(StringRef Name);
  OutputSection *createSection(StringRef Name);

  uint32_t getSizeOfInitializedData();
  std::map<StringRef, std::vector<DefinedImportData *>> binImports();

  SymbolTable *Symtab;
  std::unique_ptr<llvm::FileOutputBuffer> Buffer;
  llvm::object::coff_file_header *COFF;
  llvm::object::pe32plus_header *PE;
  llvm::object::data_directory *DataDirectory;
  llvm::object::coff_section *SectionTable;
  std::vector<std::unique_ptr<OutputSection>> OutputSections;
  Chunk *ImportAddressTable = nullptr;
  uint32_t ImportAddressTableSize = 0;

  Defined *Entry;
  uint64_t FileSize;
  uint64_t SizeOfImage;

  std::vector<std::unique_ptr<Chunk>> Chunks;
};

} // namespace pecoff
} // namespace lld

#endif
