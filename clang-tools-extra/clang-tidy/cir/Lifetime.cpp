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
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclFriend.h"
#include "clang/AST/DeclGroup.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/CIR/CIRGenerator.h"
#include "clang/CIR/Dialect/Passes.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ThreadPool.h"
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

// Root selection only visits declarations, never statement bodies or templates.
static bool isOrdinaryRecord(const CXXRecordDecl *Record) {
  return !Record->isImplicit() && !Record->isLambda() &&
         !Record->isLocalClass() && !Record->isDependentContext() &&
         !Record->getDescribedClassTemplate() &&
         Record->getTemplateSpecializationKind() == TSK_Undeclared;
}

static bool isUnusedMainFileRoot(const FunctionDecl *Definition,
                                 const SourceManager &SourceMgr) {
  if (!Definition || !Definition->doesThisDeclarationHaveABody() ||
      !Definition->isInlined() || Definition->isImplicit() ||
      Definition->isDefaulted() || Definition->isDeleted() ||
      Definition->isConsteval() ||
      Definition->getTemplatedKind() != FunctionDecl::TK_NonTemplate ||
      !SourceMgr.isWrittenInMainFile(
          SourceMgr.getExpansionLoc(Definition->getLocation())))
    return false;
  if (const auto *Method = dyn_cast<CXXMethodDecl>(Definition))
    if (isa<CXXConstructorDecl, CXXDestructorDecl>(Method) ||
        Method->isVirtual())
      return false;
  for (const DeclContext *Context = Definition->getDeclContext(); Context;
       Context = Context->getParent()) {
    if (Context->isFunctionOrMethod() || Context->isDependentContext())
      return false;
    if (const auto *Record = dyn_cast<CXXRecordDecl>(Context))
      if (!isOrdinaryRecord(Record))
        return false;
  }
  return !Definition->hasAttr<CUDAHostAttr>() &&
         !Definition->hasAttr<CUDADeviceAttr>() &&
         !Definition->hasAttr<CUDAGlobalAttr>() &&
         !Definition->hasAttr<DeviceKernelAttr>() &&
         !Definition->hasAttr<SYCLKernelAttr>() &&
         !Definition->hasAttr<SYCLExternalAttr>() &&
         !Definition->hasAttr<SYCLKernelEntryPointAttr>() &&
         !OMPDeclareTargetDeclAttr::isDeclareTargetDeclaration(Definition) &&
         !Definition->hasAttr<OpenACCRoutineDeclAttr>();
}

static void
collectUnusedMainFileRoots(DeclContext *Context, const SourceManager &SourceMgr,
                           llvm::SmallPtrSetImpl<const FunctionDecl *> &Seen,
                           llvm::SmallVectorImpl<FunctionDecl *> &Roots) {
  auto AddFunction = [&](FunctionDecl *Function) {
    FunctionDecl *Definition = Function->getDefinition();
    if (isUnusedMainFileRoot(Definition, SourceMgr) &&
        Seen.insert(Definition->getCanonicalDecl()).second)
      Roots.push_back(Definition);
  };
  for (Decl *D : Context->decls()) {
    if (auto *Function = dyn_cast<FunctionDecl>(D)) {
      AddFunction(Function);
    } else if (auto *Friend = dyn_cast<FriendDecl>(D)) {
      if (auto *Function =
              dyn_cast_or_null<FunctionDecl>(Friend->getFriendDecl()))
        AddFunction(Function);
    } else if (isa<NamespaceDecl, LinkageSpecDecl, ExportDecl>(D)) {
      collectUnusedMainFileRoots(cast<DeclContext>(D), SourceMgr, Seen, Roots);
    } else if (auto *Record = dyn_cast<CXXRecordDecl>(D)) {
      if (isOrdinaryRecord(Record))
        collectUnusedMainFileRoots(Record, SourceMgr, Seen, Roots);
    }
  }
}

} // namespace

Lifetime::Lifetime(StringRef Name, ClangTidyContext *Context)
    : ClangTidyCheck(Name, Context), TidyContext(*Context),
      CodeGenOpts(Context->getCodeGenOpts()),
      RemarksList(
          utils::options::parseStringList(Options.get("RemarksList", ""))),
      HistoryList(
          utils::options::parseStringList(Options.get("HistoryList", "all"))),
      HistLimit(Options.get("HistLimit", 1U)),
      NumThreads(Options.get("NumThreads", 0U)),
      AnalyzeUnusedMainFileFunctions(
          Options.get("AnalyzeUnusedMainFileFunctions", false)) {}

void Lifetime::storeOptions(ClangTidyOptions::OptionMap &Opts) {
  Options.store(Opts, "RemarksList",
                utils::options::serializeStringList(RemarksList));
  Options.store(Opts, "HistoryList",
                utils::options::serializeStringList(HistoryList));
  Options.store(Opts, "HistLimit", HistLimit);
  Options.store(Opts, "NumThreads", NumThreads);
  Options.store(Opts, "AnalyzeUnusedMainFileFunctions",
                AnalyzeUnusedMainFileFunctions);
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
  // The supplied pool must outlive Generator's MLIRContext.
  std::optional<llvm::DefaultThreadPool> Pool;
  ::cir::CIRGenerator Generator(
      AST.getDiagnostics(),
      SourceMgr.getFileManager().getVirtualFileSystemPtr(), CodeGenOpts,
      /*preserveLifetimeScopes=*/true);
  Generator.Initialize(AST);
  auto &Context = Generator.getMLIRContext();
  if (NumThreads == 1) {
    Context.disableMultithreading();
  } else if (NumThreads > 1) {
    Context.disableMultithreading();
    Pool.emplace(llvm::hardware_concurrency(NumThreads));
    Context.setThreadPool(*Pool);
  }
  if (AST.getDiagnostics().hasErrorOccurred())
    return;
  // User-facing diagnostics should not serialize entire CIR operations.
  Generator.getMLIRContext().printOpOnDiagnostic(false);
  CIRTidyDiagnosticHandler Handler(Generator.getMLIRContext(), *this,
                                   SourceMgr);

  if (AnalyzeUnusedMainFileFunctions && !AST.getLangOpts().CUDAIsDevice &&
      !AST.getLangOpts().SYCLIsDevice &&
      !AST.getLangOpts().OpenMPIsTargetDevice && !AST.getLangOpts().OpenCL &&
      !AST.getLangOpts().HLSL) {
    llvm::SmallVector<FunctionDecl *, 8> Roots;
    llvm::SmallPtrSet<const FunctionDecl *, 16> Seen;
    collectUnusedMainFileRoots(AST.getTranslationUnitDecl(), SourceMgr, Seen,
                               Roots);
    Generator.addLifetimeAnalysisRoots(Roots);
  }

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
