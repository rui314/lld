Section-based PE/COFF Linker
============================

This directory contains an experimental linker for the PE/COFF file
format. Because the fundamental design of this port is different from
the other ports of the LLD, this port is separated to this directory.

The other ports are based on the Atom model, in which symbols and
references are represented as vetices and edges of a graph. The port
in this directory is on the other hand based on sections to aim for
simplicity and better performance. Our plan is to experiment the idea
by implementing an linker for the PE/COFF format and then apply the
same idea to the ELF if proved to be useful.

Overall Design
--------------

This is a list of important data types in this linker.

* Symbol and SymbolBody

  SymbolBody is a class for symbols, which may be read from symbol
  tables of object files or archive file headers, but may be created
  by the linker itself.

  There are mainly three types of SymbolBodies, Defined, Undefined and
  CanBeDefined. Defined symbols are for all symbols that are
  considered as "resolved", including real defined symbols, COMDAT
  symbols, common symbols, absolute symbols, linker-created symbols,
  etc. Undefined symbols are for undefined symbols, which need to be
  replaced by Defined symbols by the resolver. CanBeDefined symbols
  represents symbols we found in archive file headers -- which can
  turn into Defined symbols if we read archieve members, but we
  haven't done that yet.

  Symbol is a pointer to SymbolBody. The resolver works on Symbols and
  mutates their pointers, so that if you keep Symbols, they will
  automatically point to resolved symbols (SymbolBodies) after name
  resolution.

* Chunk

  Chunk represents a chunk of data that will occupy space in an
  output. They may be backed by sections of input files, but can be
  something different, such as common or BSS symbols. The linker may
  also create chunks itself and append it to an output.

* InputFile

  InputFile is a superclass for file readers. We have a different
  subclasse for each input file type, such as regular object file,
  archive file, etc. They are responsible for creating SymbolBodies
  and Chunks.

* SymbolTable

  SymbolTable is basically a hash table from StringRefs to Symbols,
  with a logic to resolve symbol conflicts. It resolves symbols by
  symbol types. For example, if we have Undefined and Defined symbols,
  we chooses the latter. If we have CanBeDefined and Undefined, we
  choose the former and trigger it to load the archive member to
  actually resolve the symbol.

The linking process is drived by the driver. The driver

1. processes command line options,
2. creates a symbol table,
3. creates an InputFile for each input file and put all symbols in it
   to the symbol table,
4. checks if there's no remaining undefined symbols,
5. creates a writer,
6. and passes the symbol table to the writer to write the result to a
   file.

Notes on Performance
--------------------

Currently it's able to self-host on the Windows platform. It takes 1.5
seconds to self-host on my Xeon 2580 machine, while the existing
Atom-based linker takes 5 seconds to self-host. We believe the
performance difference comes from simplification and optimizations we
made to the new port. The optimizations are listed below.

* Reduced number of relocation table reads

  In the existing design, relocation tables are read from beginning to
  construct graphs. In this design, they are not read until we
  actually apply relocations.

  This optimization has two benefits: One is that we don't create
  additional objects for relocations but instead consume relocation
  tables directly. The other is that it reduces number of relocation
  entries we have to read, because we don't read relocations for
  dead-stripped COMDAT sections. Large C++ programs tend to consist of
  lots of COMDAT sections. In the existing design, the time to process
  relocation table is linear to size of input. In this new model, it's
  linear to size of output.

* Reduced number of symbol table lookup

  Symbol table lookup can be a heavy operation because number of
  symbols can be very large and each symbol name can also be very long
  (think of C++ mangled symbols -- time to compute a hash value for a
  string is linear to its length.)

  We look up the symbol table exactly only once for each symbol in the
  new design. This is achieved by this: we don't store symbols
  directory to the symbol table, but instead stores pointers to
  symbols.  All symbol accesses involving name resolution go through
  the pointers. The resolver mutates the pointers as it resolves
  symbols. We look up the hash table and hold pointers returned from
  that. Once the resolver is done, all pointers returned from the
  symbol table should point to appropriate symbols, so that you can
  just dereference them.

  This design is also good for parallelism because it allows us to
  make the resolver multi-threaded which "resolves" symbols by
  swapping pointers using compare-and-swap.

* Reduced number of file visits

  The symbol table implements the Windows linker semantics. We treat
  the symbol table as a bucket of all known symbols, including symbols
  in archive file headers, and put all symbols into one bucket as we
  visit new files. That means we visit each file only once.

  This is different from the Unix linker semantics, in which we keep
  only undefined symbols and visit each file one by one until we
  resolve all undefined symbols. In this model, we have to visit
  archive files multiple times if there are circular dependencies
  between archives.
