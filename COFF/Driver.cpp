//===- Driver.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Allocator.h"
#include "CommandLine.h"
#include "Config.h"
#include "Reader.h"
#include "SymbolTable.h"
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
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"
#include <memory>
#include <mutex>
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

class BumpPtrStringSaver : public llvm::cl::StringSaver {
public:
  BumpPtrStringSaver(lld::coff::StringAllocator *A) : Alloc(A) {}

  const char *SaveString(const char *S) override {
    return Alloc->save(S).data();
  }

private:
  lld::coff::StringAllocator *Alloc;
};

}

static std::string getOutputPath(llvm::opt::InputArgList *Args) {
  if (auto *Arg = Args->getLastArg(OPT_out))
    return Arg->getValue();
  auto *Arg = *Args->filtered_begin(OPT_INPUT);
  SmallString<128> Val = Arg->getValue();
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

namespace lld {
namespace coff {

Configuration *Config;

std::string findLib(StringRef Filename) {
  if (llvm::sys::fs::exists(Filename))
    return Filename;
  std::string Name;
  if (Filename.endswith_lower(".lib")) {
    Name = Filename;
  } else {
    Name = (Filename + ".lib").str();
  }

  llvm::Optional<std::string> Env = llvm::sys::Process::GetEnv("LIB");
  if (!Env.hasValue())
    return Filename;
  for (StringRef Dir : splitPathList(*Env)) {
    SmallString<128> Path = Dir;
    llvm::sys::path::append(Path, Name);
    if (llvm::sys::fs::exists(Path.str()))
      return Path.str();
  }
  return Filename;
}

std::string findFile(StringRef Filename) {
  if (llvm::sys::fs::exists(Filename))
    return Filename;
  llvm::Optional<std::string> Env = llvm::sys::Process::GetEnv("LIB");
  if (!Env.hasValue())
    return Filename;
  for (StringRef Dir : splitPathList(*Env)) {
    SmallString<128> Path = Dir;
    llvm::sys::path::append(Path, Filename);
    if (llvm::sys::fs::exists(Path.str()))
      return Path.str();
  }
  return Filename;
}

ErrorOr<std::unique_ptr<InputFile>> createFile(StringRef Path) {
  if (StringRef(Path).endswith_lower(".lib"))
    return ArchiveFile::create(Path);
  return ObjectFile::create(Path);
}

std::error_code parseDirectives(StringRef S,
                                std::vector<std::unique_ptr<InputFile>> *Res,
                                StringAllocator *Alloc) {
  SmallVector<const char *, 16> Tokens;
  Tokens.push_back("link"); // argv[0] value. Will be ignored.
  BumpPtrStringSaver Saver(Alloc);
  llvm::cl::TokenizeWindowsCommandLine(S, Saver, Tokens);
  Tokens.push_back(nullptr);
  int Argc = Tokens.size() - 1;
  const char **Argv = &Tokens[0];

  auto ArgsOrErr = parseArgs(Argc, Argv);
  if (auto EC = ArgsOrErr.getError())
    return EC;
  std::unique_ptr<llvm::opt::InputArgList> Args = std::move(ArgsOrErr.get());

  for (auto *Arg : Args->filtered(OPT_defaultlib)) {
    std::string Path = findLib(Arg->getValue());
    if (!Config->insertFile(Path))
      continue;
    ErrorOr<std::unique_ptr<InputFile>> FileOrErr = ArchiveFile::create(Path);
    if (auto EC = FileOrErr.getError())
      return EC;
    Res->push_back(std::move(FileOrErr.get()));
  }
  return std::error_code();
}

bool link(int Argc, const char *Argv[]) {
  Config = new Configuration();
  auto ArgsOrErr = parseArgs(Argc, Argv);
  if (auto EC = ArgsOrErr.getError()) {
    llvm::errs() << EC.message() << "\n";
    return false;
  }
  std::unique_ptr<llvm::opt::InputArgList> Args = std::move(ArgsOrErr.get());

  if (Args->hasArg(OPT_verbose))
    Config->Verbose = true;

  SymbolTable Symtab;
  for (auto *Arg : Args->filtered(OPT_INPUT)) {
    std::string Path = findFile(Arg->getValue());
    if (!Config->insertFile(Path))
      continue;

    ErrorOr<std::unique_ptr<InputFile>> FileOrErr = createFile(Path);
    if (auto EC = FileOrErr.getError()) {
      llvm::errs() << "Cannot open " << Path << ": " << EC.message() << "\n";
      return false;
    }
    if (auto EC = Symtab.addFile(std::move(FileOrErr.get()))) {
      llvm::errs() << "addFile failed: " << Path << ": " << EC.message() << "\n";
      return false;
    }
  }
  if (Symtab.reportRemainingUndefines())
    return false;

  Writer OutFile(&Symtab);
  if (auto EC = OutFile.write(getOutputPath(Args.get()))) {
    llvm::errs() << EC.message() << "\n";
    return false;
  }
  return true;
}

}
}
