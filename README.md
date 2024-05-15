# Tree-sitter grammar for the Puppet DSL

[![License](https://img.shields.io/github/license/smoeding/tree-sitter-puppet.svg)](https://raw.githubusercontent.com/smoeding/tree-sitter-puppet/master/LICENSE)
[![Build Status](https://github.com/smoeding/tree-sitter-puppet/actions/workflows/ci.yaml/badge.svg)](https://github.com/smoeding/tree-sitter-puppet/actions/workflows/ci.yaml)

A Tree-sitter grammar for the [Puppet](https://www.puppet.com) language.

This grammar and the corresponding scanner have been developed following the structure of the original RACC grammar used in the Puppet-Agent (see `lib/puppet/pops/parser/egrammar.ra` in the Puppet source code). Therefore the grammar should cover all language features that Puppet can parse.

The grammar has been tested using almost 100000 lines of code in more than 1500 files of real world Puppet code. This includes Puppet modules written by myself and popular modules by Puppetlabs and Vox Pupuli (e.g. `apache`, `apt`, `docker`, `firewall`, `nginx`, `ntp`, `php`, `postgresql`, `stdlib`, ...).

This parser is used to support my own [Emacs major mode with tree-sitter support for Puppet manifests](https://github.com/smoeding/puppet-ts-mode).
