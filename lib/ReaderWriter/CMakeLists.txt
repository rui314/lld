add_subdirectory(ELF)
add_subdirectory(MachO)
add_subdirectory(PECOFF)
add_subdirectory(YAML)

if (MSVC)
  add_definitions(-wd4062) # Suppress 'warning C4062: Enumerator has no associated handler in a switch statement.'
endif()

add_llvm_library(lldReaderWriter
  CoreLinkingContext.cpp
  FileArchive.cpp
  LinkerScript.cpp
  LINK_LIBS
    lldCore
    lldYAML
    LLVMObject
    LLVMSupport
  )
