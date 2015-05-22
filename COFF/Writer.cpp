//===- Writer.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Config.h"
#include "ImportTable.h"
#include "Reader.h"
#include "Writer.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <functional>
#include <map>
#include <utility>

using namespace llvm;
using namespace llvm::object;

namespace lld {
namespace coff {

static StringRef dropDollar(StringRef S) {
  return S.substr(0, S.find('$'));
}

static void forEachChunk(SymbolTable *Symtab, std::function<void(Chunk *)> F) {
  for (std::unique_ptr<ObjectFile> &File : Symtab->getFiles())
    for (std::unique_ptr<Chunk> &C : File->Chunks)
      if (C)
        F(C.get());
}

void Writer::markChunks() {
  forEachChunk(Symtab, [](Chunk *C) { if (C->isRoot()) C->markLive(); });
  cast<Defined>(Symtab->find("mainCRTStartup"))->markLive();

  if (Config->Verbose) {
    auto F = [](Chunk *C) { if (!C->isLive()) C->printDiscardMessage(); };
    forEachChunk(Symtab, F);
  }
}

void Writer::groupSections() {
  std::map<StringRef, std::vector<Chunk *>> Map;
  auto Push = [&](Chunk *C) {
    Map[dropDollar(C->getSectionName())].push_back(C);
  };
  forEachChunk(Symtab, Push);

  auto comp = [](Chunk *A, Chunk *B) {
    return A->getSectionName() < B->getSectionName();
  };
  for (auto &P : Map) {
    StringRef SectionName = P.first;
    std::vector<Chunk *> &Chunks = P.second;
    std::stable_sort(Chunks.begin(), Chunks.end(), comp);
    auto Sec = llvm::make_unique<OutputSection>(SectionName, OutputSections.size());
    for (Chunk *C : Chunks) {
      C->setOutputSection(Sec.get());
      Sec->addChunk(C);
      Sec->addPermissions(C->getPermissions());
    }
    OutputSections.push_back(std::move(Sec));
  }
}

std::map<StringRef, std::vector<DefinedImportData *>> Writer::groupImports() {
  std::map<StringRef, std::vector<DefinedImportData *>> Ret;
  OutputSection *Text = createSection(".text");
  for (std::unique_ptr<ImportFile> &P : Symtab->ImportFiles) {
    for (std::unique_ptr<Symbol> &S : P->getSymbols()) {
      if (auto *Sym = dyn_cast<DefinedImportData>(S.get())) {
        Ret[Sym->getDLLName()].push_back(Sym);
        continue;
      }
      Text->addChunk(cast<DefinedImportFunc>(S.get())->getChunk());
    }
  }

  // Sort by symbol name
  auto comp = [](DefinedImportData *A, DefinedImportData *B) {
    return A->getName() < B->getName();
  };
  for (auto &P : Ret) {
    std::vector<DefinedImportData *> &V = P.second;
    std::sort(V.begin(), V.end(), comp);
  }
  return Ret;
}

void Writer::createImportTables() {
  if (Symtab->ImportFiles.empty())
    return;

  std::vector<ImportTable> Tabs;
  for (auto &P : groupImports()) {
    StringRef DLLName = P.first;
    std::vector<DefinedImportData *> &Imports = P.second;
    Tabs.emplace_back(DLLName, Imports);
  }
  OutputSection *Idata = createSection(".idata");
  size_t NumChunks = Idata->getChunks().size();

  // Add the directory tables.
  for (ImportTable &T : Tabs)
    Idata->addChunk(T.DirTab);
  Idata->addChunk(new NullChunk(sizeof(llvm::COFF::ImportDirectoryTableEntry)));

  // Add the import lookup tables.
  for (ImportTable &T : Tabs) {
    for (LookupChunk *C : T.LookupTables)
      Idata->addChunk(C);
    Idata->addChunk(new NullChunk(8));
  }

  // Add the import address tables. Their contents are the same as the
  // lookup tables.
  for (ImportTable &T : Tabs) {
    for (LookupChunk *C : T.AddressTables)
      Idata->addChunk(C);
    Idata->addChunk(new NullChunk(8));
  }
  ImportAddressTable = Tabs[0].AddressTables[0];

  // Add the hint name table.
  for (ImportTable &T : Tabs)
    for (HintNameChunk *C : T.HintNameTables)
      Idata->addChunk(C);

  // Add DLL names.
  for (ImportTable &T : Tabs)
    Idata->addChunk(T.DLLName);

  // Claim ownership of all chunks in the .idata section.
  for (size_t I = NumChunks, E = Idata->getChunks().size(); I < E; ++I)
    Chunks.push_back(std::unique_ptr<Chunk>(Idata->getChunks()[I]));
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
  uint64_t Entry = cast<Defined>(Symtab->find("mainCRTStartup"))->getRVA();
  PE->AddressOfEntryPoint = Entry;
  PE->SizeOfStackReserve = 1024 * 1024;
  PE->SizeOfStackCommit = 4096;
  PE->SizeOfHeapReserve = 1024 * 1024;
  PE->SizeOfHeapCommit = 4096;
  PE->NumberOfRvaAndSize = NumberfOfDataDirectory;
  if (OutputSection *Text = findSection(".text")) {
    PE->BaseOfCode = Text->getRVA();
    PE->SizeOfCode = Text->getRawSize();
  }
  if (OutputSection *Data = findSection(".data"))
    PE->SizeOfInitializedData = Data->getRawSize();

  // Write data directory
  DataDirectory = reinterpret_cast<data_directory *>(P);
  P += sizeof(data_directory) * NumberfOfDataDirectory;
  if (OutputSection *Idata = findSection(".idata")) {
    using namespace llvm::COFF;
    DataDirectory[IMPORT_TABLE].RelativeVirtualAddress = Idata->getRVA();
    DataDirectory[IMPORT_TABLE].Size = Idata->getVirtualSize();
    DataDirectory[IAT].RelativeVirtualAddress = ImportAddressTable->getRVA();
    DataDirectory[IAT].Size = ImportAddressTable->getSize();
  }

  // The section table immediately follows the data directory.
  SectionTable = reinterpret_cast<coff_section *>(P);
  int Idx = 0;
  for (std::unique_ptr<OutputSection> &Out : OutputSections)
    SectionTable[Idx++] = *Out->getHeader();
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
  uint8_t *P = Buffer->getBufferStart();
  for (std::unique_ptr<OutputSection> &Sec : OutputSections) {
    if (Sec->getName() == ".text")
      memset(P + Sec->getFileOff(), 0xCC, Sec->getRawSize());
    for (Chunk *C : Sec->getChunks())
      if (!C->isBSS())
        memcpy(P + C->getFileOff(), C->getData(), C->getSize());
  }
}

OutputSection *Writer::findSection(StringRef Name) {
  for (std::unique_ptr<OutputSection> &S : OutputSections)
    if (S->getName() == Name)
      return S.get();
  return nullptr;
}

OutputSection *Writer::createSection(StringRef Name) {
  using namespace llvm::COFF;
  if (auto *S = findSection(Name))
    return S;

  const auto Read = IMAGE_SCN_MEM_READ;
  const auto Write = IMAGE_SCN_MEM_WRITE;
  const auto Execute = IMAGE_SCN_MEM_EXECUTE;
  uint32_t Perm = StringSwitch<uint32_t>(Name)
    .Case(".bss", IMAGE_SCN_CNT_UNINITIALIZED_DATA | Read | Write)
    .Case(".data", IMAGE_SCN_CNT_INITIALIZED_DATA | Read | Write)
    .Case(".idata", IMAGE_SCN_CNT_INITIALIZED_DATA | Read)
    .Case(".rdata", IMAGE_SCN_CNT_INITIALIZED_DATA | Read)
    .Case(".text", IMAGE_SCN_CNT_CODE | Read | Execute)
    .Default(0);
  if (!Perm)
    llvm_unreachable("unknown section name");
  auto S = new OutputSection(Name, OutputSections.size());
  S->addPermissions(Perm);
  OutputSections.push_back(std::unique_ptr<OutputSection>(S));
  return S;
}

void Writer::applyRelocations() {
  uint8_t *Buf = Buffer->getBufferStart();
  for (std::unique_ptr<OutputSection> &Sec : OutputSections)
    for (Chunk *C : Sec->getChunks())
      C->applyRelocations(Buf);
}

void Writer::write(StringRef OutputPath) {
  markChunks();
  groupSections();
  createImportTables();
  assignAddresses();
  removeEmptySections();
  openFile(OutputPath);
  writeHeader();
  writeSections();
  applyRelocations();
  Buffer->commit();
}

} // namespace coff
} // namespace lld
