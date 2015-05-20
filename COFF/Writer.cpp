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

void Writer::groupSections() {
  std::map<StringRef, std::vector<InputSection *>> Map;
  for (std::unique_ptr<ObjectFile> &File : Res->getFiles())
    for (InputSection &Sec : File->Sections)
      Map[Sec.getNameDropDollar()].push_back(&Sec);

  auto comp = [](InputSection *A, InputSection *B) {
    return A->getName() < B->getName();
  };

  for (auto &P : Map) {
    StringRef SectionName = P.first;
    std::vector<InputSection *> &InputSections = P.second;
    std::stable_sort(InputSections.begin(), InputSections.end(), comp);
    std::unique_ptr<OutputSection> OSec(
      new OutputSection(SectionName, OutputSections.size()));
    for (InputSection *ISec : InputSections) {
      ISec->setOutputSection(OSec.get());
      OSec->addChunk(ISec->getChunk());
      OSec->addPermission(ISec->getPermission());
    }
    OutputSections.push_back(std::move(OSec));
  }
}

std::map<StringRef, std::vector<DefinedImportData *>> Writer::groupImports() {
  std::map<StringRef, std::vector<DefinedImportData *>> Ret;
  OutputSection *Text = findSection(".text");
  for (std::unique_ptr<ImplibFile> &P : Res->ImplibFiles) {
    for (Symbol *S : P->getSymbols()) {
      if (auto *Sym = dyn_cast<DefinedImportData>(S)) {
	Ret[Sym->getDLLName()].push_back(Sym);
	continue;
      }
      Text->addChunk(cast<DefinedImportFunc>(S)->getChunk());
    }
  }

  // Sort by symbol name
  auto comp = [](DefinedImportData *A, DefinedImportData *B) {
    return A->getName() < B->getName();
  };
  for (auto &P : Ret) {
    std::vector<DefinedImportData *> &V = P.second;
    std::stable_sort(V.begin(), V.end(), comp);
  }
  return Ret;
}

void Writer::createImportTables() {
  if (Res->ImplibFiles.empty())
    return;

  std::unique_ptr<OutputSection> Idata(
    new OutputSection(".idata", OutputSections.size()));
  Idata->addPermission(llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA
		       | llvm::COFF::IMAGE_SCN_MEM_READ);

  std::vector<ImportTable *> Tabs;
  for (auto &P : groupImports()) {
    StringRef DLLName = P.first;
    std::vector<DefinedImportData *> &Imports = P.second;
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
    return S->getVirtualSize() == 0;
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
    RVA += RoundUpToAlignment(Out->getVirtualSize(), PageSize);
    FileOff += RoundUpToAlignment(Out->getRawSize(), FileAlignment);
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
    SectionTable[Idx++] = *Out->getHeader();
  uint8_t *P = Buffer->getBufferStart();
  for (std::unique_ptr<OutputSection> &OSec : OutputSections) {
    if (OSec->getName() == ".text")
      memset(P + OSec->getFileOff(), 0xCC, OSec->getRawSize());
    for (Chunk *C : OSec->getChunks())
      if (!C->isBSS())
	memcpy(P + C->getFileOff(), C->getData(), C->getSize());
  }
}

void Writer::backfillHeaders() {
  PE->AddressOfEntryPoint = Res->getRVA("main");
  if (OutputSection *Text = findSection(".text")) {
    PE->BaseOfCode = Text->getRVA();
    PE->SizeOfCode = Text->getRawSize();
  }
  if (OutputSection *Data = findSection(".data")) {
    PE->SizeOfInitializedData = Data->getRawSize();
  }
  if (OutputSection *Idata = findSection(".idata")) {
    DataDirectory[COFF::IMPORT_TABLE].RelativeVirtualAddress = Idata->getRVA();
    DataDirectory[COFF::IMPORT_TABLE].Size = Idata->getVirtualSize();
    DataDirectory[COFF::IAT].RelativeVirtualAddress = IAT->getRVA();
    DataDirectory[COFF::IAT].Size = IAT->getSize();
  }
}

OutputSection *Writer::findSection(StringRef name) {
  for (std::unique_ptr<OutputSection> &S : OutputSections)
    if (S->getName() == name)
      return S.get();
  return nullptr;
}

void Writer::applyRelocations() {
  uint8_t *Buf = Buffer->getBufferStart();
  for (std::unique_ptr<OutputSection> &Sec : OutputSections)
    for (Chunk *C : Sec->getChunks())
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
