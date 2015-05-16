//===- Writer.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Reader.h"
#include "Writer.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <map>
#include <utility>

using namespace llvm;
using namespace llvm::object;

namespace lld {
namespace coff {

static uint64_t getSectionAlignment(const InputSection *Sec) {
  unsigned Shift = ((Sec->Header->Characteristics & 0x00F00000) >> 20) - 1;
  return uint64_t(1) << Shift;
}

static StringRef stripDollar(StringRef Name) {
  return Name.substr(0, Name.find('$'));
}

static uint32_t
mergeCharacteristics(const coff_section &A, const coff_section &B) {
  uint32_t Mask = (llvm::COFF::IMAGE_SCN_MEM_SHARED
		   | llvm::COFF::IMAGE_SCN_MEM_EXECUTE
		   | llvm::COFF::IMAGE_SCN_MEM_READ
		   | llvm::COFF::IMAGE_SCN_CNT_CODE);
  return (A.Characteristics | B.Characteristics) & Mask;
}

static void sortByName(std::vector<InputSection *> *Sections) {
  auto comp = [](const InputSection *A, const InputSection *B) {
    return A->Name < B->Name;
  };
  std::stable_sort(Sections->begin(), Sections->end(), comp);
}

OutputSection::OutputSection(StringRef N, uint32_t SI,
			     std::vector<InputSection *> *InputSections)
  : Name(N), SectionIndex(SI) {
  memset(&Header, 0, sizeof(Header));
  sortByName(InputSections);

  uint64_t Off = 0;
  for (InputSection *Sec : *InputSections) {
    Chunk *C = &Sec->Chunk;
    Off = RoundUpToAlignment(Off, C->Align);
    C->RVA = Off;
    C->FileOff = Off;
    Chunks.push_back(C);
    Off += C->Data.size();
    Header.Characteristics = mergeCharacteristics(Header, *Sec->Header);
  }
  Header.VirtualSize = Off;
  Header.SizeOfRawData = RoundUpToAlignment(Off, FileAlignment);

  strncpy(Header.Name, Name.data(), std::min(Name.size(), size_t(8)));
  Header.SizeOfRawData = RoundUpToAlignment(Header.SizeOfRawData, FileAlignment);
}

void OutputSection::setRVA(uint64_t RVA) {
  Header.VirtualAddress = RVA;
  for (Chunk *C : Chunks)
    C->RVA += RVA;
}

void OutputSection::setFileOffset(uint64_t Off) {
  Header.PointerToRawData = Off;
  for (Chunk *C : Chunks)
    C->FileOff += Off;
}

void Writer::groupSections() {
  std::map<StringRef, std::vector<InputSection *>> Map;
  for (std::unique_ptr<ObjectFile> &File : Res->getFiles())
    for (InputSection &Sec : File->Sections)
      Map[stripDollar(Sec.Name)].push_back(&Sec);

  uint32_t Index = 0;
  for (auto &P : Map) {
    StringRef SectionName = P.first;
    std::vector<InputSection *> &Sections = P.second;
    std::unique_ptr<OutputSection> OSec(
      new OutputSection(SectionName, Index++, &Sections));
    for (InputSection *ISec : Sections)
      ISec->Out = OSec.get();
    OutputSections.push_back(std::move(OSec));
  }
  EndOfSectionTable = RoundUpToAlignment(
    HeaderSize + sizeof(coff_section) * OutputSections.size(), PageSize);
}

void Writer::removeEmptySections() {
  auto IsEmpty = [](const std::unique_ptr<OutputSection> &S) {
    return S->Header.VirtualSize == 0;
  };
  OutputSections.erase(std::remove_if(OutputSections.begin(),
				      OutputSections.end(),
				      IsEmpty),
		       OutputSections.end());
}

void Writer::assignAddresses() {
  uint64_t RVA = 0x1000;
  uint64_t FileOff = EndOfSectionTable;
  uint64_t InitRVA = RVA;
  uint64_t InitFileOff = FileOff;
  for (std::unique_ptr<OutputSection> &Out : OutputSections) {
    Out->setRVA(RVA);
    Out->setFileOffset(FileOff);
    RVA += RoundUpToAlignment(Out->Header.VirtualSize, PageSize);
    FileOff += RoundUpToAlignment(Out->Header.SizeOfRawData, FileAlignment);
  }
  SectionTotalSizeMemory = RoundUpToAlignment(RVA - InitRVA, PageSize);
  SectionTotalSizeDisk = RoundUpToAlignment(FileOff - InitFileOff, FileAlignment);
}

void Writer::writeHeader() {
  // Write DOS stub
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
			   | llvm::COFF::IMAGE_FILE_RELOCS_STRIPPED
			   | llvm::COFF::IMAGE_FILE_LARGE_ADDRESS_AWARE);
  COFF->SizeOfOptionalHeader = sizeof(pe32plus_header)
    + sizeof(llvm::object::data_directory) * NumberfOfDataDirectory;

  // Write PE header
  PE = reinterpret_cast<pe32plus_header *>(P);
  P += sizeof(pe32plus_header);
  PE->Magic = llvm::COFF::PE32Header::PE32_PLUS;
  PE->ImageBase = ImageBase;
  PE->SectionAlignment = SectionAlignment;
  PE->FileAlignment = FileAlignment;
  PE->MajorOperatingSystemVersion = 6;
  PE->MajorOperatingSystemVersion = 6;
  PE->MajorSubsystemVersion = 6;
  PE->Subsystem = llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_CUI;
  PE->SizeOfImage = EndOfSectionTable + SectionTotalSizeMemory;
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

void Writer::openFile(StringRef OutputPath) {
  uint64_t Size = EndOfSectionTable + SectionTotalSizeDisk;
  if (auto EC = FileOutputBuffer::create(
        OutputPath, Size, Buffer, FileOutputBuffer::F_executable)) {
    llvm::errs() << "Failed to open " << OutputPath
		 << ": " << EC.message() << "\n";
  }
}

void Writer::writeSections() {
  int Idx = 0;
  for (std::unique_ptr<OutputSection> &Out : OutputSections)
    SectionTable[Idx++] = Out->Header;
  uint8_t *P = Buffer->getBufferStart();
  for (std::unique_ptr<OutputSection> &OSec : OutputSections) {
    if (OSec->Name == ".text")
      memset(P + OSec->Header.PointerToRawData, 0xCC, OSec->Header.SizeOfRawData);
    for (Chunk *C : OSec->Chunks)
      memcpy(P + C->FileOff, C->Data.data(), C->Data.size());
  }
}

void Writer::backfillHeaders() {
  PE->AddressOfEntryPoint = Res->getRVA("main");
  OutputSection *Text = findSection(".text");
  if (Text) {
    PE->SizeOfCode = Text->Header.SizeOfRawData;
    PE->BaseOfCode = Text->Header.VirtualAddress;
  }
}

OutputSection *Writer::findSection(StringRef name) {
  for (std::unique_ptr<OutputSection> &S : OutputSections)
    if (S->Name == name)
      return S.get();
  return nullptr;
}

void Writer::applyRelocations() {
  uint8_t *Buf = Buffer->getBufferStart();
  for (std::unique_ptr<OutputSection> &Sec : OutputSections)
    for (Chunk *C : Sec->Chunks)
      C->applyRelocations(Buf);
}

void Writer::write(StringRef OutputPath) {
  groupSections();
  assignAddresses();
  removeEmptySections();
  openFile(OutputPath);
  writeHeader();
  writeSections();
  applyRelocations();
  backfillHeaders();
  Buffer->commit();
}

} // namespace coff
} // namespace lld
