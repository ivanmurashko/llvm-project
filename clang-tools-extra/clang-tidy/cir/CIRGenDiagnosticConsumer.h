//===--- CIRGenDiagnosticConsumer.h - clang-tidy -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_CIR_CIRGENDIAGNOSTICCONSUMER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_CIR_CIRGENDIAGNOSTICCONSUMER_H

#include "clang/Basic/Diagnostic.h"
#include "llvm/ADT/SmallString.h"
#include <memory>

namespace clang::tidy::cir {

/// Preserve the AST engine's per-location diagnostic state while making custom
/// CIRGen diagnostic IDs readable by clang-tidy's separate diagnostic engine.
class ScopedCIRGenDiagnosticConsumer : public DiagnosticConsumer {
public:
  ScopedCIRGenDiagnosticConsumer(DiagnosticsEngine &SourceDiags,
                                 DiagnosticsEngine &TidyDiags)
      : SourceDiags(SourceDiags), TidyDiags(TidyDiags),
        OriginalClient(SourceDiags.getClient()),
        OwnedClient(SourceDiags.takeClient()) {
    SourceDiags.setClient(this, /*ShouldOwnClient=*/false);
  }

  ~ScopedCIRGenDiagnosticConsumer() override {
    bool OwnsClient = bool(OwnedClient);
    if (OwnsClient)
      OwnedClient.release();
    SourceDiags.setClient(OriginalClient, OwnsClient);
  }

  bool IncludeInDiagnosticCounts() const override {
    return OriginalClient->IncludeInDiagnosticCounts();
  }

  void HandleDiagnostic(DiagnosticsEngine::Level Level,
                        const Diagnostic &Info) override {
    if (Info.getID() < diag::DIAG_UPPER_LIMIT ||
        SourceDiags.getDiagnosticIDs() == TidyDiags.getDiagnosticIDs()) {
      OriginalClient->HandleDiagnostic(Level, Info);
      return;
    }

    llvm::SmallString<128> Message;
    Info.FormatDiagnostic(Message);
    unsigned ID = TidyDiags.getDiagnosticIDs()->getCustomDiagID(
        static_cast<DiagnosticIDs::Level>(Level), "%0");
    DiagnosticStorage Storage;
    Storage.NumDiagArgs = 1;
    Storage.DiagArgumentsKind[0] = DiagnosticsEngine::ak_std_string;
    Storage.DiagArgumentsStr[0] = Message.str().str();
    Storage.DiagRanges.append(Info.getRanges().begin(), Info.getRanges().end());
    Storage.FixItHints.append(Info.getFixItHints().begin(),
                              Info.getFixItHints().end());
    Diagnostic Forwarded(&TidyDiags, Info.getLocation(), ID, Storage, Message);
    // The source engine already determined severity and updated its error
    // state. Forward directly rather than applying another engine's mappings
    // or carrying an error state across translation units.
    OriginalClient->HandleDiagnostic(Level, Forwarded);
  }

private:
  DiagnosticsEngine &SourceDiags;
  DiagnosticsEngine &TidyDiags;
  DiagnosticConsumer *OriginalClient;
  std::unique_ptr<DiagnosticConsumer> OwnedClient;
};

} // namespace clang::tidy::cir

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_CIR_CIRGENDIAGNOSTICCONSUMER_H
