The New PE/COFF Linker
======================

This directory contains an experimental linker for the PE/COFF file
format. Because the fundamental design of this port is different from
the other ports of the LLD, this port is separated to this directory.

The other ports are based on the Atom model, in which symbols and
references are represented as vertices and edges of graphs. The port
in this directory is on the other hand based on sections. The aim is
simplicity and better performance. Our plan is to implement a linker
for the PE/COFF format based on a different idea, and then apply the
same idea to the ELF if proved to be effective.

Overall Design
--------------

This is a list of important data types in this linker.

* Symbol and SymbolBody

  SymbolBody is a class for symbols, which may be read from symbol
  tables of object files or archive file headers. They also can be
  created by the linker itself out of nothing.

  There are mainly three types of SymbolBodies: Defined, Undefined, or
  CanBeDefined. Defined symbols are for all symbols that are
  considered as "resolved", including real defined symbols, COMDAT
  symbols, common symbols, absolute symbols, linker-created symbols,
  etc. Undefined symbols are for undefined symbols, which need to be
  replaced by Defined symbols by the resolver. CanBeDefined symbols
  represent symbols we found in archive file headers -- which can
  turn into Defined symbols if we read archieve members, but we
  haven't done that yet.

  Symbol is a pointer to a SymbolBody. There's only one Symbol for
  each unique symbol name, so there can be many SymbolBodies that a
  Symbol can point to. The resolver keeps the Symbol pointer to always
  point to the "best" SymbolBody. Pointer mutation is the resolve
  operation.

* Chunk

  Chunk represents a chunk of data that will occupy space in an
  output. They may be backed by sections of input files, but can be
  something different, if they are for common or BSS symbols. The
  linker may also create chunks out of nothing to append additional
  data to an output.

  Chunks know about their size, how to copy their data to mmap'ed
  outputs, and how to apply relocations to them. Specifically,
  section-based chunks know how to read relocation tables and how to
  apply them.

* InputFile

  InputFile is a superclass for file readers. We have a different
  subclasses for each input file type, such as regular object file,
  archive file, etc. They are responsible for creating SymbolBodies
  and Chunks.

* SymbolTable

  SymbolTable is basically a hash table from strings to Symbols,
  with a logic to resolve symbol conflicts. It resolves conflicts by
  symbol type. For example, if we add Undefined and Defined symbols,
  the symbol table will keep the latter. If we add CanBeDefined and
  Undefined, it will keep the former, but it will also trigger the
  CanBeDefined symbol to load the archive member to actually resolve
  the symbol.

The linking process is drived by the driver. The driver

1. processes command line options,
2. creates a symbol table,
3. creates an InputFile for each input file and put all symbols in it
   into the symbol table,
4. checks if there's no remaining undefined symbols,
5. creates a writer,
6. and passes the symbol table to the writer to write the result to a
   file.

Performance
-----------

Currently it's able to self-host on the Windows platform. It takes 1.5
seconds to self-host on my Xeon 2580 machine, while the existing
Atom-based linker takes 5 seconds to self-host. We believe the
performance difference comes from simplification and optimizations we
made to the new port. The differences are listed below.

* Reduced number of relocation table reads

  In the existing design, relocation tables are read from beginning to
  construct graphs because they consist of graph edges. In the new
  design, they are not read until we actually apply relocations.

  This simplification has two benefits. One is that we don't create
  additional objects for relocations but instead consume relocation
  tables directly. The other is that it reduces number of relocation
  entries we have to read, because we won't read relocations for
  dead-stripped COMDAT sections. Large C++ programs tend to consist of
  lots of COMDAT sections. In the existing design, the time to process
  relocation table is linear to size of input. In this new model, it's
  linear to size of output.

* Reduced number of symbol table lookup

  Symbol table lookup can be a heavy operation because number of
  symbols can be very large and each symbol name can be very long
  (think of C++ mangled symbols -- time to compute a hash value for a
  string is linear to the length.)

  We look up the symbol table exactly only once for each symbol in the
  new design. This is achieved by the separation of Symbol and
  SymbolBody. Once you get a pointer to a Symbol by looking up the
  symbol table, you can always get the latest symbol resolution result
  by just dereferencing the pointer. (I'm not sure if the idea is new
  to the linker. At least, all other linkers I've investigated so far
  seem to look up hash tables or sets more than once for each new
  symbol.)

* Reduced number of file visits

  The symbol table implements the Windows linker semantics. We treat
  the symbol table as a bucket of all known symbols, including symbols
  in archive file headers. We put all symbols into one bucket as we
  visit new files. That means we visit each file only once.

  This is different from the Unix linker semantics, in which we only
  keep undefined symbols and visit each file one by one until we
  resolve all undefined symbols. In the Unix model, we have to visit
  archive files many times if there are circular dependencies between
  archives.

* Avoid creating additional objects and copying data

  The data structures described in the previous section are all thin
  wrappers for classes that LLVM libObject provides. We avoid copying
  data from libObject's objects to our objects. We read much less data
  than before. For example, we don't read symbol's value until we
  apply relocations because symbol offsets in sections are not
  relevant to symbol resolution. Again, COMDAT symbols may be
  discarded during symbol resolution, so reading their attributes too
  early could result in a waste. We use underlying objects directly
  where doing so makes sense.

Parallelism
-----------

The abovementioned data structures are also chosen with
multi-threading in mind. It should be relatively easy to make the
symbol table a concurrent hash map, so that multiple threads read
files and put their symbols into the symbol table concurrently. Symbol
resolution in this design is a single pointer mutation, so we can make
it atomic using compare-and-swap.

It should also be easy to apply relocations and write chunks
concurrently.
