//===--- Lifetime.h - clang-tidy --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_CIR_LIFETIME_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_CIR_LIFETIME_H

#include "../ClangTidyCheck.h"
#include "clang/Basic/CodeGenOptions.h"

namespace clang::tidy::cir {

/// Diagnoses invalid pointer uses using the CIR lifetime analysis.
///
/// For the user-facing documentation see:
/// https://clang.llvm.org/extra/clang-tidy/checks/cir/lifetime-check.html
class Lifetime : public ClangTidyCheck {
public:
  Lifetime(StringRef Name, ClangTidyContext *Context);
  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
  void storeOptions(ClangTidyOptions::OptionMap &Opts) override;

private:
  ClangTidyContext &TidyContext;
  const CodeGenOptions CodeGenOpts;
  const std::vector<StringRef> RemarksList;
  const std::vector<StringRef> HistoryList;
  const unsigned HistLimit;
  const bool AnalyzeUnusedMainFileFunctions;
};

} // namespace clang::tidy::cir

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_CIR_LIFETIME_H
