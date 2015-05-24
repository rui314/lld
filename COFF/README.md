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
