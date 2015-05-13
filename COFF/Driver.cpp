//===- Driver.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Writer.h"
#include "lld/Core/Error.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Object/COFF.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <memory>
#include <sstream>

using namespace llvm;
using llvm::object::COFFObjectFile;
using llvm::object::coff_section;

// Create enum with OPT_xxx values for each option in Options.td
enum {
  OPT_INVALID = 0,
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM, \
               HELP, META) \
          OPT_##ID,
#include "Options.inc"
#undef OPTION
};

// Create prefix string literals used in Options.td
#define PREFIX(NAME, VALUE) const char *const NAME[] = VALUE;
#include "Options.inc"
#undef PREFIX

// Create table mapping all options defined in Options.td
static const llvm::opt::OptTable::Info infoTable[] = {
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM, \
               HELPTEXT, METAVAR)   \
  { PREFIX, NAME, HELPTEXT, METAVAR, OPT_##ID, llvm::opt::Option::KIND##Class, \
    PARAM, FLAGS, OPT_##GROUP, OPT_##ALIAS, ALIASARGS },
#include "Options.inc"
#undef OPTION
};

namespace {
class COFFOptTable : public llvm::opt::OptTable {
public:
  COFFOptTable()
    : OptTable(infoTable, llvm::array_lengthof(infoTable),
	       /* ignoreCase */ true) {}
};
} // anonymous namespace

static ErrorOr<std::unique_ptr<COFFObjectFile>> readFile(StringRef Path) {
  ErrorOr<std::unique_ptr<MemoryBuffer>> MBOrErr = MemoryBuffer::getFile(Path);
  if (std::error_code EC = MBOrErr.getError())
    return EC;
  std::unique_ptr<MemoryBuffer> MB = std::move(MBOrErr.get());
  auto BinOrErr = llvm::object::createBinary(MB->getMemBufferRef());
  MB.release(); // leak
  if (std::error_code EC = BinOrErr.getError())
    return EC;
  std::unique_ptr<llvm::object::Binary> Bin = std::move(BinOrErr.get());
  if (!isa<COFFObjectFile>(Bin.get()))
    return lld::make_dynamic_error_code(Twine(Path) + " is not a COFF file.");
  return std::unique_ptr<COFFObjectFile>(cast<COFFObjectFile>(Bin.release()));
}

static void readSections(lld::coff::SectionList &Result, COFFObjectFile *File) {
  for (const auto &SectionRef : File->sections()) {
    const coff_section *Sec = File->getCOFFSection(SectionRef);
    StringRef Name;
    if (auto EC = File->getSectionName(Sec, Name)) {
      llvm::errs() << "Failed to get a section name: " << EC.message() << "\n";
      return;
    }
    if (Sec->Characteristics & llvm::COFF::IMAGE_SCN_LNK_REMOVE)
      continue;
    Result.push_back(make_unique<lld::coff::Section>(File, Sec, Name));
  }
}

static std::string replaceExtension(StringRef Path, StringRef Ext) {
  SmallString<128> Val = Path;
  llvm::sys::path::replace_extension(Val, Ext);
  return Val.str();
}

namespace lld {

bool linkCOFF(int Argc, const char *Argv[]) {
  COFFOptTable Table;
  unsigned MissingIndex;
  unsigned MissingCount;
  std::unique_ptr<llvm::opt::InputArgList> Args(
    Table.ParseArgs(&Argv[1], &Argv[Argc], MissingIndex, MissingCount));
  if (MissingCount) {
    llvm::errs() << "error: missing arg value for '"
		 << Args->getArgString(MissingIndex) << "' expected "
		 << MissingCount << " argument(s).\n";
    return false;
  }

  for (auto *Arg : Args->filtered(OPT_UNKNOWN)) {
    llvm::errs() << "warning: ignoring unknown argument: "
		 << Arg->getSpelling() << "\n";
  }

  if (Args->filtered_begin(OPT_INPUT) == Args->filtered_end()) {
    llvm::errs() << "no input files.\n";
    return true;
  }

  std::vector<std::unique_ptr<COFFObjectFile>> Files;
  std::vector<std::unique_ptr<lld::coff::Section>> Sections;
  for (auto *Arg : Args->filtered(OPT_INPUT)) {
    StringRef Path = Arg->getValue();
    ErrorOr<std::unique_ptr<COFFObjectFile>> FileOrErr = readFile(Arg->getValue());
    if (std::error_code EC = FileOrErr.getError()) {
      llvm::errs() << "Cannot open " << Path << ": " << EC.message() << "\n";
      continue;
    }
    std::unique_ptr<COFFObjectFile> File = std::move(FileOrErr.get());
    readSections(Sections, File.get());
    Files.push_back(std::move(File));
  }

  std::string OutputFile;
  if (auto *Arg = Args->getLastArg(OPT_out)) {
    OutputFile = Arg->getValue();
  } else {
    StringRef Path = Args->getLastArg(OPT_INPUT)[0].getValue();
    OutputFile = replaceExtension(Path, ".exe");
  }

  coff::Writer Writer(OutputFile);
  Writer.addSections(std::move(Sections));
  Writer.write();
  return true;
}

}
