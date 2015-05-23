set(LLVM_TARGET_DEFINITIONS Options.td)
tablegen(LLVM Options.inc -gen-opt-parser-defs)
add_public_tablegen_target(COFFOptionsTableGen)

add_llvm_library(lldCOFF
  Chunks.cpp
  CommandLine.cpp
  Driver.cpp
  Reader.cpp
  SymbolTable.cpp
  Writer.cpp
  )

add_dependencies(lldCOFF COFFOptionsTableGen)
