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

#include <set>

namespace lld {
namespace coff {

struct Configuration {
  bool Verbose = false;
  std::set<std::string> VisitedFiles;
};

extern Configuration *Config;

} // namespace coff
} // namespace lld

#endif
