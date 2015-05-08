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

static uint64_t getSectionAlignment(const coff_section *Sec) {
  return uint64_t(1) << (((Sec->Characteristics & 0x00F00000) >> 20) - 1);
}

static StringRef stripDollar(StringRef Name) {
  return Name.substr(0, Name.find('$'));
}

void Writer::groupSections() {
  StringRef Last = "";
  for (std::unique_ptr<Section> &Sec : Sections) {
    StringRef Name = stripDollar(Sec->Name);
    if (Name != Last)
      OutputSections.emplace_back();
    OutputSection &Out = OutputSections.back();
    Out.Name = Name;
    Out.Sections.push_back(Sec.get());
    Last = Name;
  }
}

void Writer::assignAddresses() {
  uint64_t Off = 0;
  for (OutputSection &Out : OutputSections) {
    Off = RoundUpToAlignment(Off, PageSize);
    Off = RoundUpToAlignment(Off, getSectionAlignment(Out.Sections.front()->Header));
    Out.Header.PointerToRawData = Off;
    Out.Header.VirtualAddress = Off;
    for (Section *Sec : Out.Sections) {
      Off = RoundUpToAlignment(Off, getSectionAlignment(Sec->Header));
      Sec->FileOffset = Off;
      Sec->RVA = Off;
      Off += Sec->getSectionSize();
    }
  }
  SectionTotalSize = RoundUpToAlignment(Off, PageSize);

  for (auto I = OutputSections.begin(), E = OutputSections.end() - 1; I < E; ++I) {
    OutputSection &Curr = *I;
    OutputSection &Next = *(I + 1);
    Curr.Header.VirtualSize = Next.Header.PointerToRawData - Curr.Header.PointerToRawData;
  }
  OutputSection &Last = OutputSections.back();
  Last.Header.VirtualSize = SectionTotalSize - Last.Header.PointerToRawData;
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
  COFF->NumberOfSections = OutputSections.size();
  COFF->Characteristics = (llvm::COFF::IMAGE_FILE_EXECUTABLE_IMAGE
			   | llvm::COFF::IMAGE_FILE_RELOCS_STRIPPED);
  COFF->SizeOfOptionalHeader = sizeof(pe32plus_header)
    + sizeof(llvm::object::data_directory) * NumberfOfDataDirectory;

  // Write PE header
  PE = reinterpret_cast<pe32plus_header *>(P);
  P += sizeof(pe32plus_header);
  PE->Magic = llvm::COFF::PE32Header::PE32_PLUS;
  PE->ImageBase = 0x140000000;
  PE->SectionAlignment = SectionAlignment;
  PE->FileAlignment = FileAlignment;
  PE->MajorOperatingSystemVersion = 6;
  PE->MajorSubsystemVersion = 6;
  PE->SizeOfImage = SectionTotalSize;
  PE->SizeOfStackReserve = 1024 * 1024;
  PE->SizeOfStackCommit = 4096;
  PE->SizeOfHeapReserve = 1024 * 1024;
  PE->SizeOfHeapCommit = 4096;
  PE->NumberOfRvaAndSize = NumberfOfDataDirectory;

  // Write data directory
  DataDirectory = reinterpret_cast<data_directory *>(P);
  P += sizeof(data_directory) * NumberfOfDataDirectory;

  // Initialize SectionTable pointer
  SectionTable = reinterpret_cast<coff_section *>(P);
  PE->SizeOfHeaders = RoundUpToAlignment(
    HeaderSize + sizeof(coff_section) * OutputSections.size(), FileAlignment);
}

void Writer::open() {
  uint64_t Size = RoundUpToAlignment(
    HeaderSize + sizeof(coff_section) * OutputSections.size(), PageSize);
  Size += SectionTotalSize;
  if (auto EC = FileOutputBuffer::create(
	Path, Size, Buffer, FileOutputBuffer::F_executable)) {
    llvm::errs() << "Failed to open " << Path << ": " << EC.message() << "\n";
  }
}

void Writer::writeSections() {
  int Idx = 0;
  for (OutputSection &Out : OutputSections) {
    Out.finalize();
    SectionTable[Idx++] = Out.Header;
  }
}

void Writer::backfillHeaders() {
  for (OutputSection &Out : OutputSections) {
    if (Out.Name == ".text") {
      PE->SizeOfCode = Out.Header.VirtualSize;
      PE->BaseOfCode = Out.Header.VirtualAddress;
      return;
    }
  }
}

void Writer::write() {
  groupSections();
  assignAddresses();
  open();
  writeHeader();
  writeSections();
  backfillHeaders();
  Buffer->commit();
}

} // namespace coff
} // namespace lld
