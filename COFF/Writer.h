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

#include "llvm/ADT/StringRef.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/FileOutputBuffer.h"
#include <memory>
#include <vector>

namespace llvm {
namespace object {
class COFFObjectFile;
struct coff_section;
}
}

namespace lld {
namespace coff {

class Section {
public:
  Section(llvm::object::COFFObjectFile *F,
	  const llvm::object::coff_section *S, llvm::StringRef N)
    : File(F), Header(S), Name(N) {}

  uint64_t getSectionSize() const { return File->getSectionSize(Header); }

  llvm::object::COFFObjectFile *File;
  const llvm::object::coff_section *Header;
  llvm::StringRef Name;
  uint64_t FileOffset;
  uint64_t RVA;
};

class OutputSection {
public:
  uint64_t FileOffset;
  uint64_t RVA;
  uint64_t Size;
  llvm::StringRef Name;
  std::vector<Section *> Sections;
};

typedef std::vector<std::unique_ptr<Section>> SectionList;

class Writer {
public:
  explicit Writer(llvm::StringRef P) : Path(P) {}
  void addSections(SectionList &&S) { Sections = std::move(S); }
  void write();

private:
  void open();
  void writeHeader();
  void groupSections();
  void assignAddresses();
  void backfillHeaders();
  void writeSections();

  llvm::StringRef Path;
  std::unique_ptr<llvm::FileOutputBuffer> Buffer;
  SectionList Sections;
  llvm::object::coff_file_header *COFF;
  llvm::object::pe32plus_header *PE;
  llvm::object::data_directory *DataDirectory;
  llvm::object::coff_section *SectionTable;
  std::vector<OutputSection> OutputSections;
  uint64_t SectionTotalSize;

  const int DOSStubSize = 64;
  const int NumberfOfDataDirectory = 16;
  const int PageSize = 4096;
  const int FileAlignment = 512;
  const int HeaderSize = DOSStubSize + sizeof(llvm::COFF::PEMagic)
    + sizeof(llvm::object::coff_file_header)
    + sizeof(llvm::object::pe32plus_header)
    + sizeof(llvm::object::data_directory) * NumberfOfDataDirectory;
  const int SectionAlignment = 4096;
};

} // namespace pecoff
} // namespace lld

#endif
