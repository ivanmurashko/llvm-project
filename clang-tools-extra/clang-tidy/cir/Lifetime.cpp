//===--- Lifetime.cpp - clang-tidy ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Lifetime.h"
#include "../utils/OptionsUtils.h"
#include "CIRGenDiagnosticConsumer.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Pass/PassManager.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclGroup.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/CIR/CIRGenerator.h"
#include "clang/CIR/Dialect/Passes.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/ErrorHandling.h"
#include <optional>

using namespace clang::ast_matchers;

namespace clang::tidy::cir {
namespace {

/// Keep diagnostics in clang-tidy's diagnostic stream, including attached notes
/// and remarks, so filtering and YAML export retain the check's name.
class CIRTidyDiagnosticHandler : public mlir::ScopedDiagnosticHandler {
public:
  CIRTidyDiagnosticHandler(mlir::MLIRContext &Context, ClangTidyCheck &Check,
                           SourceManager &SourceMgr)
      : mlir::ScopedDiagnosticHandler(&Context), Check(Check),
        SourceMgr(SourceMgr) {
    setHandler([this](mlir::Diagnostic &D) {
      emit(D);
      for (mlir::Diagnostic &Note : D.getNotes())
        emit(Note);
      return mlir::success();
    });
  }

private:
  // Match CIRDiagnosticHandler's handling of common location wrappers. Missing
  // or untranslatable locations must not turn analysis diagnostics into
  // crashes.
  SourceLocation translateLoc(mlir::Location Loc) {
    if (auto File = mlir::dyn_cast<mlir::FileLineColLoc>(Loc)) {
      if (File.getLine() == 0 || File.getColumn() == 0)
        return {};
      auto FileRef = SourceMgr.getFileManager().getOptionalFileRef(
          File.getFilename().getValue());
      if (!FileRef)
        return {};
      return SourceMgr.translateFileLineCol(&FileRef->getFileEntry(),
                                            File.getLine(), File.getColumn());
    }
    if (auto Fused = mlir::dyn_cast<mlir::FusedLoc>(Loc)) {
      for (mlir::Location Child : Fused.getLocations()) {
        SourceLocation Translated = translateLoc(Child);
        if (Translated.isValid())
          return Translated;
      }
      return {};
    }
    if (auto CallSite = mlir::dyn_cast<mlir::CallSiteLoc>(Loc))
      return translateLoc(CallSite.getCallee());
    if (auto Named = mlir::dyn_cast<mlir::NameLoc>(Loc))
      return translateLoc(Named.getChildLoc());
    return {};
  }

  static std::optional<mlir::FileLineColLoc>
  findFileLineColLoc(mlir::Location Loc) {
    if (auto File = mlir::dyn_cast<mlir::FileLineColLoc>(Loc)) {
      if (File.getLine() != 0 && File.getColumn() != 0)
        return File;
      return std::nullopt;
    }
    if (auto Fused = mlir::dyn_cast<mlir::FusedLoc>(Loc)) {
      for (mlir::Location Child : Fused.getLocations())
        if (auto File = findFileLineColLoc(Child))
          return File;
      return std::nullopt;
    }
    if (auto CallSite = mlir::dyn_cast<mlir::CallSiteLoc>(Loc))
      return findFileLineColLoc(CallSite.getCallee());
    if (auto Named = mlir::dyn_cast<mlir::NameLoc>(Loc))
      return findFileLineColLoc(Named.getChildLoc());
    return std::nullopt;
  }

  static DiagnosticIDs::Level
  translateSeverity(mlir::DiagnosticSeverity Level) {
    switch (Level) {
    case mlir::DiagnosticSeverity::Error:
      return DiagnosticIDs::Error;
    case mlir::DiagnosticSeverity::Warning:
      return DiagnosticIDs::Warning;
    case mlir::DiagnosticSeverity::Remark:
      return DiagnosticIDs::Remark;
    case mlir::DiagnosticSeverity::Note:
      return DiagnosticIDs::Note;
    }
    llvm_unreachable("unknown MLIR diagnostic severity");
  }

  void emit(mlir::Diagnostic &D) {
    SourceLocation Loc = translateLoc(D.getLocation());
    DiagnosticIDs::Level Level = translateSeverity(D.getSeverity());
    std::string Message = D.str();
    if (Loc.isValid()) {
      Check.diag(Loc, "%0", Level) << Message;
      return;
    }
    // Preserve a usable textual location even if the SourceManager has no file
    // for it (for example, a virtual filename introduced with #line).
    if (auto File = findFileLineColLoc(D.getLocation()))
      Message = (llvm::Twine(File->getFilename().getValue()) + ":" +
                 llvm::Twine(File->getLine()) + ":" +
                 llvm::Twine(File->getColumn()) + ": " + Message)
                    .str();
    Check.diag("%0", Level) << Message;
  }

  ClangTidyCheck &Check;
  SourceManager &SourceMgr;
};

} // namespace

Lifetime::Lifetime(StringRef Name, ClangTidyContext *Context)
    : ClangTidyCheck(Name, Context), TidyContext(*Context),
      CodeGenOpts(Context->getCodeGenOpts()),
      RemarksList(
          utils::options::parseStringList(Options.get("RemarksList", ""))),
      HistoryList(
          utils::options::parseStringList(Options.get("HistoryList", "all"))),
      HistLimit(Options.get("HistLimit", 1U)) {}

void Lifetime::storeOptions(ClangTidyOptions::OptionMap &Opts) {
  Options.store(Opts, "RemarksList",
                utils::options::serializeStringList(RemarksList));
  Options.store(Opts, "HistoryList",
                utils::options::serializeStringList(HistoryList));
  Options.store(Opts, "HistLimit", HistLimit);
}

void Lifetime::registerMatchers(MatchFinder *Finder) {
  Finder->addMatcher(translationUnitDecl(), this);
}

void Lifetime::check(const MatchFinder::MatchResult &Result) {
  ASTContext &AST = *Result.Context;
  if (AST.getDiagnostics().hasErrorOccurred())
    return;

  SourceManager &SourceMgr = AST.getSourceManager();
  ScopedCIRGenDiagnosticConsumer CodeGenDiags(
      AST.getDiagnostics(), TidyContext.getDiagnosticsEngine());
  ::cir::CIRGenerator Generator(
      AST.getDiagnostics(),
      SourceMgr.getFileManager().getVirtualFileSystemPtr(), CodeGenOpts,
      /*preserveLifetimeScopes=*/true);
  Generator.Initialize(AST);
  if (AST.getDiagnostics().hasErrorOccurred())
    return;
  // User-facing diagnostics should not serialize entire CIR operations.
  Generator.getMLIRContext().printOpOnDiagnostic(false);
  CIRTidyDiagnosticHandler Handler(Generator.getMLIRContext(), *this,
                                   SourceMgr);

  // CIRGenerator consumes top-level declarations, not a TranslationUnitDecl.
  // Reuse the AST already parsed by clang-tidy and the active compilation's
  // options, including normal CIR deferred-emission behavior.
  for (Decl *D : AST.getTranslationUnitDecl()->decls()) {
    if (!Generator.HandleTopLevelDecl(DeclGroupRef(D))) {
      diag("CIR generation failed; lifetime analysis did not complete",
           DiagnosticIDs::Error);
      return;
    }
    if (AST.getDiagnostics().hasErrorOccurred())
      return;
  }
  Generator.HandleTranslationUnit(AST);
  if (AST.getDiagnostics().hasErrorOccurred())
    return;
  if (!Generator.verifyModule()) {
    diag("CIR verification failed; lifetime analysis did not complete",
         DiagnosticIDs::Error);
    return;
  }

  mlir::PassManager PM(&Generator.getMLIRContext());
  PM.enableVerifier(true);
  PM.addPass(mlir::createCIRCanonicalizePass());
  PM.addPass(
      mlir::createLifetimeCheckPass(RemarksList, HistoryList, HistLimit));
  if (mlir::failed(PM.run(Generator.getModule())))
    diag("CIR lifetime analysis failed", DiagnosticIDs::Error);
}

} // namespace clang::tidy::cir
