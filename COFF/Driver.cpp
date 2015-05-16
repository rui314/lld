//===- Driver.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Resolver.h"
#include "Symbol.h"
#include "Writer.h"
#include "lld/Core/Error.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Optional.h"
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
#include "llvm/Support/Process.h"
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

static std::string
getOutputPath(std::unique_ptr<llvm::opt::InputArgList> &Args) {
  if (auto *Arg = Args->getLastArg(OPT_out))
    return Arg->getValue();
  SmallString<128> Val = Args->getLastArg(OPT_INPUT)[0].getValue();
  llvm::sys::path::replace_extension(Val, ".exe");
  return Val.str();
}

// Split the given string with the path separator.
static std::vector<StringRef> splitPathList(StringRef str) {
  std::vector<StringRef> ret;
  while (!str.empty()) {
    StringRef path;
    std::tie(path, str) = str.split(';');
    ret.push_back(path);
  }
  return ret;
}

static std::string findFile(StringRef Filename) {
  if (llvm::sys::fs::exists(Filename))
    return Filename;
  llvm::Optional<std::string> Env = llvm::sys::Process::GetEnv("LIB");
  if (!Env.hasValue())
    return "";
  for (StringRef Dir : splitPathList(*Env)) {
    SmallString<128> Path = Dir;
    llvm::sys::path::append(Path, Filename);
    if (llvm::sys::fs::exists(Path.str()))
      return Path.str();
  }
  return "";
}

namespace lld {
namespace coff {

bool link(int Argc, const char *Argv[]) {
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

  Resolver Res;
  for (auto *Arg : Args->filtered(OPT_INPUT)) {
    std::string Path = findFile(Arg->getValue());
    if (StringRef(Path).endswith_lower(".lib")) {
      ErrorOr<std::unique_ptr<ArchiveFile>> FileOrErr = ArchiveFile::create(Path);
      if (auto EC = FileOrErr.getError()) {
	llvm::errs() << "Cannot open " << Path << ": " << EC.message() << "\n";
	continue;
      }
      if (auto EC = Res.addFile(std::move(FileOrErr.get()))) {
	llvm::errs() << "addFile failed: " << Path << ": " << EC.message() << "\n";
	return false;
      }
      continue;
    }
    ErrorOr<std::unique_ptr<ObjectFile>> FileOrErr = ObjectFile::create(Path);
    if (auto EC = FileOrErr.getError()) {
      llvm::errs() << "Cannot open " << Path << ": " << EC.message() << "\n";
      continue;
    }
    if (auto EC = Res.addFile(std::move(FileOrErr.get()))) {
      llvm::errs() << "addFile failed: " << Path << ": " << EC.message() << "\n";
      return false;
    }
  }
  if (Res.reportRemainingUndefines())
    return false;

  Writer OutFile(&Res);
  OutFile.write(getOutputPath(Args));
  return true;
}

}
}
