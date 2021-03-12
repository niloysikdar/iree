// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//===- TileAndVectorizeInOneWorkgroup.cpp ---------------------------------===//
//
// This pass tiles and vectorizes Linalg ops on buffers within in a single
// workgroup.
//
//===----------------------------------------------------------------------===//

#include "iree/compiler/Conversion/CodegenUtils/FunctionUtils.h"
#include "iree/compiler/Conversion/CodegenUtils/MarkerUtils.h"
#include "iree/compiler/Conversion/CodegenUtils/TransformUtils.h"
#include "iree/compiler/Conversion/Common/Attributes.h"
#include "iree/compiler/Conversion/Common/Transforms.h"
#include "iree/compiler/Conversion/LinalgToSPIRV/CodeGenOptionUtils.h"
#include "iree/compiler/Conversion/LinalgToSPIRV/KernelDispatchUtils.h"
#include "iree/compiler/Conversion/LinalgToSPIRV/MemorySpace.h"
#include "iree/compiler/Conversion/LinalgToSPIRV/Passes.h"
#include "iree/compiler/Conversion/LinalgToSPIRV/Utils.h"
#include "iree/compiler/Conversion/LinalgToVector/Passes.h"
#include "iree/compiler/Dialect/HAL/IR/HALDialect.h"
#include "iree/compiler/Dialect/HAL/IR/HALOps.h"
#include "iree/compiler/Dialect/Shape/IR/ShapeDialect.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Debug.h"
#include "mlir/Dialect/GPU/GPUDialect.h"
#include "mlir/Dialect/Linalg/Analysis/DependenceAnalysis.h"
#include "mlir/Dialect/Linalg/IR/LinalgOps.h"
#include "mlir/Dialect/Linalg/Transforms/Hoisting.h"
#include "mlir/Dialect/Linalg/Transforms/Transforms.h"
#include "mlir/Dialect/Linalg/Utils/Utils.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Dialect/Vector/VectorTransforms.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Identifier.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/FoldUtils.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "mlir/Transforms/LoopUtils.h"

#define DEBUG_TYPE "iree-linalg-tile-and-fuse"

namespace mlir {
namespace iree_compiler {

//===----------------------------------------------------------------------===//
// Utility functions
//===----------------------------------------------------------------------===//

/// Returns a Linalg marker that matches any of the `matchMarkers` and replaces
/// it with `replaceMarker`.
static linalg::LinalgTransformationFilter getLinalgMatchAndReplaceMarker(
    ArrayRef<StringRef> matchMarkers, StringRef replaceMarker,
    MLIRContext *context) {
  SmallVector<Identifier, 2> markers;
  markers.reserve(matchMarkers.size());
  for (StringRef marker : matchMarkers) {
    markers.emplace_back(Identifier::get(marker, context));
  }
  return linalg::LinalgTransformationFilter(
      markers, Identifier::get(replaceMarker, context));
}

//===----------------------------------------------------------------------===//
// Main pass
//===----------------------------------------------------------------------===//

namespace {
/// Function pass that implements tiling and fusion in Linalg on buffers.
class TileAndVectorizeInOneWorkgroupPass
    : public PassWrapper<TileAndVectorizeInOneWorkgroupPass,
                         OperationPass<IREE::HAL::ExecutableTargetOp>> {
 public:
  TileAndVectorizeInOneWorkgroupPass(const SPIRVCodegenOptions &passOptions)
      : options(passOptions) {}
  TileAndVectorizeInOneWorkgroupPass(
      const TileAndVectorizeInOneWorkgroupPass &pass)
      : options(pass.options) {}

  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<AffineDialect, IREE::HAL::HALDialect, gpu::GPUDialect,
                    linalg::LinalgDialect, scf::SCFDialect, ShapeDialect,
                    vector::VectorDialect>();
  }

  void runOnOperation() override;

 private:
  SPIRVCodegenOptions options;
};
}  // namespace

//===----------------------------------------------------------------------===//
// Patterns to promote subviews to workgroup memory
//===----------------------------------------------------------------------===//

namespace {
/// Pattern to promote matmul operands to workgroup memory.
struct PromoteMatmulSubviewsPattern
    : public linalg::LinalgPromotionPattern<linalg::MatmulOp> {
  PromoteMatmulSubviewsPattern(MLIRContext *context,
                               linalg::LinalgPromotionOptions options,
                               linalg::LinalgTransformationFilter marker,
                               PatternBenefit benefit = 1)
      : linalg::LinalgPromotionPattern<linalg::MatmulOp>(
            context,
            options.setOperandsToPromote({0, 1}).setUseFullTileBuffers(
                {false, false}),
            marker, benefit) {}
};

/// Patterns to promote convolution operands to workgroup memory.
// TODO(ravishankarm): This pattern is only promoting the image subview to
// workgroup memory. In reality we should also be able to promote the filter
// subview to workgroup memory as well. Since none of the loops used to access
// the filter are tiled, this would mean the entire filter is moved to workgroup
// memory. Two reasons this is not done right now:
// 1) Linalg when tiling doesnt create a subview for the filter (since none of
//    its dimensions are tiled. This needs to be relaxed (maybe by using an
//    option).
// 2) Maybe there are better alternatives for handling filter like using
//    different storage classes, since for inference workloads these are model
//    constants. This is TBD.
template <typename ConvOpTy>
struct PromoteConvSubviewsPattern
    : public linalg::LinalgPromotionPattern<ConvOpTy> {
  PromoteConvSubviewsPattern(MLIRContext *context,
                             linalg::LinalgPromotionOptions options,
                             linalg::LinalgTransformationFilter marker,
                             PatternBenefit benefit = 1)
      : linalg::LinalgPromotionPattern<ConvOpTy>(
            context,
            options.setOperandsToPromote({0}).setUseFullTileBuffers(
                {false, false}),
            marker, benefit) {}
};
}  // namespace

static void populatePromotionPatterns(MLIRContext *context,
                                      OwningRewritePatternList &patterns) {
  patterns
      .insert<PromoteMatmulSubviewsPattern,
              PromoteConvSubviewsPattern<linalg::ConvInputNHWCFilterHWCFOp>>(
          context,
          linalg::LinalgPromotionOptions()
              .setAllocationDeallocationFns(allocateWorkgroupMemory,
                                            deallocateWorkgroupMemory)
              .setCopyInOutFns(copyToWorkgroupMemory, copyToWorkgroupMemory),
          getLinalgMatchAndReplaceMarker(getWorkgroupMarker(),
                                         getWorkgroupMemoryMarker(), context));
}

//===----------------------------------------------------------------------===//
// Patterns to tile computation to map to subgroups
//===----------------------------------------------------------------------===//

/// Computes the Value for subgroupID along each dimension given number of
/// subgroups `numSubGroups` along each dimension (x-first, y-second, z-third).
static SmallVector<linalg::ProcInfo, 2> getSubgroupIdsAndCounts(
    OpBuilder &builder, Location loc, ArrayRef<int64_t> numSubgroups) {
  Type indexType = builder.getIndexType();
  Value subgroupId = builder.create<gpu::SubgroupIdOp>(loc, indexType);
  SmallVector<linalg::ProcInfo, 2> procInfo(numSubgroups.size());

  // subgroupID
  //   = id.z * nsubgroups.y * nsubgroups.x + id.y * nsubgroups.x + id.x
  using edsc::op::operator%;
  for (size_t i = 0, e = numSubgroups.size(); i != e; ++i) {
    Value nprocs = builder.create<ConstantIndexOp>(loc, numSubgroups[i]);
    Value procId = subgroupId % nprocs;
    procInfo[e - i - 1] = linalg::ProcInfo{procId, nprocs};
    subgroupId = builder.create<SignedDivIOp>(loc, subgroupId, nprocs);
  }
  return procInfo;
}

namespace {
/// Pattern to tile linalg.matmul for subgroups.
struct TileMatmulSubgroupPattern
    : public linalg::LinalgTilingPattern<linalg::MatmulOp> {
  using Base = linalg::LinalgTilingPattern<linalg::MatmulOp>;
  TileMatmulSubgroupPattern(MLIRContext *context,
                            linalg::LinalgTilingOptions options,
                            linalg::LinalgTransformationFilter marker,
                            PatternBenefit benefit = 1)
      : Base(context, options, marker, benefit) {}
};
}  // namespace

/// Patterns for second level tiling to target subgroups.
static void populateTilingToSubgroupPatterns(
    MLIRContext *context, const LaunchConfig &launchConfig,
    OwningRewritePatternList &patterns) {
  auto getInnerTileSizeFn = [&launchConfig](
                                OpBuilder &builder,
                                Operation *operation) -> SmallVector<Value, 4> {
    ArrayRef<int64_t> tileSizes = launchConfig.getTileSizes(operation, 1);
    if (tileSizes.empty()) return {};
    SmallVector<Value, 4> tileSizesVal;
    tileSizesVal.reserve(tileSizes.size());
    for (auto val : tileSizes) {
      tileSizesVal.push_back(
          builder.create<ConstantIndexOp>(operation->getLoc(), val));
    }
    return tileSizesVal;
  };

  auto getSubgroupProcInfoFn = [&launchConfig](
                                   OpBuilder &builder, Location loc,
                                   ArrayRef<Range> parallelLoopRanges) {
    ArrayRef<int64_t> numSubgroups =
        launchConfig.getNumSubgroups().take_front(parallelLoopRanges.size());
    return getSubgroupIdsAndCounts(builder, loc, numSubgroups);
  };

  linalg::LinalgLoopDistributionOptions subgroupDistributionOptions = {
      getSubgroupProcInfoFn,
      {linalg::DistributionMethod::CyclicNumProcsEqNumIters,
       linalg::DistributionMethod::CyclicNumProcsEqNumIters}};

  patterns.insert<TileMatmulSubgroupPattern>(
      context,
      linalg::LinalgTilingOptions()
          .setLoopType(linalg::LinalgTilingLoopType::ParallelLoops)
          .setTileSizeComputationFunction(getInnerTileSizeFn)
          .setDistributionOptions(subgroupDistributionOptions),
      getLinalgMatchAndReplaceMarker(
          {getWorkgroupMemoryMarker(), getWorkgroupMarker()},
          getVectorizeMarker(), context));
}

//===----------------------------------------------------------------------===//
// Patterns and methods for thread tiling.
//===----------------------------------------------------------------------===//

/// Patterns for third level tiling to target invocations.
static void populateTilingToInvocationPatterns(
    MLIRContext *context, const LaunchConfig &launchConfig,
    OwningRewritePatternList &patterns) {
  linalg::TileSizeComputationFunction getInnerTileSizeFn =
      [&launchConfig](OpBuilder &builder, Operation *operation) {
        ArrayRef<int64_t> tileSizes = launchConfig.getTileSizes(operation, 2);
        if (tileSizes.empty()) return SmallVector<Value, 4>();
        SmallVector<Value, 4> tileSizesVal;
        tileSizesVal.reserve(tileSizes.size());
        for (auto val : tileSizes) {
          tileSizesVal.push_back(
              builder.create<ConstantIndexOp>(operation->getLoc(), val));
        }
        return tileSizesVal;
      };

  auto getThreadProcInfoFn = [](OpBuilder &builder, Location loc,
                                ArrayRef<Range> parallelLoopRanges) {
    return getGPUProcessorIdsAndCounts<gpu::ThreadIdOp, gpu::BlockDimOp>(
        builder, loc, parallelLoopRanges.size());
  };
  linalg::LinalgLoopDistributionOptions invocationDistributionOptions = {
      getThreadProcInfoFn,
      {linalg::DistributionMethod::CyclicNumProcsEqNumIters,
       linalg::DistributionMethod::CyclicNumProcsEqNumIters,
       linalg::DistributionMethod::CyclicNumProcsEqNumIters}};

  auto tilingOptions =
      linalg::LinalgTilingOptions()
          .setLoopType(linalg::LinalgTilingLoopType::ParallelLoops)
          .setTileSizeComputationFunction(getInnerTileSizeFn)
          .setDistributionOptions(invocationDistributionOptions);

  patterns.insert<linalg::LinalgTilingPattern<linalg::MatmulOp>,
                  linalg::LinalgTilingPattern<linalg::FillOp>,
                  linalg::LinalgTilingPattern<linalg::BatchMatmulOp>,
                  linalg::LinalgTilingPattern<linalg::GenericOp>>(
      context, tilingOptions,
      getLinalgMatchAndReplaceMarker(
          {getWorkgroupMemoryMarker(), getWorkgroupMarker()},
          getVectorizeMarker(), context));

  patterns.insert<
      linalg::LinalgTilingPattern<linalg::ConvInputNHWCFilterHWCFOp>,
      linalg::LinalgTilingPattern<linalg::DepthwiseConvInputNHWCFilterHWCOp>>(
      context, tilingOptions,
      getLinalgMatchAndReplaceMarker(
          {getWorkgroupMemoryMarker(), getWorkgroupMarker()},
          getConvFilterTileMarker(), context));
}

//====---------------------------------------------------------------------===//
// Patterns for vectorization
//====---------------------------------------------------------------------===//

static void populateVectorizationPatterns(MLIRContext *context,
                                          const LaunchConfig &launchConfig,
                                          OwningRewritePatternList &patterns) {
  linalg::insertVectorizationPatterns<linalg::FillOp, linalg::GenericOp,
                                      linalg::ContractionOpInterface>(
      patterns, context, linalg::LinalgVectorizationOptions(),
      linalg::LinalgTransformationFilter(
          Identifier::get(getVectorizeMarker(), context)));
}

//====---------------------------------------------------------------------===//
// Patterns for unrolling vectors
//====---------------------------------------------------------------------===//

static void populateVectorUnrollPatterns(MLIRContext *context,
                                         OwningRewritePatternList &patterns) {
  patterns.insert<vector::UnrollVectorPattern>(
      context,
      vector::UnrollVectorOptions().setNativeShapeFn(getNativeVectorSize));
}

//====---------------------------------------------------------------------===//
// Vector patterns
//====---------------------------------------------------------------------===//

static void applyVectorTransformation(FuncOp funcOp) {
  {
    OwningRewritePatternList vectorUnrollPatterns;
    populateVectorUnrollPatterns(funcOp.getContext(), vectorUnrollPatterns);
    (void)applyPatternsAndFoldGreedily(funcOp, std::move(vectorUnrollPatterns));

    OwningRewritePatternList canonicalizationPatterns1;
    vector::populateVectorToVectorCanonicalizationPatterns(
        canonicalizationPatterns1, funcOp.getContext());
    vector::populateVectorToVectorTransformationPatterns(
        canonicalizationPatterns1, funcOp.getContext());
    vector::populateSplitVectorTransferPatterns(canonicalizationPatterns1,
                                                funcOp.getContext());
    (void)applyPatternsAndFoldGreedily(funcOp,
                                       std::move(canonicalizationPatterns1));

    OwningRewritePatternList canonicalizationPatterns2;
    vector::populateVectorSlicesLoweringPatterns(canonicalizationPatterns2,
                                                 funcOp.getContext());
    (void)applyPatternsAndFoldGreedily(funcOp,
                                       std::move(canonicalizationPatterns2));
    LLVM_DEBUG({
      llvm::dbgs() << "--- After Vector Unroll ---\n";
      funcOp.print(llvm::dbgs(), OpPrintingFlags().useLocalScope());
      llvm::dbgs() << "\n\n";
    });
  }

  {
    linalg::hoistRedundantVectorTransfers(funcOp);

    LLVM_DEBUG({
      llvm::dbgs() << "--- After Hoisting ---\n";
      funcOp.print(llvm::dbgs(), OpPrintingFlags().useLocalScope());
      llvm::dbgs() << "\n\n";
    });
  }
}

//====---------------------------------------------------------------------===//
// Patterns to tile convolution window dimensions
//====---------------------------------------------------------------------===//

static void populateTilingConvFilterPatterns(
    MLIRContext *context, OwningRewritePatternList &patterns,
    const LaunchConfig &launchConfig,
    linalg::LinalgTransformationFilter marker) {
  auto getTileSizeFn = [&launchConfig](OpBuilder &builder, Operation *op) {
    SmallVector<Value, 4> tileSizes;
    ArrayRef<int64_t> fourthLevel = launchConfig.getTileSizes(op, 3);
    tileSizes.reserve(fourthLevel.size());

    Location loc = op->getLoc();
    for (int64_t size : fourthLevel) {
      tileSizes.push_back(builder.create<ConstantIndexOp>(loc, size));
    }
    return tileSizes;
  };

  auto tilingOptions = linalg::LinalgTilingOptions()
                           .setLoopType(linalg::LinalgTilingLoopType::Loops)
                           .setTileSizeComputationFunction(getTileSizeFn);

  patterns.insert<
      linalg::LinalgTilingPattern<linalg::ConvInputNHWCFilterHWCFOp>,
      linalg::LinalgTilingPattern<linalg::DepthwiseConvInputNHWCFilterHWCOp>>(
      context, tilingOptions, marker);
}

//====---------------------------------------------------------------------===//
// Main pass implementation
//====---------------------------------------------------------------------===//

void TileAndVectorizeInOneWorkgroupPass::runOnOperation() {
  MLIRContext *context = &getContext();
  IREE::HAL::ExecutableTargetOp targetOp = getOperation();
  ModuleOp module = targetOp.getInnerModule();

  for (FuncOp funcOp : module.getOps<FuncOp>()) {
    if (!isEntryPoint(funcOp)) continue;

    SmallVector<linalg::LinalgOp, 4> linalgOps;
    SmallVector<Operation *, 4> tiledLoops;

    if (failed(getLinalgOps(funcOp, linalgOps, tiledLoops))) {
      return signalPassFailure();
    }

    linalg::Aliases aliases;
    linalg::LinalgDependenceGraph dependenceGraph(aliases, linalgOps);
    Optional<LaunchConfig> launchConfigOpt =
        initGPULaunchConfig(context, dependenceGraph, options, linalgOps);
    if (!launchConfigOpt) {
      funcOp.emitError("unable to find launch configuration");
      return signalPassFailure();
    }
    LaunchConfig &launchConfig = *launchConfigOpt;

    LLVM_DEBUG({
      llvm::dbgs() << "\n--- IREE Linalg tile and fuse configuration ---\n";
      llvm::dbgs() << "@func " << funcOp.getName() << ": # workgroup sizes: [";
      interleaveComma(launchConfig.getWorkgroupSize(), llvm::dbgs());
      llvm::dbgs() << "]\n";
      for (auto op : linalgOps) {
        llvm::dbgs() << "\t" << op.getOperation()->getName() << " : ";
        TileSizesListTypeRef tileSizes = launchConfig.getTileSizes(op);
        llvm::dbgs() << "{";
        std::string sep = "";
        for (auto &level : enumerate(tileSizes)) {
          llvm::dbgs() << sep << level.index() << " : [";
          sep = ", ";
          interleaveComma(level.value(), llvm::dbgs());
          llvm::dbgs() << "]";
        }
        llvm::dbgs() << "}\n";
      }
    });

    if (options.useWorkgroupMemory) {
      // The promotion patterns are put separate from the tiling patterns to
      // make sure that the allocated scratchspace memory is constant sizes
      // which requires some folding to trigger.
      OwningRewritePatternList promotionPatterns;
      populatePromotionPatterns(context, promotionPatterns);
      (void)applyPatternsAndFoldGreedily(funcOp, std::move(promotionPatterns));
      applyCanonicalizationPatternsForTiling(context, funcOp);

      LLVM_DEBUG({
        llvm::dbgs() << "--- After Promotion  ---\n";
        funcOp.print(llvm::dbgs(), OpPrintingFlags().useLocalScope());
        llvm::dbgs() << "\n\n";
      });
    }

    if (launchConfig.useVectorize()) {
      {
        OwningRewritePatternList secondLevelTilingPatterns;
        populateTilingToSubgroupPatterns(context, launchConfig,
                                         secondLevelTilingPatterns);
        (void)applyPatternsAndFoldGreedily(
            funcOp, std::move(secondLevelTilingPatterns));
        applyCanonicalizationPatternsForTiling(context, funcOp);
        promoteSingleIterationLoops(funcOp);

        LLVM_DEBUG({
          llvm::dbgs() << "--- After Second level Tiling  ---\n";
          funcOp.print(llvm::dbgs(), OpPrintingFlags().useLocalScope());
          llvm::dbgs() << "\n\n";
        });
      }

      {
        OwningRewritePatternList thirdLevelTilingPatterns;
        populateTilingToInvocationPatterns(context, launchConfig,
                                           thirdLevelTilingPatterns);
        (void)applyPatternsAndFoldGreedily(funcOp,
                                           std::move(thirdLevelTilingPatterns));
        applyCanonicalizationPatternsForTiling(context, funcOp);
        promoteSingleIterationLoops(funcOp);

        LLVM_DEBUG({
          llvm::dbgs() << "--- After Third level Tiling  ---\n";
          funcOp.print(llvm::dbgs(), OpPrintingFlags().useLocalScope());
          llvm::dbgs() << "\n\n";
        });
      }

      {
        OwningRewritePatternList tilingPatterns;
        auto marker = getLinalgMatchAndReplaceMarker(
            getConvFilterTileMarker(), getVectorizeMarker(), context);
        populateTilingConvFilterPatterns(context, tilingPatterns, launchConfig,
                                         marker);
        populateFoldGPUProcessorIDUsesPatterns(context, tilingPatterns);
        tilingPatterns.insert<linalg::AffineMinSCFCanonicalizationPattern>(
            context);
        (void)applyPatternsAndFoldGreedily(funcOp, std::move(tilingPatterns));
        applyCanonicalizationPatternsForTiling(context, funcOp);

        LLVM_DEBUG({
          llvm::dbgs() << "--- After tiling convolution filter  ---\n";
          funcOp.print(llvm::dbgs(), OpPrintingFlags().useLocalScope());
          llvm::dbgs() << "\n\n";
        });
      }

      {
        OwningRewritePatternList vectorizationPatterns;
        populateVectorizationPatterns(context, launchConfig,
                                      vectorizationPatterns);
        populateVectorizeLinalgConvPatterns(context, vectorizationPatterns);
        (void)applyPatternsAndFoldGreedily(funcOp,
                                           std::move(vectorizationPatterns));
        LLVM_DEBUG({
          llvm::dbgs() << "--- After Vectorization ---\n";
          funcOp.print(llvm::dbgs(), OpPrintingFlags().useLocalScope());
          llvm::dbgs() << "\n\n";
        });
      }

      // TODO: This should be a folding of Add into Contract in core but while
      // they live in different dialects, it is not possible without unnatural
      // dependencies.
      funcOp.walk([&](Operation *op) {
        if (auto contract = canonicalizeContractionAdd(op))
          op->replaceAllUsesWith(contract);
      });

      applyVectorTransformation(funcOp);
    }

    // Invoke patterns to generalize linalg.depthwise_conv_2d_nhwc ops to Linalg
    // generic ops. This can handle those cases that failed tiling and
    // vectorization in the above.
    // TODO(antiagainst): remove this once we have depthwise convolution
    // vectorization applicable everywhere.
    {
      // Carry over the Linalg marker because it is load-bearing and affects
      // later passes.
      linalg::LinalgTransformationFilter marker =
          getLinalgMatchAndReplaceMarker({getWorkgroupMarker()},
                                         getWorkgroupMarker(), context);
      marker.addFilter([](Operation *op) -> LogicalResult {
        return success(isa<linalg::DepthwiseConvInputNHWCFilterHWCFOp,
                           linalg::DepthwiseConvInputNHWCFilterHWCOp>(op));
      });

      OwningRewritePatternList patterns;
      linalg::populateLinalgNamedOpsGeneralizationPatterns(context, patterns,
                                                           marker);

      (void)applyPatternsAndFoldGreedily(funcOp, std::move(patterns));

      LLVM_DEBUG({
        llvm::dbgs() << "--- After generalization ---\n";
        funcOp.print(llvm::dbgs(), OpPrintingFlags().useLocalScope());
        llvm::dbgs() << "\n\n";
      });
    }

    launchConfig.finalize(funcOp);
  }
}

//===----------------------------------------------------------------------===//
// Pass entry point and registration
//===----------------------------------------------------------------------===//

std::unique_ptr<OperationPass<IREE::HAL::ExecutableTargetOp>>
createTileAndVectorizeInOneWorkgroupPass(const SPIRVCodegenOptions &options) {
  return std::make_unique<TileAndVectorizeInOneWorkgroupPass>(options);
}

static PassRegistration<TileAndVectorizeInOneWorkgroupPass> pass(
    "iree-spirv-tile-and-vectorize-in-one-workgroup",
    "Tile and vectorize Linalg operations on buffers in one workgroup", [] {
      SPIRVCodegenOptions options = getSPIRVCodegenOptionsFromClOptions();
      return std::make_unique<TileAndVectorizeInOneWorkgroupPass>(options);
    });

}  // namespace iree_compiler
}  // namespace mlir