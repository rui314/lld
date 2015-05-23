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

#include "Reader.h"
#include "SymbolTable.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/FileOutputBuffer.h"
#include <memory>
#include <vector>

namespace lld {
namespace coff {

const int DOSStubSize = 64;
const int NumberfOfDataDirectory = 16;
const int HeaderSize = DOSStubSize + sizeof(llvm::COFF::PEMagic)
  + sizeof(llvm::object::coff_file_header)
  + sizeof(llvm::object::pe32plus_header)
  + sizeof(llvm::object::data_directory) * NumberfOfDataDirectory;

class OutputSection {
public:
  OutputSection(StringRef Nam, uint32_t SectionIndex);
  void setRVA(uint64_t);
  void setFileOffset(uint64_t);
  void addChunk(Chunk *C);
  StringRef getName() { return Name; }
  uint64_t getSectionIndex() { return SectionIndex; }
  std::vector<Chunk *> &getChunks() { return Chunks; }

  const llvm::object::coff_section *getHeader() {
    if (Header.SizeOfRawData == 0)
      Header.PointerToRawData = 0;
    return &Header;
  }
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
  uint32_t getTotalSectionSize(uint32_t Perm);
  OutputSection *createSection(StringRef Name);
  std::map<StringRef, std::vector<DefinedImportData *>> groupImports();

  SymbolTable *Symtab;
  std::unique_ptr<llvm::FileOutputBuffer> Buffer;
  llvm::object::coff_file_header *COFF;
  llvm::object::pe32plus_header *PE;
  llvm::object::data_directory *DataDirectory;
  llvm::object::coff_section *SectionTable;
  std::vector<std::unique_ptr<OutputSection>> OutputSections;
  Chunk *ImportAddressTable = nullptr;
  uint32_t ImportAddressTableSize = 0;

  uint64_t FileSize;
  uint64_t SizeOfImage;

  std::vector<std::unique_ptr<Chunk>> Chunks;
};

} // namespace pecoff
} // namespace lld

#endif
