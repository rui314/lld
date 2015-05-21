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
#include "Resolver.h"
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

class Writer {
public:
  explicit Writer(Resolver *R) : Res(R) {}
  void write(StringRef Path);

private:
  void groupSections();
  void createImportTables();
  void assignAddresses();
  void removeEmptySections();
  void openFile(StringRef OutputPath);
  void writeHeader();
  void writeSections();
  void applyRelocations();
  void backfillHeaders();
  OutputSection *findSection(StringRef name);
  OutputSection *createSection(StringRef name);
  std::map<StringRef, std::vector<DefinedImportData *>> groupImports();

  Resolver *Res;
  std::unique_ptr<llvm::FileOutputBuffer> Buffer;
  llvm::object::coff_file_header *COFF;
  llvm::object::pe32plus_header *PE;
  llvm::object::data_directory *DataDirectory;
  llvm::object::coff_section *SectionTable;
  std::vector<std::unique_ptr<OutputSection>> OutputSections;
  Chunk *IAT = nullptr;

  uint64_t EndOfSectionTable;
  uint64_t SectionTotalSizeDisk;
  uint64_t SectionTotalSizeMemory;
};

} // namespace pecoff
} // namespace lld

#endif
