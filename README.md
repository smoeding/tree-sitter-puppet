# Tree-Sitter for Puppet

A Tree-Sitter grammar for the [Puppet](https://www.puppet.com) language.

This grammar and the corresponding scanner have been developed following the structure of the original RACC grammar used in the Puppet-Agent (see the file `lib/puppet/pops/parser/egrammar.ra` in the Puppet source code). Therefore the grammar should cover all language features that Puppet can parse.

The grammar has been tested using almost 100000 lines of code in more than 1500 files of real world Puppet code. This includes Puppet modules written by myself and popular modules by Puppetlabs and Vox Pupuli (e.g. stdlib, apache, docker, firewall, nginx, php, postgresql, ...).
