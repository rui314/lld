//===- Driver.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Config.h"
#include "Driver.h"
#include "InputFiles.h"
#include "Memory.h"
#include "SymbolTable.h"
#include "Writer.h"
#include "lld/Core/Error.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <memory>

using namespace llvm;

namespace lld {
namespace coff {

Configuration *Config;

static std::string getOutputPath(llvm::opt::InputArgList *Args) {
  if (auto *Arg = Args->getLastArg(OPT_out))
    return Arg->getValue();
  for (auto *Arg : Args->filtered(OPT_INPUT)) {
    if (!StringRef(Arg->getValue()).endswith_lower(".obj"))
      continue;
    SmallString<128> Val = StringRef(Arg->getValue());
    llvm::sys::path::replace_extension(Val, ".exe");
    return Val.str();
  }
  llvm_unreachable("internal error");
}

std::unique_ptr<InputFile> createFile(StringRef Path) {
  if (StringRef(Path).endswith_lower(".lib"))
    return llvm::make_unique<ArchiveFile>(Path);
  return llvm::make_unique<ObjectFile>(Path);
}

namespace {
class BumpPtrStringSaver : public llvm::cl::StringSaver {
public:
  BumpPtrStringSaver(lld::coff::StringAllocator *A) : Alloc(A) {}
  const char *SaveString(const char *S) override {
    return Alloc->save(S).data();
  }
  lld::coff::StringAllocator *Alloc;
};
}

// Parses .drectve section contents and returns a list of files
// specified by /defaultlib.
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
    Res->push_back(llvm::make_unique<ArchiveFile>(Path));
  }
  return std::error_code();
}

static std::error_code doLink(int Argc, const char *Argv[]) {
  // Parse command line options.
  Config = new Configuration();
  auto ArgsOrErr = parseArgs(Argc, Argv);
  if (auto EC = ArgsOrErr.getError())
    return EC;
  std::unique_ptr<llvm::opt::InputArgList> Args = std::move(ArgsOrErr.get());

  // Handle /help
  if (Args->hasArg(OPT_help)) {
    printHelp(Argv[0]);
    return std::error_code();
  }

  if (Args->filtered_begin(OPT_INPUT) == Args->filtered_end())
    return make_dynamic_error_code("no input files.");

  // Handle /verbose
  if (Args->hasArg(OPT_verbose))
    Config->Verbose = true;

  // Handle /entry
  if (auto *Arg = Args->getLastArg(OPT_entry))
    Config->EntryName = Arg->getValue();

  // Handle /machine
  auto MTOrErr = getMachineType(Args.get());
  if (auto EC = MTOrErr.getError())
    return EC;
  Config->MachineType = MTOrErr.get();

  // Handle /base
  if (auto *Arg = Args->getLastArg(OPT_base)) {
    uint64_t Addr, Size;
    if (auto EC = parseMemoryOption(Arg->getValue(), &Addr, &Size))
      return EC;
    Config->ImageBase = Addr;
  }

  // Parse all input files and put all symbols to the symbol table.
  // The symbol table will take care of name resolution.
  SymbolTable Symtab;
  for (auto *Arg : Args->filtered(OPT_INPUT)) {
    std::string Path = findFile(Arg->getValue());
    if (!Config->insertFile(Path))
      continue;
    if (auto EC = Symtab.addFile(createFile(Path)))
      return make_dynamic_error_code(Twine(Path) + ": " + EC.message());
  }
  if (Symtab.reportRemainingUndefines())
    return make_dynamic_error_code("link failed");

  // Write the result.
  Writer Out(&Symtab);
  if (auto EC = Out.write(getOutputPath(Args.get())))
    return EC;
  return std::error_code();
}

bool link(int Argc, const char *Argv[]) {
  if (auto EC = doLink(Argc, Argv)) {
    llvm::errs() << EC.message() << "\n";
    return false;
  }
  return true;
}

} // namespace coff
} // namespace lld
