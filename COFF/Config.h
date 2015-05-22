//===- Config.h -----------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_CONFIG_H
#define LLD_COFF_CONFIG_H

#include "llvm/ADT/StringRef.h"
#include <set>

namespace lld {
namespace coff {

class Configuration {
public:
  bool Verbose = false;

  bool insertFile(llvm::StringRef Path) {
    return VisitedFiles.insert(Path.lower()).second;
  }

private:
  std::set<std::string> VisitedFiles;
};

extern Configuration *Config;

} // namespace coff
} // namespace lld

#endif
