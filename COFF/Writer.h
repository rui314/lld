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
    : File(F), Sec(S), Name(N) {}

  uint64_t getSectionSize() const { return File->getSectionSize(Sec); }

  llvm::object::COFFObjectFile *File;
  const llvm::object::coff_section *Sec;
  llvm::StringRef Name;
  uint64_t FileOffset;
  uint64_t RVA;
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
  void assignAddress();
  void writeSections();

  llvm::StringRef Path;
  std::unique_ptr<llvm::FileOutputBuffer> Buffer;
  SectionList Sections;
  llvm::object::coff_file_header *COFF;
  llvm::object::pe32plus_header *PE;
  llvm::object::data_directory *DataDirectory;
  llvm::object::coff_section *SectionTable;
  int NumSections = 0;
  uint64_t SectionTotalSize = 0;

  const int DOSStubSize = 64;
  const int NumberfOfDataDirectory = 16;
  const int HeaderSize = DOSStubSize + sizeof(llvm::COFF::PEMagic)
    + sizeof(llvm::object::coff_file_header)
    + sizeof(llvm::object::pe32plus_header)
    + sizeof(llvm::object::data_directory) * NumberfOfDataDirectory;
  const int SectionAlignment = 4096;
};

} // namespace pecoff
} // namespace lld

#endif
