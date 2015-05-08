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
#include <map>
#include <utility>

using namespace llvm;
using namespace llvm::object;

typedef std::multimap<StringRef, lld::coff::Section *> SectionMap;

static const int DOSStubSize = 64;
static const int HeaderSize = DOSStubSize + sizeof(llvm::COFF::PEMagic)
  + sizeof(coff_file_header) + sizeof(pe32plus_header);

static void writeHeader(uint8_t *P) {
  dos_header DOS;
  memset(&DOS, 0, sizeof(DOS));
  DOS.Magic[0] = 'M';
  DOS.Magic[1] = 'Z';
  DOS.AddressOfRelocationTable = sizeof(dos_header);
  DOS.AddressOfNewExeHeader = DOSStubSize;
  memcpy(P, &DOS, sizeof(DOS));
  P += DOSStubSize;

  memcpy(P, llvm::COFF::PEMagic, sizeof(llvm::COFF::PEMagic));
  P += sizeof(llvm::COFF::PEMagic);

  coff_file_header COFF;
  memset(&COFF, 0, sizeof(COFF));
  COFF.Machine = llvm::COFF::IMAGE_FILE_MACHINE_AMD64;
  memcpy(P, &COFF, sizeof(COFF));
  P += sizeof(COFF);

  pe32plus_header PE;
  memset(&PE, 0, sizeof(PE));
  PE.Magic = llvm::COFF::PE32Header::PE32_PLUS;
  memcpy(P, &PE, sizeof(PE));
}

static void writeFile(StringRef Path, SectionMap &Map) {
  std::unique_ptr<llvm::FileOutputBuffer> Buffer;
  if (auto EC = FileOutputBuffer::create(
	Path, HeaderSize, Buffer, FileOutputBuffer::F_executable)) {
    llvm::errs() << "Failed to open " << Path << ": " << EC.message() << "\n";
    return;
  }
  writeHeader(Buffer->getBufferStart());
  Buffer->commit();
}

namespace lld {
namespace coff {

static SectionMap groupSections(SectionList &Sections) {
  SectionMap Result;
  for (std::unique_ptr<Section> &Sec : Sections)
    Result.insert({Sec->Name, Sec.get()});
  return Result;
}

void write(StringRef OutputPath, SectionList &Sections) {
  if (Sections.empty())
    return;
  SectionMap Map = groupSections(Sections);
  writeFile(OutputPath, Map);
}

} // namespace coff
} // namespace lld
