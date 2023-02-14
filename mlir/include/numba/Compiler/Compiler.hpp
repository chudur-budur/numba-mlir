// SPDX-FileCopyrightText: 2021 - 2022 Intel Corporation
//
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>

namespace llvm {
class raw_ostream;
}

namespace mlir {
class MLIRContext;
class ModuleOp;
} // namespace mlir

namespace numba {
class PipelineRegistry;

class CompilerContext {
public:
  struct Settings {
    struct IRPrintingSettings {
      llvm::SmallVector<std::string, 1> printBefore;
      llvm::SmallVector<std::string, 1> printAfter;
      llvm::raw_ostream *out;
    };

    bool verify = false;
    bool passStatistics = false;
    bool passTimings = false;
    bool irDumpStderr = false;
    bool diagDumpStderr = false;

    std::optional<IRPrintingSettings> irPrinting;
  };

  class CompilerContextImpl;

  CompilerContext(mlir::MLIRContext &ctx, const Settings &settings,
                  const PipelineRegistry &registry);
  ~CompilerContext();

  CompilerContext(CompilerContext &&) = default;

  void run(mlir::ModuleOp module);

private:
  std::unique_ptr<CompilerContextImpl> impl;
};
} // namespace numba
