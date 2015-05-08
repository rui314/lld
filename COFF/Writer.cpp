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

namespace lld {
namespace coff {

static SectionMap groupSections(SectionList &Sections) {
  SectionMap Result;
  for (std::unique_ptr<Section> &Sec : Sections)
    Result.insert({Sec->Name, Sec.get()});
  return Result;
}

void Writer::open() {
  if (auto EC = FileOutputBuffer::create(
	Path, HeaderSize, Buffer, FileOutputBuffer::F_executable)) {
    llvm::errs() << "Failed to open " << Path << ": " << EC.message() << "\n";
    return;
  }

  // Write DOS stub header
  uint8_t *P = Buffer->getBufferStart();
  auto *DOS = reinterpret_cast<dos_header *>(P);
  DOS->Magic[0] = 'M';
  DOS->Magic[1] = 'Z';
  DOS->AddressOfRelocationTable = sizeof(dos_header);
  DOS->AddressOfNewExeHeader = DOSStubSize;

  // Write PE magic
  memcpy(P + DOSStubSize, llvm::COFF::PEMagic, sizeof(llvm::COFF::PEMagic));

  // Write some COFF header attributes
  COFF = reinterpret_cast<coff_file_header *>(
    P + DOSStubSize + sizeof(llvm::COFF::PEMagic));
  COFF->Machine = llvm::COFF::IMAGE_FILE_MACHINE_AMD64;

  // Write some PE header attributes
  PE = reinterpret_cast<pe32plus_header *>(
    P + DOSStubSize + sizeof(llvm::COFF::PEMagic) + sizeof(coff_file_header));
  PE->Magic = llvm::COFF::PE32Header::PE32_PLUS;
}

void Writer::write() {
  open();
  SectionMap Map = groupSections(Sections);
  Buffer->commit();
}

} // namespace coff
} // namespace lld
