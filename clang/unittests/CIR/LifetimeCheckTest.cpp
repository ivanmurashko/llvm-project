//===- LifetimeCheckTest.cpp - CIR lifetime analysis tests
//------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/PassManager.h"
#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Dialect/Passes.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/Threading.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

#if LLVM_ENABLE_THREADS
// Observe real work submitted to the context's pool. The parser runs before
// attaching this pool, and pass-manager verification is disabled, so verifier
// work cannot make the lifetime-parallelism assertion pass accidentally.
class ObservedThreadPool final : public llvm::ThreadPoolInterface {
public:
  ObservedThreadPool() : pool(llvm::hardware_concurrency(2)) {}

  void wait() override { pool.wait(); }
  void wait(llvm::ThreadPoolTaskGroup &group) override { pool.wait(group); }
  unsigned getMaxConcurrency() const override { return 2; }

  std::atomic<unsigned> submissions{0};
  std::set<std::thread::id> workerThreads;

private:
  void asyncEnqueue(llvm::unique_function<void()> task,
                    llvm::ThreadPoolTaskGroup *group) override {
    ++submissions;
    auto observed = [this, task = std::move(task)]() mutable {
      {
        std::lock_guard<std::mutex> guard(mutex);
        workerThreads.insert(std::this_thread::get_id());
      }
      task();
    };
    if (group)
      pool.async(*group, std::move(observed));
    else
      pool.async(std::move(observed));
  }

  llvm::StdThreadPool pool;
  std::mutex mutex;
};

struct AnalysisResult {
  bool succeeded = false;
  unsigned submissions = 0;
  std::set<std::thread::id> workerThreads;
  std::vector<std::string> diagnostics;
  bool diagnosticsOnCaller = true;
};

static std::string makeModule() {
  std::string text;
  llvm::raw_string_ostream os(text);
  os << "!i = !cir.int<s, 32>\n!p = !cir.ptr<!i>\n"
        "!pp = !cir.ptr<!p>\nmodule {\n";
  for (unsigned i = 0; i != 8; ++i) {
    os << "cir.func @f" << i << "() {\n"
       << "  %p = cir.alloca \"p" << i << "\" align(8) : !pp\n"
       << "  %null = cir.const #cir.ptr<null> : !p\n"
       << "  cir.store %null, %p : !p, !pp\n"
       << "  %use = cir.load deref %p : !pp, !p\n"
       << "  cir.return\n}\n";
  }
  os << "}\n";
  return text;
}

static AnalysisResult runAnalysis(bool threading) {
  ObservedThreadPool pool;
  mlir::MLIRContext context(mlir::MLIRContext::Threading::DISABLED);
  context.loadDialect<cir::CIRDialect>();
  auto module = mlir::parseSourceString<mlir::ModuleOp>(makeModule(), &context);
  EXPECT_TRUE(module);
  if (!module)
    return {};

  context.setThreadPool(pool);
  context.disableMultithreading(!threading);
  AnalysisResult result;
  std::mutex diagnosticMutex;
  const auto caller = std::this_thread::get_id();
  mlir::ScopedDiagnosticHandler handler(&context, [&](mlir::Diagnostic &diag) {
    std::lock_guard<std::mutex> guard(diagnosticMutex);
    result.diagnosticsOnCaller &= std::this_thread::get_id() == caller;
    std::string text;
    llvm::raw_string_ostream os(text);
    os << diag.getLocation() << ": " << diag.str();
    for (mlir::Diagnostic &note : diag.getNotes())
      os << "\n" << note.getLocation() << ": " << note.str();
    result.diagnostics.push_back(std::move(text));
  });
  mlir::PassManager pm(&context);
  pm.enableVerifier(false);
  pm.addPass(mlir::createLifetimeCheckPass({}, {"all"}, 1));
  result.succeeded = mlir::succeeded(pm.run(*module));
  pool.wait();
  result.submissions = pool.submissions.load();
  result.workerThreads = pool.workerThreads;
  return result;
}

TEST(CIRLifetimeCheckTest, UsesContextPoolForFunctionAnalysis) {
  auto result = runAnalysis(true);
  ASSERT_TRUE(result.succeeded);
  EXPECT_GE(result.submissions, 2u);
  EXPECT_FALSE(result.workerThreads.empty());
  EXPECT_EQ(result.workerThreads.count(std::this_thread::get_id()), 0u);
  EXPECT_TRUE(result.diagnosticsOnCaller);
  ASSERT_EQ(result.diagnostics.size(), 8u);
  for (unsigned i = 0; i != 8; ++i)
    EXPECT_NE(result.diagnostics[i].find("use of invalid pointer 'p" +
                                         std::to_string(i) + "'"),
              std::string::npos);
}

TEST(CIRLifetimeCheckTest, DisabledThreadingPreservesDiagnostics) {
  auto serial = runAnalysis(false);
  auto parallel = runAnalysis(true);
  ASSERT_TRUE(serial.succeeded);
  ASSERT_TRUE(parallel.succeeded);
  EXPECT_EQ(serial.submissions, 0u);
  EXPECT_TRUE(serial.workerThreads.empty());
  EXPECT_TRUE(serial.diagnosticsOnCaller);
  EXPECT_TRUE(parallel.diagnosticsOnCaller);
  EXPECT_EQ(serial.diagnostics, parallel.diagnostics);
}
#endif // LLVM_ENABLE_THREADS

} // namespace
