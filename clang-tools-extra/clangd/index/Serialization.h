//===--- Serialization.h - Binary serialization of index data ----*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides serialization of indexed symbols and other data.
//
// It writes sections:
//  - metadata such as version info
//  - a string table (which is compressed)
//  - lists of encoded symbols
//
// The format has a simple versioning scheme: the format version number is
// written in the file and non-current versions are rejected when reading.
//
// Human-readable YAML serialization is also supported, and recommended for
// debugging and experiments only.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_SERIALIZATION_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_SERIALIZATION_H

#include "Headers.h"
#include "index/Index.h"
#include "index/Symbol.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/Support/Error.h"
#include <optional>

namespace clang {
namespace clangd {

enum class IndexFileFormat {
  RIFF, // Versioned binary format, suitable for production use.
  YAML, // Human-readable format, suitable for experiments and debugging.
};

// Holds the contents of an index file that was read.
struct IndexFileIn {
  std::optional<SymbolSlab> Symbols;
  std::optional<RefSlab> Refs;
  std::optional<RelationSlab> Relations;
  // Keys are URIs of the source files.
  std::optional<IncludeGraph> Sources;
  // This contains only the Directory and CommandLine.
  std::optional<tooling::CompileCommand> Cmd;
};
// Parse an index file. The input must be a RIFF or YAML file.
llvm::Expected<IndexFileIn> readIndexFile(llvm::StringRef, SymbolOrigin);

// Specifies the contents of an index file to be written.
struct IndexFileOut {
  const SymbolSlab *Symbols = nullptr;
  const RefSlab *Refs = nullptr;
  const RelationSlab *Relations = nullptr;
  // Keys are URIs of the source files.
  const IncludeGraph *Sources = nullptr;
  // TODO: Support serializing Dex posting lists.
  IndexFileFormat Format = IndexFileFormat::RIFF;
  const tooling::CompileCommand *Cmd = nullptr;

  IndexFileOut() = default;
  IndexFileOut(const IndexFileIn &I)
      : Symbols(I.Symbols ? &*I.Symbols : nullptr),
        Refs(I.Refs ? &*I.Refs : nullptr),
        Relations(I.Relations ? &*I.Relations : nullptr),
        Sources(I.Sources ? &*I.Sources : nullptr),
        Cmd(I.Cmd ? &*I.Cmd : nullptr) {}
};
// Serializes an index file.
llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const IndexFileOut &O);

// Convert a single symbol to YAML, a nice debug representation.
std::string toYAML(const Symbol &);
std::string toYAML(const std::pair<SymbolID, ArrayRef<Ref>> &);
std::string toYAML(const Relation &);
std::string toYAML(const Ref &);

// Build an in-memory static index from an index file.
// The size should be relatively small, so data can be managed in memory.
std::unique_ptr<SymbolIndex> loadIndex(llvm::StringRef Filename,
                                       SymbolOrigin Origin, bool UseDex,
                                       bool SupportContainedRefs);

} // namespace clangd
} // namespace clang

#endif
