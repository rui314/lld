//===- Driver.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

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
#include "llvm/Support/raw_ostream.h"
#include "lld/Core/Error.h"
#include <memory>
#include <sstream>

using llvm::ErrorOr;
using llvm::MemoryBuffer;
using llvm::StringRef;
using llvm::Twine;
using llvm::isa;
using llvm::cast;
using llvm::object::COFFObjectFile;

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

static ErrorOr<std::unique_ptr<COFFObjectFile>> readFile(StringRef path) {
  ErrorOr<std::unique_ptr<MemoryBuffer>> mbOrErr = MemoryBuffer::getFile(path);
  if (std::error_code ec = mbOrErr.getError())
    return ec;
  std::unique_ptr<MemoryBuffer> mb = std::move(mbOrErr.get());
  auto binOrErr = llvm::object::createBinary(mb->getMemBufferRef());
  if (std::error_code ec = binOrErr.getError())
    return ec;
  std::unique_ptr<llvm::object::Binary> bin = std::move(binOrErr.get());
  if (isa<COFFObjectFile>(bin.get()))
    return lld::make_dynamic_error_code(Twine(path) + " is not a COFF file.");
  return std::unique_ptr<COFFObjectFile>(cast<COFFObjectFile>(bin.release()));
}

namespace lld {

bool linkCOFF(int argc, const char *argv[]) {
  std::unique_ptr<llvm::opt::InputArgList> parsedArgs;
  COFFOptTable table;
  unsigned missingIndex;
  unsigned missingCount;
  parsedArgs.reset(table.ParseArgs(&argv[1], &argv[argc],
                                   missingIndex, missingCount));
  if (missingCount) {
    llvm::errs() << "error: missing arg value for '"
		 << parsedArgs->getArgString(missingIndex) << "' expected "
		 << missingCount << " argument(s).\n";
    return false;
  }

  for (auto *arg : parsedArgs->filtered(OPT_UNKNOWN)) {
    llvm::errs() << "warning: ignoring unknown argument: "
		 << arg->getSpelling() << "\n";
  }

  std::vector<std::unique_ptr<COFFObjectFile>> files;
  for (auto *arg : parsedArgs->filtered(OPT_INPUT)) {
    StringRef path = arg->getValue();
    ErrorOr<std::unique_ptr<COFFObjectFile>> file = readFile(arg->getValue());
    if (std::error_code ec = file.getError()) {
      llvm::errs() << "Cannot open " << path << ": " << ec.message() << "\n";
      continue;
    }
    files.push_back(std::move(file.get()));
  }

  return true;
}

}
