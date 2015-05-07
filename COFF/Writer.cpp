//===- Writer.cpp ---------------------------------------------------------===// 
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Writer.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::object;

void writeFile(StringRef Path) {
  coff_file_header COFF;
  memset(&COFF, 0, sizeof(COFF));
  COFF.Machine = llvm::COFF::IMAGE_FILE_MACHINE_AMD64;
  pe32plus_header PE;
  memset(&PE, 0, sizeof(PE));
  PE.Magic = llvm::COFF::PE32Header::PE32_PLUS;

  size_t Size = sizeof(llvm::COFF::PEMagic) + sizeof(COFF) + sizeof(PE);

  std::unique_ptr<llvm::FileOutputBuffer> Buffer;
  if (auto EC = FileOutputBuffer::create(
	Path, Size, Buffer, FileOutputBuffer::F_executable)) {
    llvm::errs() << "Failed to open " << Path << ": " << EC.message() << "\n";
    return;
  }

  uint8_t *P = Buffer->getBufferStart();
  memcpy(P, llvm::COFF::PEMagic, sizeof(llvm::COFF::PEMagic));
  P += sizeof(llvm::COFF::PEMagic);
  memcpy(P, &COFF, sizeof(COFF));
  P += sizeof(COFF);
  memcpy(P, &PE, sizeof(PE));
  Buffer->commit();
}

namespace lld {
namespace coff {

void write(StringRef OutputPath,
	   std::vector<std::unique_ptr<COFFObjectFile>> &Files) {
  if (Files.empty())
    return;
  COFFObjectFile *File = Files[0].get();
  (void)File->getMachine();
  for (const auto &SectionRef : File->sections()) {
    const coff_section *Sec = File->getCOFFSection(SectionRef);
    StringRef Name;
    if (auto EC = File->getSectionName(Sec, Name)) {
      llvm::errs() << "Failed to get a section name: " << EC.message() << "\n";
      return;
    }
    llvm::dbgs() << Name << "\n";
  }
  writeFile(OutputPath);
}

} // namespace coff
} // namespace lld
