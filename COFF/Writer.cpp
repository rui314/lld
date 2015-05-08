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
#include <algorithm>
#include <map>
#include <utility>

using namespace llvm;
using namespace llvm::object;

namespace lld {
namespace coff {

static void sortSections(SectionList &Sections) {
  std::sort(Sections.begin(), Sections.end(),
	    [](std::unique_ptr<Section> &A, std::unique_ptr<Section> &B) {
	      return A->Name < B->Name;
	    });
}

static uint64_t getSectionAlignment(const coff_section *Sec) {
  return uint64_t(1) << (((Sec->Characteristics & 0x00F00000) >> 20) - 1);
}

void Writer::assignAddress() {
  uint64_t Off = 0;
  StringRef Last = "";
  for (std::unique_ptr<Section> &Sec : Sections) {
    if (Last != Sec->Name) {
      ++NumSections;
      Off = RoundUpToAlignment(Off, 4096);
    }
    Last = Sec->Name;
    Off = RoundUpToAlignment(Off, getSectionAlignment(Sec->Sec));
    Sec->FileOffset = Off;
    Sec->RVA = Off;
    Off += Sec->getSectionSize();
  }
  SectionTotalSize = RoundUpToAlignment(Off, 4096);
}

void Writer::writeHeader() {
  // Write DOS stub header
  uint8_t *P = Buffer->getBufferStart();
  auto *DOS = reinterpret_cast<dos_header *>(P);
  P += DOSStubSize;
  DOS->Magic[0] = 'M';
  DOS->Magic[1] = 'Z';
  DOS->AddressOfRelocationTable = sizeof(dos_header);
  DOS->AddressOfNewExeHeader = DOSStubSize;

  // Write PE magic
  memcpy(P, llvm::COFF::PEMagic, sizeof(llvm::COFF::PEMagic));
  P += sizeof(llvm::COFF::PEMagic);

  // Write COFF header
  COFF = reinterpret_cast<coff_file_header *>(P);
  P += sizeof(coff_file_header);
  COFF->Machine = llvm::COFF::IMAGE_FILE_MACHINE_AMD64;
  COFF->NumberOfSections = NumSections;
  COFF->Characteristics = llvm::COFF::IMAGE_FILE_EXECUTABLE_IMAGE;
  COFF->SizeOfOptionalHeader = sizeof(pe32plus_header)
    + sizeof(llvm::object::data_directory) * NumberfOfDataDirectory;

  // Write PE header
  PE = reinterpret_cast<pe32plus_header *>(P);
  P += sizeof(pe32plus_header);
  PE->Magic = llvm::COFF::PE32Header::PE32_PLUS;
  PE->ImageBase = 0x400000;
  PE->SectionAlignment = SectionAlignment;
  PE->FileAlignment = 512;
  PE->SizeOfStackReserve = 1024 * 1024;
  PE->SizeOfStackCommit = 4096;
  PE->SizeOfHeapReserve = 1024 * 1024;;
  PE->SizeOfHeapCommit = 4096;
  PE->NumberOfRvaAndSize = NumberfOfDataDirectory;

  // Write data directory
  DataDirectory = reinterpret_cast<data_directory *>(P);
  P += sizeof(data_directory) * NumberfOfDataDirectory;

  // Initialize SectionTable pointer
  SectionTable = reinterpret_cast<coff_section *>(P);
  PE->SizeOfHeaders = RoundUpToAlignment(
    HeaderSize + sizeof(coff_section) * NumSections, 4096);
}

void Writer::open() {
  uint64_t Size = RoundUpToAlignment(
    HeaderSize + sizeof(coff_section) * NumSections, 4096);
  Size += SectionTotalSize;
  if (auto EC = FileOutputBuffer::create(
	Path, Size, Buffer, FileOutputBuffer::F_executable)) {
    llvm::errs() << "Failed to open " << Path << ": " << EC.message() << "\n";
  }
}

void Writer::writeSections() {
  int Idx = 0;
  StringRef Last = "";
  for (std::unique_ptr<Section> &Sec : Sections) {
    if (Last != Sec->Name) {
      coff_section &Hdr = SectionTable[Idx++];
      strncpy(Hdr.Name, Sec->Name.data(), std::min(Sec->Name.size(), size_t(8)));
    }
  }
}

void Writer::write() {
  sortSections(Sections);
  assignAddress();

  open();
  writeHeader();
  writeSections();
  Buffer->commit();
}

} // namespace coff
} // namespace lld
