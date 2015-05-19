//===- Writer.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "ImportTable.h"
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

void Writer::groupSections() {
  std::map<StringRef, std::vector<InputSection *>> Map;
  for (std::unique_ptr<ObjectFile> &File : Res->getFiles())
    for (InputSection &Sec : File->Sections)
      Map[stripDollar(Sec.Name)].push_back(&Sec);

  for (auto &P : Map) {
    StringRef SectionName = P.first;
    std::vector<InputSection *> &InputSections = P.second;
    sortByName(&InputSections);
    std::unique_ptr<OutputSection> OSec(
      new OutputSection(SectionName, OutputSections.size()));
    for (InputSection *ISec : InputSections) {
      ISec->Out = OSec.get();
      OSec->addChunk(&ISec->Chunk);
      OSec->Header.Characteristics = mergeCharacteristics(
	OSec->Header, *ISec->Header);
    }
    OutputSections.push_back(std::move(OSec));
  }
}

std::map<StringRef, std::vector<DefinedImplib *>>
groupImports(std::vector<DefinedImplib *> Impsyms) {
  std::map<StringRef, std::vector<DefinedImplib *>> Res;
  for (DefinedImplib *S : Impsyms)
    Res[S->DLLName].push_back(S);

  // Sort by symbol name
  auto comp = [](const DefinedImplib *A, const DefinedImplib *B) {
    return A->Name < B->Name;
  };
  for (auto &P : Res) {
    std::vector<DefinedImplib *> &V = P.second;
    std::stable_sort(V.begin(), V.end(), comp);
  }
  return Res;
}

void Writer::createImportTables() {
  if (Res->ImpSyms.empty())
    return;
  
  std::unique_ptr<OutputSection> Idata(
    new OutputSection(".idata", OutputSections.size()));
  Idata->Header.Characteristics = (llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA
				   | llvm::COFF::IMAGE_SCN_MEM_READ);

  std::vector<ImportTable *> Tabs;
  for (auto &P : groupImports(Res->ImpSyms)) {
    StringRef DLLName = P.first;
    std::vector<DefinedImplib *> &Imports = P.second;
    Tabs.push_back(new ImportTable(DLLName, Imports));
  }

  // Add the directory tables.
  for (ImportTable *T : Tabs)
    Idata->addChunk(&T->DirTab);
  Idata->addChunk(new NullChunk(sizeof(llvm::COFF::ImportDirectoryTableEntry)));

  // Add the import lookup tables.
  for (ImportTable *T : Tabs) {
    for (LookupChunk &C : T->LookupTables)
      Idata->addChunk(&C);
    Idata->addChunk(new NullChunk(8));
  }

  // Add the import address tables. Their contents are the same as the
  // lookup tables.
  for (ImportTable *T : Tabs) {
    for (LookupChunk &C : T->AddressTables)
      Idata->addChunk(&C);
    Idata->addChunk(new NullChunk(8));
  }

  IAT = &Tabs[0]->AddressTables[0];

  // Add the hint name table.
  for (ImportTable *T : Tabs)
    for (HintNameChunk &C : T->HintNameTables)
      Idata->addChunk(&C);

  // Add DLL names.
  for (ImportTable *T : Tabs)
    Idata->addChunk(&T->DirTab.Name);
  OutputSections.push_back(std::move(Idata));
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
  EndOfSectionTable = RoundUpToAlignment(
    HeaderSize + sizeof(coff_section) * OutputSections.size(), PageSize);

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
      if (!C->isBSS())
	memcpy(P + C->FileOff, C->getData(), C->getSize());
  }
}

void Writer::backfillHeaders() {
  PE->AddressOfEntryPoint = Res->getRVA("main");
  if (OutputSection *Text = findSection(".text")) {
    PE->BaseOfCode = Text->Header.VirtualAddress;
    PE->SizeOfCode = Text->Header.SizeOfRawData;
  }
  if (OutputSection *Idata = findSection(".idata")) {
    DataDirectory[COFF::IMPORT_TABLE].RelativeVirtualAddress = Idata->Header.VirtualAddress;
    DataDirectory[COFF::IMPORT_TABLE].Size = Idata->Header.SizeOfRawData;
    DataDirectory[COFF::IAT].RelativeVirtualAddress = IAT->RVA;
    DataDirectory[COFF::IAT].Size = IAT->getSize();
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
  createImportTables();
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
