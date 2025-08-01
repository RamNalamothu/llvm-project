//===- TensorToSPIRVPass.cpp - Tensor to SPIR-V Passes ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements a pass to convert Tensor dialect to SPIR-V dialect.
//
//===----------------------------------------------------------------------===//

#include "mlir/Conversion/TensorToSPIRV/TensorToSPIRVPass.h"

#include "mlir/Conversion/ArithToSPIRV/ArithToSPIRV.h"
#include "mlir/Conversion/FuncToSPIRV/FuncToSPIRV.h"
#include "mlir/Conversion/TensorToSPIRV/TensorToSPIRV.h"
#include "mlir/Dialect/SPIRV/IR/SPIRVDialect.h"
#include "mlir/Dialect/SPIRV/Transforms/SPIRVConversion.h"

namespace mlir {
#define GEN_PASS_DEF_CONVERTTENSORTOSPIRVPASS
#include "mlir/Conversion/Passes.h.inc"
} // namespace mlir

using namespace mlir;

namespace {
/// A pass converting MLIR Tensor operations into the SPIR-V dialect.
class ConvertTensorToSPIRVPass
    : public impl::ConvertTensorToSPIRVPassBase<ConvertTensorToSPIRVPass> {
  using Base::Base;

  void runOnOperation() override {
    MLIRContext *context = &getContext();
    Operation *op = getOperation();

    auto targetAttr = spirv::lookupTargetEnvOrDefault(op);
    std::unique_ptr<ConversionTarget> target =
        SPIRVConversionTarget::get(targetAttr);

    SPIRVConversionOptions options;
    options.emulateLT32BitScalarTypes = this->emulateLT32BitScalarTypes;
    options.emulateUnsupportedFloatTypes = this->emulateUnsupportedFloatTypes;
    SPIRVTypeConverter typeConverter(targetAttr, options);

    RewritePatternSet patterns(context);
    arith::populateArithToSPIRVPatterns(typeConverter, patterns);
    populateFuncToSPIRVPatterns(typeConverter, patterns);
    populateTensorToSPIRVPatterns(typeConverter, /*byteCountThreshold=*/64,
                                  patterns);
    populateBuiltinFuncToSPIRVPatterns(typeConverter, patterns);

    if (failed(applyPartialConversion(op, *target, std::move(patterns))))
      return signalPassFailure();
  }
};
} // namespace
