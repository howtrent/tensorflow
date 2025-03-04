/* Copyright 2023 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/service/gpu/ir_emitter_triton.h"

#include <array>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <system_error>  // NOLINT(build/c++11): required to interface with LLVM
#include <utility>
#include <variant>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/cord.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include "mlir/Conversion/AffineToStandard/AffineToStandard.h"  // from @llvm-project
#include "mlir/Conversion/ArithToLLVM/ArithToLLVM.h"  // from @llvm-project
#include "mlir/Conversion/ControlFlowToLLVM/ControlFlowToLLVM.h"  // from @llvm-project
#include "mlir/Conversion/IndexToLLVM/IndexToLLVM.h"  // from @llvm-project
#include "mlir/Conversion/SCFToControlFlow/SCFToControlFlow.h"  // from @llvm-project
#include "mlir/Dialect/Affine/IR/AffineOps.h"  // from @llvm-project
#include "mlir/Dialect/Arith/IR/Arith.h"  // from @llvm-project
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"  // from @llvm-project
#include "mlir/Dialect/LLVMIR/LLVMTypes.h"  // from @llvm-project
#include "mlir/Dialect/LLVMIR/NVVMDialect.h"  // from @llvm-project
#include "mlir/Dialect/Math/IR/Math.h"  // from @llvm-project
#include "mlir/Dialect/SCF/IR/SCF.h"  // from @llvm-project
#include "mlir/ExecutionEngine/OptUtils.h"  // from @llvm-project
#include "mlir/IR/AffineExpr.h"  // from @llvm-project
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/BuiltinAttributes.h"  // from @llvm-project
#include "mlir/IR/BuiltinOps.h"  // from @llvm-project
#include "mlir/IR/BuiltinTypeInterfaces.h"  // from @llvm-project
#include "mlir/IR/BuiltinTypes.h"  // from @llvm-project
#include "mlir/IR/DialectRegistry.h"  // from @llvm-project
#include "mlir/IR/ImplicitLocOpBuilder.h"  // from @llvm-project
#include "mlir/IR/Location.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/IR/OwningOpRef.h"  // from @llvm-project
#include "mlir/IR/PatternMatch.h"  // from @llvm-project
#include "mlir/IR/TypeUtilities.h"  // from @llvm-project
#include "mlir/IR/Types.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/IR/ValueRange.h"  // from @llvm-project
#include "mlir/IR/Verifier.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Pass/PassManager.h"  // from @llvm-project
#include "mlir/Support/LLVM.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "mlir/Support/TypeID.h"  // from @llvm-project
#include "mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h"  // from @llvm-project
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"  // from @llvm-project
#include "mlir/Target/LLVMIR/Dialect/NVVM/NVVMToLLVMIRTranslation.h"  // from @llvm-project
#include "mlir/Target/LLVMIR/Dialect/ROCDL/ROCDLToLLVMIRTranslation.h"  // from @llvm-project
#include "mlir/Target/LLVMIR/Export.h"  // from @llvm-project
#include "mlir/Transforms/Passes.h"  // from @llvm-project
#include "xla/autotuning.pb.h"
#include "xla/comparison_util.h"
#include "xla/debug_options_flags.h"
#include "xla/hlo/ir/hlo_casting_utils.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/hlo/utils/hlo_query.h"
#include "xla/layout_util.h"
#include "xla/literal.h"
#include "xla/mlir_hlo/mhlo/IR/hlo_ops.h"
#include "xla/mlir_hlo/mhlo/transforms/map_mhlo_to_scalar_op.h"
#include "xla/primitive_util.h"
#include "xla/service/algorithm_util.h"
#include "xla/service/dump.h"
#include "xla/service/gpu/fusions/mlir/elemental_hlo_to_mlir.h"
#include "xla/service/gpu/hlo_traversal.h"
#include "xla/service/gpu/ir_emission_utils.h"
#include "xla/service/gpu/launch_dimensions.h"
#include "xla/service/gpu/llvm_gpu_backend/gpu_backend_lib.h"
#include "xla/service/gpu/matmul_utils.h"
#include "xla/service/gpu/model/indexing_map.h"
#include "xla/service/gpu/model/symbolic_tile_analysis.h"
#include "xla/service/gpu/model/tiled_hlo_instruction.h"
#include "xla/service/gpu/target_util.h"
#include "xla/service/gpu/triton_fusion_analysis.h"
#include "xla/service/gpu/triton_tiling_propagation.h"
#include "xla/service/hlo_module_config.h"
#include "xla/service/llvm_ir/llvm_util.h"
#include "xla/shape_util.h"
#include "xla/status.h"
#include "xla/status_macros.h"
#include "xla/stream_executor/device_description.h"
#include "xla/stream_executor/launch_dim.h"
#include "xla/translate/hlo_to_mhlo/hlo_function_importer.h"
#include "xla/util.h"
#include "xla/xla_data.pb.h"
#include "tsl/platform/env.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/path.h"
#include "tsl/platform/status.h"
#include "tsl/platform/statusor.h"
#include "tsl/platform/tensor_float_32_utils.h"
#include "triton/Conversion/TritonGPUToLLVM/Passes.h"
#include "triton/Conversion/TritonToTritonGPU/TritonToTritonGPUPass.h"
#include "triton/Dialect/Triton/IR/Dialect.h"
#include "triton/Dialect/Triton/IR/Types.h"
#include "triton/Dialect/TritonGPU/IR/Dialect.h"
#include "triton/Dialect/TritonNvidiaGPU/Transforms/Passes.h"

namespace xla {
namespace gpu {

namespace ma = ::mlir::arith;
namespace mm = ::mlir::math;
namespace ml = ::mlir::LLVM;
namespace mn = ::mlir::NVVM;
namespace mt = ::mlir::triton;

using ::llvm::SmallVector;
using mlir::ArrayRef;
using mlir::ImplicitLocOpBuilder;
using ::mlir::ShapedType;
using ::mlir::Type;
using ::mlir::Value;
using mlir::ValueRange;

namespace {

// XLA -> Triton type conversions.
Type TritonType(mlir::OpBuilder b, PrimitiveType t) {
  switch (t) {
    case F64:
      return b.getF64Type();
    case F32:
      return b.getF32Type();
    case F16:
      return b.getF16Type();
    case BF16:
      return b.getBF16Type();
    case S64:
      return b.getI64Type();
    case S32:
      return b.getI32Type();
    case S16:
      return b.getI16Type();
    case PRED:
      return b.getI1Type();
    case S8:
      return b.getI8Type();
    default:
      LOG(FATAL) << "This type is not supported yet: "
                 << primitive_util::LowercasePrimitiveTypeName(t);
  }
}

Type StorageType(mlir::OpBuilder b, Type t) {
  if (t.isInteger(1)) {
    return b.getI8Type();
  }
  return t;
}

// Get the value of the scalar constant's literal in a C++ type.
template <typename T>
T ScalarConstantValue(const HloInstruction& instr, PrimitiveType dst_type) {
  CHECK(hlo_query::IsScalarConstant(&instr));
  absl::StatusOr<Literal> converted = instr.literal().Convert(dst_type);
  TF_CHECK_OK(converted.status());
  return converted.value().GetFirstElement<T>();
}

// Create a scalar constant.
template <typename T>
ma::ConstantOp CreateConst(ImplicitLocOpBuilder b, Type type, T value) {
  if (mlir::isa<mlir::IntegerType>(type)) {
    return b.create<ma::ConstantOp>(b.getIntegerAttr(type, value));
  }
  if (mlir::isa<mlir::FloatType>(type)) {
    return b.create<ma::ConstantOp>(
        b.getFloatAttr(type, static_cast<double>(value)));
  }
  LOG(FATAL) << "Constant type not supported: " << llvm_ir::DumpToString(type);
}

// Create a tensor constant.
template <typename T>
ma::ConstantOp CreateConst(ImplicitLocOpBuilder& b, Type type, T value,
                           ArrayRef<int64_t> shape) {
  auto tensor_type = mlir::RankedTensorType::get(shape, type);
  if (auto int_type = mlir::dyn_cast<mlir::IntegerType>(type)) {
    return b.create<ma::ConstantOp>(mlir::DenseElementsAttr::get(
        tensor_type, mlir::APInt(int_type.getIntOrFloatBitWidth(), value)));
  }
  if (auto float_type = mlir::dyn_cast<mlir::FloatType>(type)) {
    return b.create<ma::ConstantOp>(mlir::DenseElementsAttr::get(
        tensor_type, b.getFloatAttr(type, static_cast<double>(value))));
  }
  LOG(FATAL) << "Constant type not supported: " << llvm_ir::DumpToString(type);
}

Value ZerosLike(ImplicitLocOpBuilder& b, Value x) {
  if (auto src_shaped_ty = mlir::dyn_cast<ShapedType>(x.getType())) {
    Type src_ty = src_shaped_ty.getElementType();
    return CreateConst(b, src_ty, 0, src_shaped_ty.getShape());
  }
  return CreateConst(b, x.getType(), 0);
}

Value OnesLike(ImplicitLocOpBuilder& b, Value x) {
  if (auto src_shaped_ty = mlir::dyn_cast<ShapedType>(x.getType())) {
    Type src_ty = src_shaped_ty.getElementType();
    return CreateConst(b, src_ty, 1, src_shaped_ty.getShape());
  }
  return CreateConst(b, x.getType(), 1);
}

// Triton type conversions.
Value Cast(ImplicitLocOpBuilder& b, Value value, Type dst_element_ty) {
  Type src_ty = value.getType();
  Type src_element_ty = src_ty;
  Type fp32_ty = b.getF32Type();
  Type dst_ty = dst_element_ty;
  if (auto src_shaped_ty = mlir::dyn_cast<ShapedType>(src_ty)) {
    src_element_ty = src_shaped_ty.getElementType();
    dst_ty = src_shaped_ty.clone(src_shaped_ty.getShape(), dst_element_ty);
    fp32_ty = src_shaped_ty.clone(src_shaped_ty.getShape(), b.getF32Type());
  }
  if (src_ty == dst_ty) {
    return value;
  }

  // All operations on bf16 are done through f32.
  if (src_element_ty.isBF16()) {
    return Cast(b, b.create<ma::ExtFOp>(fp32_ty, value), dst_element_ty);
  }
  if (dst_element_ty.isBF16()) {
    // S8 -> BF16 is directly supported and doesn't need to go through f32.
    if (!src_element_ty.isInteger(8)) {
      return b.create<ma::TruncFOp>(dst_ty, Cast(b, value, b.getF32Type()));
    }
  }

  // float => float
  auto src_fp_element_ty = mlir::dyn_cast<mlir::FloatType>(src_element_ty);
  auto dst_fp_element_ty = mlir::dyn_cast<mlir::FloatType>(dst_element_ty);
  if (src_fp_element_ty && dst_fp_element_ty) {
    if (src_fp_element_ty.getFPMantissaWidth() >
        dst_fp_element_ty.getFPMantissaWidth()) {
      return b.create<ma::TruncFOp>(dst_ty, value);
    } else {
      return b.create<ma::ExtFOp>(dst_ty, value);
    }
  }
  // int => int
  if (mlir::isa<mlir::IntegerType>(src_element_ty) &&
      mlir::isa<mlir::IntegerType>(dst_element_ty)) {
    if (src_element_ty.getIntOrFloatBitWidth() <
        dst_element_ty.getIntOrFloatBitWidth()) {
      if (src_element_ty.isInteger(1)) {
        return b.create<ma::ExtUIOp>(dst_ty, value);
      }
      return b.create<ma::ExtSIOp>(dst_ty, value);
    }
    return b.create<ma::TruncIOp>(dst_ty, value);
  }
  // int => float
  if (mlir::isa<mlir::IntegerType>(src_element_ty) && dst_fp_element_ty) {
    // TODO(b/266862493): Support unsigned integer types.
    if (src_element_ty.isInteger(1)) {
      return b.create<ma::UIToFPOp>(dst_ty, value);
    }
    return b.create<ma::SIToFPOp>(dst_ty, value);
  }
  // float => int
  if (src_fp_element_ty && mlir::isa<mlir::IntegerType>(dst_element_ty)) {
    // TODO(b/266862493): Support unsigned integer types.
    if (dst_element_ty.isInteger(1)) {
      return b.create<ma::CmpFOp>(ma::CmpFPredicate::UNE, value,
                                  ZerosLike(b, value));
    }
    return b.create<ma::FPToSIOp>(dst_ty, value);
  }

  LOG(FATAL) << "Type conversion not supported: "
             << llvm_ir::DumpToString(src_element_ty) << " -> "
             << llvm_ir::DumpToString(dst_element_ty);
}

Value Subtract(ImplicitLocOpBuilder& b, ValueRange values) {
  if (mlir::isa<mlir::IntegerType>(mlir::getElementTypeOrSelf(values[0]))) {
    return b.create<ma::SubIOp>(values[0], values[1]);
  } else {
    return b.create<ma::SubFOp>(values[0], values[1]);
  }
}

Value Compare(ImplicitLocOpBuilder& b, ValueRange values,
              mlir::mhlo::ComparisonDirection direction) {
  const Type type = mlir::getElementTypeOrSelf(values[0]);
  if (mlir::isa<mlir::IntegerType>(type)) {
    return b.create<ma::CmpIOp>(
        mlir::mhlo::impl::getCmpPredicate<ma::CmpIPredicate>(
            direction,
            /*isSigned=*/!type.isInteger(1))
            .value(),
        values[0], values[1]);
  }
  return b.create<ma::CmpFOp>(
      mlir::mhlo::impl::getCmpPredicate<ma::CmpFPredicate>(direction,
                                                           /*isSigned=*/true)
          .value(),
      values[0], values[1]);
}

Value Maximum(ImplicitLocOpBuilder& b, const se::DeviceDescription& device_info,
              ValueRange values) {
  if (mlir::isa<mlir::FloatType>(mlir::getElementTypeOrSelf(values[0]))) {
    return b.create<ma::MaximumFOp>(values);
  }
  // logic: isNaN(lhs) || (!isNan(rhs) && lhs >= rhs) ? lhs : rhs
  // See also: IEEE Std 754-2008 5.11.
  //
  // This also works, but we wanted to make it similar to minimum.
  // logic: isNaN(lhs) || lhs >= rhs ? lhs : rhs
  Value lhs_is_nan =
      Compare(b, {values[0], values[0]}, mlir::mhlo::ComparisonDirection::NE);
  Value rhs_is_not_nan =
      Compare(b, {values[1], values[1]}, mlir::mhlo::ComparisonDirection::EQ);
  Value lhs_is_ge = Compare(b, values, mlir::mhlo::ComparisonDirection::GE);
  return b.create<ma::SelectOp>(
      b.create<ma::OrIOp>(lhs_is_nan,
                          b.create<ma::AndIOp>(rhs_is_not_nan, lhs_is_ge)),
      values[0], values[1]);
}

Value Minimum(ImplicitLocOpBuilder& b, const se::DeviceDescription& device_info,
              ValueRange values) {
  if (mlir::isa<mlir::FloatType>(mlir::getElementTypeOrSelf(values[0]))) {
    return b.create<ma::MinimumFOp>(values);
  }
  // logic: isNaN(lhs) || (!isNan(rhs) && lhs <= rhs) ? lhs : rhs
  // See also: IEEE Std 754-2008 5.11.
  //
  // This should also work, but the tests show that it doesn't work for
  // minimum(x, NaN):
  // logic: isNaN(lhs) || lhs <= rhs ? lhs : rhs
  Value lhs_is_nan =
      Compare(b, {values[0], values[0]}, mlir::mhlo::ComparisonDirection::NE);
  Value rhs_is_not_nan =
      Compare(b, {values[1], values[1]}, mlir::mhlo::ComparisonDirection::EQ);
  Value lhs_is_le = Compare(b, values, mlir::mhlo::ComparisonDirection::LE);
  return b.create<ma::SelectOp>(
      b.create<ma::OrIOp>(lhs_is_nan,
                          b.create<ma::AndIOp>(rhs_is_not_nan, lhs_is_le)),
      values[0], values[1]);
}

// TODO(b/269489810): Contribute nicer builders to Triton, so we don't need to
// define these utilities.
Value Splat(ImplicitLocOpBuilder& b, Value value, ArrayRef<int64_t> shape) {
  auto type = mlir::RankedTensorType::get(shape, value.getType());
  return b.create<mt::SplatOp>(type, value);
}

using TensorValue = mlir::TypedValue<mlir::RankedTensorType>;

Value Broadcast(ImplicitLocOpBuilder& b, TensorValue value,
                ArrayRef<int64_t> shape) {
  return b.create<mt::BroadcastOp>(value.getType().clone(shape), value);
}

Value Range(ImplicitLocOpBuilder& b, int32_t limit) {
  auto type = mlir::RankedTensorType::get(limit, b.getI32Type());
  return b.create<mt::MakeRangeOp>(type, 0, limit);
}

Value AddPtr(ImplicitLocOpBuilder& b, Value ptr, Value offset) {
  return b.create<mt::AddPtrOp>(ptr.getType(), ptr, offset);
}

absl::StatusOr<Value> EmitElementwise(ImplicitLocOpBuilder& b,
                                      absl::string_view libdevice_path,
                                      const se::DeviceDescription& device_info,
                                      const HloInstruction& hlo,
                                      ValueRange inputs) {
  if (mlir::getElementTypeOrSelf(inputs[0]).isF32() ||
      mlir::getElementTypeOrSelf(inputs[0]).isF64()) {
    auto dev_fn_id = GetTargetDeviceFunctionID(hlo.opcode());
    if (dev_fn_id.ok()) {
      llvm::Triple triple("nvptx64-unknown-unknown");
      if (std::holds_alternative<se::RocmComputeCapability>(
              device_info.gpu_compute_capability())) {
        triple.setTriple("amdgcn-unknown-unknown");
      }
      return b.create<mt::ExternElementwiseOp>(
          inputs[0].getType(), inputs, "libdevice", libdevice_path,
          ObtainDeviceFunctionName(dev_fn_id.value(),
                                   hlo.shape().element_type(), triple),
          /*pure=*/true);
    }
  }
  const bool is_integer =
      mlir::isa<mlir::IntegerType>(mlir::getElementTypeOrSelf(inputs[0]));

  switch (hlo.opcode()) {
    case HloOpcode::kCopy:
      // Dimension transformations are taken care of separately.
      return inputs[0];
    case HloOpcode::kAbs:
      if (is_integer) {
        return b.create<mm::AbsIOp>(inputs[0]);
      }
      return b.create<mm::AbsFOp>(inputs[0]);
    case HloOpcode::kNot:
      return b.create<ma::XOrIOp>(inputs[0], OnesLike(b, inputs[0]));
    case HloOpcode::kNegate:
      // NegFOp is not supported by Triton.
      return Subtract(b, {ZerosLike(b, inputs[0]), inputs[0]});
    case HloOpcode::kConvert:
      return Cast(b, inputs[0], TritonType(b, hlo.shape().element_type()));
    case HloOpcode::kAdd:
      if (is_integer) {
        return b.create<ma::AddIOp>(inputs[0], inputs[1]);
      }
      return b.create<ma::AddFOp>(inputs[0], inputs[1]);
    case HloOpcode::kSubtract:
      return Subtract(b, inputs);
    case HloOpcode::kMultiply:
      if (is_integer) {
        return b.create<ma::MulIOp>(inputs[0], inputs[1]);
      }
      return b.create<ma::MulFOp>(inputs[0], inputs[1]);
    case HloOpcode::kMaximum:
      return Maximum(b, device_info, inputs);
    case HloOpcode::kMinimum:
      return Minimum(b, device_info, inputs);
    case HloOpcode::kAnd:
      return b.create<ma::AndIOp>(inputs[0], inputs[1]);
    case HloOpcode::kOr:
      return b.create<ma::OrIOp>(inputs[0], inputs[1]);
    case HloOpcode::kXor:
      return b.create<ma::XOrIOp>(inputs[0], inputs[1]);
    case HloOpcode::kDivide:
      if (is_integer) {
        // Unsigned not supported yet.
        return b.create<ma::DivSIOp>(inputs[0], inputs[1]);
      }
      return b.create<ma::DivFOp>(inputs[0], inputs[1]);
    case HloOpcode::kCompare:
      return Compare(
          b, inputs,
          mlir::mhlo::symbolizeComparisonDirection(
              ComparisonDirectionToString(hlo.comparison_direction()))
              .value());
    case HloOpcode::kSelect:
      return b.create<ma::SelectOp>(
          Compare(b, {inputs[0], ZerosLike(b, inputs[0])},
                  mlir::mhlo::ComparisonDirection::NE),
          inputs[1], inputs[2]);
    default:
      return absl::InvalidArgumentError(
          absl::StrCat("Unsupported elementwise operation ", hlo.ToString()));
  }
}

Value EmitParameterLoad(ImplicitLocOpBuilder& b, Value pointer,
                        ArrayRef<int32_t> boundary_checks) {
  if (mt::isTensorPointerType(pointer.getType())) {
    std::optional<mt::PaddingOption> padding;
    if (!boundary_checks.empty()) {
      padding = mt::PaddingOption::PAD_ZERO;
    }
    return b.create<mt::LoadOp>(pointer, boundary_checks, padding,
                                mt::CacheModifier::NONE,
                                mt::EvictionPolicy::NORMAL,
                                /*isVolatile=*/false);
  }
  return Splat(b,
               b.create<mt::LoadOp>(pointer, mt::CacheModifier::NONE,
                                    mt::EvictionPolicy::NORMAL,
                                    /*isVolatile=*/false),
               {});
}

Value EmitConstant(ImplicitLocOpBuilder& b, const HloInstruction& constant) {
  Type ty = TritonType(b, constant.shape().element_type());
  if (constant.shape().IsInteger()) {
    if (constant.shape().element_type() == U64) {
      return CreateConst(b, ty, ScalarConstantValue<uint64_t>(constant, U64));
    } else {
      return CreateConst(b, ty, ScalarConstantValue<int64_t>(constant, S64));
    }
  }
  return CreateConst(b, ty, ScalarConstantValue<double>(constant, F64));
}

// Grouped properties of tiled dimensions used to generate block pointers.
struct DimProperties {
  DimProperties(int64_t index, Value pid, int block_size, int split_value)
      : index(index),
        pid(pid),
        block_size(block_size),
        split_value(split_value) {}

  // Logical index of the dimension at the tiling-defining operation.
  int64_t index;
  // Block program ID corresponding to this dimension.
  Value pid;
  // Elements of the dimension to process per block program.
  int block_size;
  // Size of the major part of the dimension if it's split into two parts.
  int split_value;
};

absl::StatusOr<Value> EmitBroadcast(
    ImplicitLocOpBuilder& b, const TritonFusionAnalysis* analysis,
    TritonFusionAnalysis::Scope scope,
    absl::Span<const DimProperties> tiled_dimensions,
    const HloInstruction& broadcast, Value input) {
  TF_RET_CHECK(analysis != nullptr);
  std::vector<int64_t> out_shape;
  for (const DimProperties& dim : tiled_dimensions) {
    const TensorIterationSpec::DimIterationSpec* spec =
        analysis->IterSpec(scope, &broadcast, dim.index);
    if (spec != nullptr && spec->at(0).stride > 0) {
      out_shape.push_back(dim.block_size);
    }
  }
  auto tensor_input = mlir::dyn_cast<TensorValue>(input);
  if (!tensor_input) {
    // Input is scalar.
    return Splat(b, input, out_shape);
  }
  if (tensor_input.getType().getRank() == out_shape.size()) {
    // No dimensions to broadcast.
    return input;
  }
  // Add broadcasted dimensions one by one.
  Value expanded_input = tensor_input;
  int dim_idx = 0;
  for (const DimProperties& dim : tiled_dimensions) {
    if (analysis->IterSpec(scope, &broadcast, dim.index) != nullptr &&
        analysis->IterSpec(scope, &broadcast, dim.index)->at(0).stride > 0) {
      if (analysis->IterSpec(scope, broadcast.operand(0), dim.index) ==
          nullptr) {
        // Broadcasted dimension.
        expanded_input = b.create<mt::ExpandDimsOp>(expanded_input, dim_idx);
      }
      ++dim_idx;
    }
  }
  return Broadcast(b, mlir::cast<TensorValue>(expanded_input), out_shape);
}

absl::StatusOr<Value> EmitScope(
    ImplicitLocOpBuilder& b, absl::string_view libdevice_path,
    const se::DeviceDescription& device_info,
    const TritonFusionAnalysis* analysis, TritonFusionAnalysis::Scope scope,
    absl::Span<const DimProperties> tiled_dimensions,
    absl::Span<const HloInstruction* const> instructions,
    absl::flat_hash_map<const HloInstruction*, Value>& values);

absl::StatusOr<Value> EmitReduce(ImplicitLocOpBuilder& b,
                                 absl::string_view libdevice_path,
                                 const se::DeviceDescription& device_info,
                                 const HloInstruction& hlo_reduce,
                                 Value input) {
  llvm::ArrayRef<int64_t> input_shape =
      mlir::cast<TensorValue>(input).getType().getShape();

  // At the moment, we should only emit a full reduction over the last axis of
  // a single input.
  TF_RET_CHECK(hlo_reduce.operand_count() == 2);
  TF_RET_CHECK(hlo_reduce.dimensions().size() == 1);
  TF_RET_CHECK(hlo_reduce.dimensions(0) ==
               hlo_reduce.operand(0)->shape().rank() - 1);
  const int block_row = input_shape.back();
  const int row_len = hlo_reduce.operand(0)->shape().dimensions_minor(0);
  TF_RET_CHECK(block_row >= row_len);

  const HloInstruction* operand = hlo_reduce.operand(1);
  Value neutral;

  // We assume that the reduction value was input as a constant, or in the case
  // of a data type affected by float normalization, a convert of a constant.
  if (operand->opcode() == HloOpcode::kConvert) {
    TF_RET_CHECK(operand->operand(0)->opcode() == HloOpcode::kConstant);
    TF_RET_CHECK(operand->operand(0)->shape().element_type() == BF16);
    PrimitiveType dest_ty = operand->shape().element_type();
    TF_RET_CHECK(dest_ty == F32);
    neutral = EmitConstant(b, *operand->operand(0));
    neutral = Cast(b, neutral, TritonType(b, dest_ty));
  } else {
    TF_RET_CHECK(operand->opcode() == HloOpcode::kConstant);
    neutral = EmitConstant(b, *operand);
  }

  // Since every shape is padded to a power of 2 in Triton, the input tile may
  // be padded with arbitrary values. These values could affect the result of
  // the reduction, so we need to mask them away. Luckily, we have a monoid
  // structure (element_type, hlo_reduce.to_apply(), hlo_reduce.operand(1))---
  // up to floating-point inaccuracies. Masking the input using
  // hlo_reduce.operand(1) is thus always the right choice to ensure that the
  // reduction is computed correctly, since it is the neutral value with regards
  // to the reducer.
  if (block_row != row_len) {
    Value mask = b.create<ma::CmpIOp>(
        ma::CmpIPredicate::slt, Range(b, block_row),
        Splat(b, CreateConst(b, b.getI32Type(), row_len), block_row));
    input = b.create<ma::SelectOp>(mask, input, Splat(b, neutral, input_shape));
  }

  // Triton actually only performs reductions on float32 inputs, and we must
  // thus upcast/downcast our input if its data type is different.
  Value casted_input = Cast(b, input, b.getF32Type());

  mt::ReduceOp reduction = b.create<mt::ReduceOp>(
      SmallVector<Value>({casted_input}), (int)input_shape.size() - 1);
  {
    mlir::Location loc = b.getLoc();
    mlir::Block* reducer =
        b.createBlock(&reduction->getRegion(0), {},
                      {b.getF32Type(), b.getF32Type()}, {loc, loc});

    HloComputation* reduction_computation = hlo_reduce.to_apply();

    std::vector<const HloInstruction*> to_emit;
    absl::flat_hash_map<const HloInstruction*, Value> region_values;
    for (const HloInstruction* instr :
         reduction_computation->MakeInstructionPostOrder()) {
      if (instr->opcode() == HloOpcode::kParameter) {
        int parameter_number = instr->parameter_number();
        TF_RET_CHECK(parameter_number < 2);
        TF_RET_CHECK(
            region_values
                .insert({instr, reducer->getArgument(parameter_number)})
                .second);
      } else {
        to_emit.push_back(instr);
      }
    }

    TF_RET_CHECK(!to_emit.empty());

    b.setInsertionPointToStart(reducer);
    TF_ASSIGN_OR_RETURN(
        Value result,
        EmitScope(b, libdevice_path, device_info, /*analysis=*/nullptr,
                  TritonFusionAnalysis::Scope::OUTPUT, {}, to_emit,
                  region_values));
    b.create<mt::ReduceReturnOp>(SmallVector<Value>({result}));
    b.setInsertionPointAfter(reduction);
  }

  Value result = reduction.getResult().front();

  // We want to return a tensor of float32, but the ReturnReduceOp produces an
  // f32 constant when reducing a single dim. To convert to a tensor we splat
  // the result.
  if (!mlir::dyn_cast<TensorValue>(reduction.getResult().front())) {
    result = Splat(b, result, {});
  }

  return Cast(b, result, TritonType(b, hlo_reduce.shape().element_type()));
}

// Emit code corresponding to a fusion instruction somehow nested within the
// initial Triton fusion. This can happen when we carry around auxiliary
// computations, e.g. with reduces. Since we are emitting a single Triton
// fusion, we simply flatten the fusion inside the computation.
//
// TODO(b/331413981): get rid of this special handling once this is solved.
absl::StatusOr<Value> EmitNestedFusion(
    ImplicitLocOpBuilder& b, absl::string_view libdevice_path,
    const se::DeviceDescription& device_info,
    const HloFusionInstruction& fusion_instruction,
    absl::flat_hash_map<const HloInstruction*, Value>& values) {
  // TODO(b/331402498): revisit the order of scope once we completely deprecate
  // Triton fusion analysis.
  const HloComputation* fusion_computation =
      fusion_instruction.fused_instructions_computation();

  absl::flat_hash_map<const HloInstruction*, Value> region_values;

  std::vector<const HloInstruction*> to_emit;
  for (const HloInstruction* instr :
       fusion_computation->MakeInstructionPostOrder()) {
    if (instr->opcode() == HloOpcode::kParameter) {
      int64_t parameter_number = instr->parameter_number();
      auto it = values.find(fusion_instruction.operand(parameter_number));
      TF_RET_CHECK(it != values.end());
      TF_RET_CHECK(region_values.insert({instr, it->second}).second);
    } else {
      to_emit.push_back(instr);
    }
  }

  TF_RET_CHECK(to_emit.back() == fusion_computation->root_instruction());

  return EmitScope(b, libdevice_path, device_info, /*analysis=*/nullptr,
                   TritonFusionAnalysis::Scope::OUTPUT, {}, to_emit,
                   region_values);
}

// TODO(b/331332678): Add unit tests to target this function specifically.
Value EmitTiledBroadcast(
    ImplicitLocOpBuilder& b, const TiledHloInstruction& tiled_broadcast,
    absl::flat_hash_map<const TiledHloInstruction*, Value>& values) {
  auto input_tile_shape = tiled_broadcast.operand(0)->tile_sizes();
  auto output_tile_shape = tiled_broadcast.tile_sizes();

  Value expanded_input = values[tiled_broadcast.operand(0)];

  // Returns true if `dim_id` is broadcasted.
  auto is_broadcasted_dim = [&](int64_t dim_id) {
    return !llvm::is_contained(tiled_broadcast.hlo()->dimensions(), dim_id);
  };

  // The loop below iterates over output dimensions and tracks matching dims in
  // input_tile_shape and expended_input value.
  // `input_dim_id != expanded_input_dim_id`, because size-1 dims are present in
  // the input tile shape, but not in the MLIR value. Triton doesn't like size-1
  // dims, so they are inserted only for dimensions that will be broadcasted.
  int64_t input_dim_id = 0;
  int64_t expanded_input_dim_id = 0;
  for (size_t output_dim_id = 0; output_dim_id < output_tile_shape.size();
       ++output_dim_id) {
    if (is_broadcasted_dim(output_dim_id)) {
      // The dim is broadcasted in the original instruction, but tiled to 1 in
      // this case. Nothing to broadcast.
      if (output_tile_shape[output_dim_id] == 1) continue;

      // Expand dim for broadcast.
      expanded_input =
          b.create<mt::ExpandDimsOp>(expanded_input, expanded_input_dim_id);
      ++expanded_input_dim_id;
    } else {
      // The dim is not broadcasted. Validate that it's equal in the input and
      // output tile.
      CHECK_EQ(input_tile_shape[input_dim_id],
               output_tile_shape[output_dim_id]);
      ++input_dim_id;

      // Size-1 dims are not present in the tensor type.
      if (output_tile_shape[output_dim_id] != 1) {
        ++expanded_input_dim_id;
      }
    }
  }

  SmallVector<int64_t> padded_output_tile_shape;
  padded_output_tile_shape.reserve(output_tile_shape.size());

  for (int64_t tile_dim : output_tile_shape) {
    if (tile_dim != 1) {
      padded_output_tile_shape.push_back(llvm::PowerOf2Ceil(tile_dim));
    }
  }

  return Broadcast(b, mlir::cast<TensorValue>(expanded_input),
                   padded_output_tile_shape);
}

absl::StatusOr<Value> EmitTiledHloInstruction(
    ImplicitLocOpBuilder& b, absl::string_view libdevice_path,
    const se::DeviceDescription& device_info,
    const TiledHloInstruction& tiled_hlo,
    std::function<absl::StatusOr<Value>(const TiledHloInstruction&)>
        emit_param_load_fn,
    absl::flat_hash_map<const TiledHloInstruction*, Value>& values) {
  const HloInstruction* hlo = tiled_hlo.hlo();

  if (hlo->opcode() == HloOpcode::kParameter) {
    return emit_param_load_fn(tiled_hlo);
  }

  if (hlo->opcode() == HloOpcode::kConstant &&
      ShapeUtil::IsEffectiveScalar(hlo->shape())) {
    // Splat makes it a tensor to avoid type mismatches.
    return Splat(b, EmitConstant(b, *hlo), {});
  }

  if (hlo->opcode() == HloOpcode::kBroadcast) {
    return EmitTiledBroadcast(b, tiled_hlo, values);
  }

  if (hlo->opcode() == HloOpcode::kReduce) {
    return EmitReduce(b, libdevice_path, device_info, *hlo,
                      values[tiled_hlo.operand(0)]);
  }

  if (hlo->IsElementwise()) {
    std::vector<Value> operands;
    operands.reserve(hlo->operands().size());

    for (const TiledHloInstruction* operand : tiled_hlo.operands()) {
      operands.push_back(values[operand]);
    }
    return EmitElementwise(b, libdevice_path, device_info, *hlo, operands);
  }

  if (hlo->opcode() == HloOpcode::kTranspose ||
      hlo->opcode() == HloOpcode::kSlice || hlo->opcode() == HloOpcode::kPad) {
    // All these are currently supported only as operations on indices
    // which are pushed to loads and stores. No operations on tiles are
    // performed here.
    return values[tiled_hlo.operand(0)];
  }

  return absl::UnimplementedError(
      absl::StrCat("Unsupported opcode: ", hlo->opcode()));
}

// Emit sequence of instructions using compatible tiling ordered producers
// before consumers.
absl::StatusOr<Value> EmitTiledScope(
    ImplicitLocOpBuilder& b, absl::string_view libdevice_path,
    const se::DeviceDescription& device_info,
    const std::vector<std::unique_ptr<TiledHloInstruction>>&
        tiled_hlo_instructions,
    std::function<absl::StatusOr<Value>(const TiledHloInstruction&)>
        emit_param_load_fn,
    absl::flat_hash_map<const TiledHloInstruction*, Value>& values) {
  for (const auto& tiled_hlo : tiled_hlo_instructions) {
    TF_ASSIGN_OR_RETURN(
        Value result,
        EmitTiledHloInstruction(b, libdevice_path, device_info, *tiled_hlo,
                                emit_param_load_fn, values));
    TF_RET_CHECK(values.insert({tiled_hlo.get(), result}).second)
        << tiled_hlo->hlo()->ToString();
    VLOG(8) << "Emitted "
            << tiled_hlo->hlo()->ToString(HloPrintOptions::ShortParsable());
  }
  return values[tiled_hlo_instructions.back().get()];
}

// Emit sequence of instructions using compatible tiling ordered producers
// before consumers.
absl::StatusOr<Value> EmitScope(
    ImplicitLocOpBuilder& b, absl::string_view libdevice_path,
    const se::DeviceDescription& device_info,
    const TritonFusionAnalysis* analysis, TritonFusionAnalysis::Scope scope,
    absl::Span<const DimProperties> tiled_dimensions,
    absl::Span<const HloInstruction* const> instructions,
    absl::flat_hash_map<const HloInstruction*, Value>& values) {
  for (const HloInstruction* hlo : instructions) {
    Value result;
    if (hlo->opcode() == HloOpcode::kConcatenate) {
      // Parameter loads and their concatenations are handled outside EmitScope.
      TF_RET_CHECK(values.contains(hlo)) << hlo->ToString();
      continue;
    } else if (hlo->opcode() == HloOpcode::kParameter) {
      if (hlo->users()[0]->opcode() == HloOpcode::kConcatenate) {
        continue;
      }
      TF_RET_CHECK(values.contains(hlo)) << hlo->ToString();
      continue;
    } else if (hlo->opcode() == HloOpcode::kConstant) {
      // Splat makes it a tensor to avoid type mismatches.
      result = Splat(b, EmitConstant(b, *hlo), {});
    } else if (hlo->opcode() == HloOpcode::kBroadcast) {
      TF_ASSIGN_OR_RETURN(
          result, EmitBroadcast(b, analysis, scope, tiled_dimensions, *hlo,
                                values[hlo->operand(0)]));
    } else if (hlo->opcode() == HloOpcode::kReduce) {
      TF_ASSIGN_OR_RETURN(result, EmitReduce(b, libdevice_path, device_info,
                                             *hlo, values[hlo->operand(0)]));
    } else if (HloInstruction::IsOpElementwise(hlo->opcode())) {
      std::vector<Value> operands;
      operands.reserve(hlo->operands().size());
      for (const HloInstruction* operand : hlo->operands()) {
        operands.push_back(values[operand]);
      }
      TF_ASSIGN_OR_RETURN(result, EmitElementwise(b, libdevice_path,
                                                  device_info, *hlo, operands));
    } else if (hlo->opcode() == HloOpcode::kTuple) {
      TF_RET_CHECK(hlo->IsRoot()) << hlo->ToString();
    } else if (hlo->opcode() == HloOpcode::kBitcast ||
               hlo->opcode() == HloOpcode::kTranspose ||
               hlo->opcode() == HloOpcode::kSlice ||
               hlo->opcode() == HloOpcode::kReshape ||
               hlo->opcode() == HloOpcode::kPad) {
      // All these are currently supported only as operations on indices
      // which are pushed to loads and stores. No operations on tiles are
      // performed here.
      result = values[hlo->operand(0)];
    } else if (hlo->opcode() == HloOpcode::kFusion) {
      const auto* fusion_instruction = ::xla::Cast<HloFusionInstruction>(hlo);
      TF_ASSIGN_OR_RETURN(result,
                          EmitNestedFusion(b, libdevice_path, device_info,
                                           *fusion_instruction, values));
    } else {
      return absl::InvalidArgumentError(
          absl::StrCat("Unsupported operation ", hlo->ToString()));
    }
    TF_RET_CHECK(values.insert({hlo, result}).second) << hlo->ToString();
    VLOG(8) << "Emitted " << hlo->ToString(HloPrintOptions::ShortParsable());
  }
  return values[instructions.back()];
}

// Extract additional attributes from an LLVM function that are not passed
// to the builder directly.
SmallVector<mlir::NamedAttribute> GetExtraAttrs(ml::LLVMFuncOp func) {
  llvm::StringSet<> registered_attr_names{
      func.getSymNameAttrName().getValue(),
      func.getFunctionTypeAttrName().getValue(),
      func.getLinkageAttrName().getValue(),
      func.getDsoLocalAttrName().getValue(),
      func.getCConvAttrName().getValue(),
      func.getArgAttrsAttrName().getValue(),
      func.getFunctionEntryCountAttrName().getValue()};
  return llvm::to_vector(
      llvm::make_filter_range(func->getAttrs(), [&](mlir::NamedAttribute attr) {
        return !registered_attr_names.contains(attr.getName().getValue());
      }));
}

// Strip address spaces from function parameters.
void StripParameterAddressSpaces(mlir::RewriterBase& rewriter,
                                 ml::LLVMFuncOp func) {
  // Figure out what the new signature should be.
  ml::LLVMFunctionType func_ty = func.getFunctionType();
  SmallVector<Type> generic_func_params(
      llvm::map_range(func_ty.getParams(), [](Type type) -> Type {
        auto ptr_ty = mlir::dyn_cast<ml::LLVMPointerType>(type);
        if (!ptr_ty) return type;
        if (ptr_ty.getAddressSpace() != mn::kGlobalMemorySpace) return type;
        return ml::LLVMPointerType::get(ptr_ty.getContext());
      }));
  ml::LLVMFunctionType generic_func_ty =
      func_ty.clone(generic_func_params, func_ty.getReturnTypes());

  // Create a function with the new signature.
  SmallVector<mlir::DictionaryAttr> arg_attrs(llvm::map_range(
      func.getArgAttrsAttr().getValue(), [](mlir::Attribute attr) {
        return mlir::cast<mlir::DictionaryAttr>(attr);
      }));
  auto generic_func = rewriter.create<ml::LLVMFuncOp>(
      func.getLoc(), func.getSymName(), generic_func_ty, func.getLinkage(),
      func.getDsoLocal(), func.getCConv(), /*comdat=*/nullptr,
      GetExtraAttrs(func), arg_attrs, func.getFunctionEntryCount());

  // Convert generic address spaces back to original ones within the function
  // body.
  mlir::Block* entry = generic_func.addEntryBlock(rewriter);
  rewriter.setInsertionPointToEnd(entry);
  SmallVector<Value> converted_args;
  for (auto [arg, type] :
       llvm::zip(generic_func.getArguments(), func_ty.getParams())) {
    Value converted = arg;
    if (arg.getType() != type) {
      converted = rewriter.create<ml::AddrSpaceCastOp>(arg.getLoc(), type, arg);
    }
    converted_args.push_back(converted);
  }

  // Move the rest of function body from the original function.
  rewriter.cloneRegionBefore(func.getBody(), generic_func.getBody(),
                             generic_func.getBody().end());
  rewriter.eraseOp(func);
  rewriter.mergeBlocks(entry->getNextNode(), entry, converted_args);
}

// Rewrite signatures of kernel functions to use generic data pointers and
// cast them to global ones within the kernel.
struct GeneralizeKernelSignaturePass
    : mlir::PassWrapper<GeneralizeKernelSignaturePass, mlir::OperationPass<>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(GeneralizeKernelSignaturePass);
  void runOnOperation() override {
    mlir::IRRewriter rewriter(&getContext());
    getOperation()->walk([&](ml::LLVMFuncOp func) {
      if (!func->hasAttr(mn::NVVMDialect::getKernelFuncAttrName())) {
        return;
      }
      rewriter.setInsertionPointAfter(func);
      StripParameterAddressSpaces(rewriter, func);
    });
  }
};

const TensorIterationSpec::DimIterationSpec* GetLhsNoncontractingSplitSpec(
    const TritonFusionAnalysis& analysis, int64_t lhs_noncontracting_dim_idx) {
  const TensorIterationSpec::DimIterationSpec* result = nullptr;
  for (const HloInstruction* lhs_param :
       analysis.ScopeParameters(TritonFusionAnalysis::Scope::LHS)) {
    const TensorIterationSpec::DimIterationSpec* spec =
        analysis.IterSpec(TritonFusionAnalysis::Scope::LHS, lhs_param,
                          lhs_noncontracting_dim_idx);
    if (spec != nullptr && spec->size() > 1) {
      CHECK_EQ(spec->size(), 2);
      if (result != nullptr) {
        CHECK_EQ(result->at(0).count, spec->at(0).count);
        CHECK_EQ(result->at(1).count, spec->at(1).count);
      }
      result = spec;
    }
  }
  return result;
}

// Structure for parameters relating to the MatMul shape and dimension indices.
//
// Variable naming: lhs [m, k] x rhs [k, n] -> out [m, n].
//
// The logical output dimensions are always ordered as:
//   split-K, batch, non-contracting LHS, non-contracting RHS,
// where split-K and batch are optional.
struct MatMulDims {
  static absl::StatusOr<MatMulDims> Create(
      const TritonGemmConfig& config, const HloDotInstruction& dot,
      const TritonFusionAnalysis& analysis);

  std::optional<int> out_split_k_dim_idx = std::nullopt;

  std::optional<int> lhs_batch_dim_idx = std::nullopt;
  std::optional<int> rhs_batch_dim_idx = std::nullopt;
  std::optional<int> out_batch_dim_idx = std::nullopt;

  // The LHS non-contracting can be split into two.
  std::optional<int64_t> lhs_noncontracting_split = std::nullopt;

  int lhs_contracting_dim_idx;
  int lhs_noncontracting_dim_idx;
  int rhs_contracting_dim_idx;
  int rhs_noncontracting_dim_idx;
  // The index of the LHS noncontracting dim in the output.
  int out_lhs_noncontracting_dim_idx;
  // The index of the RHS noncontracting dim in the output.
  int out_rhs_noncontracting_dim_idx;

  int64_t m;
  int64_t n;
  int64_t k;

 private:
  MatMulDims() = default;
};

// Structure for parameters relating to the MatMul launch grid.
struct MatMulLaunchConfig {
  explicit MatMulLaunchConfig(const TritonGemmConfig& config,
                              const HloDotInstruction& dot,
                              const MatMulDims& dims);

  int64_t grid_m;
  int64_t grid_n;
  LaunchDimensions launch_dims;
  mt::ProgramIDDim batch_program_id_dim;
  mt::ProgramIDDim noncontracting_program_id_dim;
};

/*static*/ absl::StatusOr<MatMulDims> MatMulDims::Create(
    const TritonGemmConfig& config, const HloDotInstruction& dot,
    const TritonFusionAnalysis& analysis) {
  MatMulDims matmul_dims;
  if (config.split_k > 1) {
    // split-k is always the first logical dimension.
    matmul_dims.out_split_k_dim_idx = 0;
  }

  int64_t num_split_k_dims = config.split_k > 1 ? 1 : 0;
  const auto& dims = dot.dot_dimension_numbers();
  matmul_dims.lhs_contracting_dim_idx = dims.lhs_contracting_dimensions(0);
  matmul_dims.lhs_noncontracting_dim_idx =
      GetNonContractingDims(dot.operand(0)->shape(),
                            dims.lhs_batch_dimensions(),
                            dims.lhs_contracting_dimensions())
          .value()[0];
  matmul_dims.rhs_contracting_dim_idx = dims.rhs_contracting_dimensions(0);
  matmul_dims.rhs_noncontracting_dim_idx =
      GetNonContractingDims(dot.operand(1)->shape(),
                            dims.rhs_batch_dimensions(),
                            dims.rhs_contracting_dimensions())
          .value()[0];

  if (dims.lhs_batch_dimensions_size() > num_split_k_dims) {
    matmul_dims.lhs_batch_dim_idx = *dims.lhs_batch_dimensions().rbegin();
    matmul_dims.rhs_batch_dim_idx = *dims.rhs_batch_dimensions().rbegin();
    // The batch dimension (if present) comes after the split-k dimension (if
    // present, otherwise it's the first dimension).
    matmul_dims.out_batch_dim_idx = num_split_k_dims;
  }

  // Logical output dimensions are always ordered as:
  //   split-K, batch, non-contracting LHS, non-contracting RHS,
  // where split-K and batch are optional.
  matmul_dims.out_rhs_noncontracting_dim_idx = dot.shape().rank() - 1;
  matmul_dims.out_lhs_noncontracting_dim_idx = dot.shape().rank() - 2;

  auto* root = dot.parent()->root_instruction();
  auto iter_spec =
      analysis.IterSpec(TritonFusionAnalysis::Scope::OUTPUT, root,
                        matmul_dims.out_rhs_noncontracting_dim_idx);
  TF_RET_CHECK(iter_spec != nullptr);
  matmul_dims.n = iter_spec->at(0).count;
  // Contracting dimension length.
  if (config.split_k > 1 &&
      dot.operand(1)->operand(0)->opcode() == HloOpcode::kPad) {
    // Unpadded LHS shape:  [..., k, ...]
    // Padded LHS shape:    [..., padded_k, ...]
    // Bitcasted LHS shape: [..., split_k, padded_k / split_k, ...]
    TF_RET_CHECK(dot.operand(1)->opcode() == HloOpcode::kBitcast);
    const Shape& unpadded_rhs_shape =
        dot.operand(1)->operand(0)->operand(0)->shape();
    matmul_dims.k =
        unpadded_rhs_shape.dimensions(dims.rhs_contracting_dimensions(0) - 1);
  } else {
    matmul_dims.k =
        dot.operand(1)->shape().dimensions(dims.rhs_contracting_dimensions(0)) *
        config.split_k;
  }

  auto* lhs_noncontracting_split_spec = GetLhsNoncontractingSplitSpec(
      analysis, matmul_dims.lhs_noncontracting_dim_idx);
  if (lhs_noncontracting_split_spec != nullptr) {
    // Just the fastest-varying part of it if the dimension is split.
    matmul_dims.m = lhs_noncontracting_split_spec->at(0).count;
    matmul_dims.lhs_noncontracting_split =
        lhs_noncontracting_split_spec->at(1).count;
  } else {
    matmul_dims.m = analysis
                        .IterSpec(TritonFusionAnalysis::Scope::OUTPUT, root,
                                  matmul_dims.out_lhs_noncontracting_dim_idx)
                        ->at(0)
                        .count;
  }

  // For now split non-contracting and batch are not supported
  // simultaneously because they are implemented via same mechanism.
  TF_RET_CHECK(!(matmul_dims.out_batch_dim_idx.has_value() &&
                 matmul_dims.lhs_noncontracting_split.has_value()));

  TF_RET_CHECK(matmul_dims.m >= 1);
  TF_RET_CHECK(matmul_dims.n >= 1);
  return std::move(matmul_dims);
}

MatMulLaunchConfig::MatMulLaunchConfig(const TritonGemmConfig& config,
                                       const HloDotInstruction& dot,
                                       const MatMulDims& dims)
    : grid_m((dims.m + config.block_m - 1) / config.block_m),
      grid_n((dims.n + config.block_n - 1) / config.block_n) {
  int64_t batch_size = dims.lhs_noncontracting_split.value_or(
      dims.out_batch_dim_idx.has_value()
          ? dot.shape().dimensions(*dims.out_batch_dim_idx)
          : 1);
  // X block size is 32-bit, Y and Z are 16-bit. Use X for large dimensions.
  constexpr int64_t kBlockCountYZLimit = 65536;

  // In the imaginary situation where both batch size and grid_m * grid_n
  // are over 65535 we have to give up. Given the minimal m, n block sizes of 16
  // this requires at least 256 GB of output.
  CHECK_LT(batch_size * grid_m * grid_n,
           kBlockCountYZLimit * kBlockCountYZLimit);

  const bool large_batch = batch_size >= kBlockCountYZLimit;
  if (large_batch) {
    batch_program_id_dim = mt::ProgramIDDim::X;
    noncontracting_program_id_dim = mt::ProgramIDDim::Y;
    launch_dims = LaunchDimensions(
        se::BlockDim(batch_size, grid_m * grid_n, config.split_k),
        se::ThreadDim(config.num_warps * WarpSize(), 1, 1));
  } else {
    batch_program_id_dim = mt::ProgramIDDim::Y;
    noncontracting_program_id_dim = mt::ProgramIDDim::X;
    launch_dims = LaunchDimensions(
        se::BlockDim(grid_m * grid_n, batch_size, config.split_k),
        se::ThreadDim(config.num_warps * WarpSize(), 1, 1));
  }
}

absl::Status ValidateMatMulConfig(const TritonGemmConfig& config,
                                  const HloDotInstruction& dot) {
  TF_RET_CHECK(config.split_k >= 1);
  TF_RET_CHECK(config.block_m >= 16);
  TF_RET_CHECK(config.block_k >= 16);
  TF_RET_CHECK(config.block_n >= 16);

  const auto& dims = dot.dot_dimension_numbers();
  int num_batch_dims =
      dims.lhs_batch_dimensions_size() - (config.split_k > 1 ? 1 : 0);
  TF_RET_CHECK(num_batch_dims <= 1);
  if (config.split_k > 1) {
    // Split-K dimension has to be the first batch one and have an index
    // just before the contracting one.
    const int lhs_split_k_dim_idx = dims.lhs_contracting_dimensions(0) - 1;
    const int rhs_split_k_dim_idx = dims.rhs_contracting_dimensions(0) - 1;
    // Size of this dimension has to match the split_k value.
    TF_RET_CHECK(dims.lhs_batch_dimensions(0) == lhs_split_k_dim_idx);
    TF_RET_CHECK(dims.rhs_batch_dimensions(0) == rhs_split_k_dim_idx);
    TF_RET_CHECK(config.split_k ==
                 dot.operand(0)->shape().dimensions(lhs_split_k_dim_idx));
    TF_RET_CHECK(config.split_k ==
                 dot.operand(1)->shape().dimensions(rhs_split_k_dim_idx));
  }

  // Rely on dot decomposer: there is just one contracting and one
  // non-contracting dimension on each side + batch ones optionally.
  TF_RET_CHECK(dims.lhs_contracting_dimensions_size() == 1);
  TF_RET_CHECK(dims.rhs_contracting_dimensions_size() == 1);

  TF_RET_CHECK(dot.operand(0)->shape().rank() ==
               2 + (config.split_k > 1 ? 1 : 0) + num_batch_dims);
  return absl::OkStatus();
}

struct Side {
  TritonFusionAnalysis::Scope scope;
  std::vector<DimProperties> tiled_dims;
  std::optional<int64_t> batch_dim_idx;
};

// if (index < limits[0]) {
//   return choices[0];
// } else if (index < limits[1]) {
//   return choices[1];
// } else if (...) {
// ...
// } else {
//   return choices.back();
// }
absl::StatusOr<Value> EmitMultiSelect(ImplicitLocOpBuilder b, Value index,
                                      ValueRange limits, ValueRange choices) {
  TF_RET_CHECK(choices.size() - 1 == limits.size());
  Value result = choices[0];
  for (int i = 0; i < choices.size() - 1; ++i) {
    result = b.create<ma::SelectOp>(
        b.create<ma::CmpIOp>(ma::CmpIPredicate::slt, index, limits[i]), result,
        choices[i + 1]);
  }
  return result;
}

absl::Status UncompilableMatmul(absl::string_view explanation) {
  absl::Status s = absl::CancelledError(explanation);
  s.SetPayload(kUncompilableFusion, absl::Cord(explanation));
  return s;
}

class MatMulEmitterHelper {
 public:
  MatMulEmitterHelper(absl::string_view libdevice_path,
                      const se::DeviceDescription& device_info,
                      const HloDotInstruction* dot_instr,
                      ImplicitLocOpBuilder& b, Type index_ty, MatMulDims dims,
                      const MatMulLaunchConfig& launch_config,
                      const TritonFusionAnalysis& analysis)
      : b_(b),
        libdevice_path_(libdevice_path),
        device_info_(device_info),
        dot_instr_(dot_instr),
        index_ty_(index_ty),
        analysis_(analysis),
        dims_(dims),
        launch_config_(launch_config) {}

  // TODO(b/266862493): Accumulator can be integer too.
  // Otherwise only f64 x f64 -> f64 uses f64 accumulator.
  mlir::FloatType GetDotAccumulatorType() {
    const PrecisionConfig::Algorithm algorithm =
        dot_instr_->precision_config().algorithm();

    if (algorithm == PrecisionConfig::ALG_UNSET) {
      Type dot_output_ty = TritonType(b_, dot_instr_->shape().element_type());
      // Data type of dot() immediate inputs.
      Type dot_input_ty = [&] {
        const Type lhs_ty =
            TritonType(b_, dot_instr_->operand(0)->shape().element_type());
        const Type rhs_ty =
            TritonType(b_, dot_instr_->operand(1)->shape().element_type());
        CHECK(lhs_ty == rhs_ty);
        return lhs_ty;
      }();
      // TODO(b/266862493): Accumulator can be integer too.
      // Otherwise only f64 x f64 -> f64 uses f64 accumulator.
      return (dot_output_ty.isF64() && dot_input_ty.isF64()) ? b_.getF64Type()
                                                             : b_.getF32Type();
    }

    absl::StatusOr<PrimitiveType> accum_type =
        algorithm_util::GetDotAccumulatorType(algorithm);
    CHECK(accum_type.ok()) << "Unexpected algorithm: "
                           << PrecisionConfig::Algorithm_Name(algorithm);
    Type mlir_accum_type = TritonType(b_, accum_type.value());
    if (auto float_accum_type =
            mlir::dyn_cast<mlir::FloatType>(mlir_accum_type)) {
      return float_accum_type;
    }
    LOG(FATAL) << "Only floating point accumulator types are supported for "
                  "now, but we got: "
               << llvm_ir::DumpToString(mlir_accum_type);
  }

  std::vector<const HloInstruction*> EpiloguePostOrderTransitiveOperands(
      const HloInstruction* root) {
    // Collect all instructions of the dot's output scope.
    absl::flat_hash_set<const HloInstruction*> to_order;
    {
      std::queue<const HloInstruction*> to_add;
      if (root != dot_instr_) {
        to_add.push(root);
      }
      while (!to_add.empty()) {
        const HloInstruction* current = to_add.front();
        for (const HloInstruction* operand : current->operands()) {
          if (!to_order.contains(operand)) {
            if (operand != dot_instr_) {
              to_add.push(operand);
            }
          }
        }
        CHECK(to_order.insert(current).second);
        to_add.pop();
      }
    }
    // Order them producers before consumers.
    std::vector<const HloInstruction*> to_emit;
    for (const HloInstruction* hlo :
         dot_instr_->parent()->MakeInstructionPostOrder()) {
      if (to_order.contains(hlo)) {
        to_emit.push_back(hlo);
      }
    }
    return to_emit;
  }

  Value MakeInput(Side& side, int64_t operand_index,
                  absl::flat_hash_map<const HloInstruction*, Value>& values) {
    return *EmitScope(
        b_, libdevice_path_, device_info_, &analysis_, side.scope,
        side.tiled_dims,
        dot_instr_->parent()->MakeInstructionPostOrderFrom(
            const_cast<HloInstruction&>(*dot_instr_->operand(operand_index))),
        values);
  }

  absl::StatusOr<Value> EmitTensorPointer(
      const HloInstruction* hlo, const Side& side, ValueRange bases,
      Value pid_k, std::vector<int32_t>& boundary_checks) {
    // Parameters of MakeTensorPtrOp to be generated by this function.
    Value base;
    std::vector<Value> bounds;
    std::vector<Value> strides;
    // Offsets from tensor origin, same for all thread blocks.
    std::vector<Value> tensor_offsets;
    std::vector<int32_t> block_dims;
    std::vector<int32_t> dim_order;

    // Offsets for a given thread block, typically pid * block size.
    // Used in a one-off AdvanceOp applied to the generated MakeTensorPtrOp.
    std::vector<Value> block_offsets;

    // Concatenations of parameters are handled during generation of block
    // pointers because of a limitation of implementation of block pointers
    // in the Triton compiler: block pointers are not supported inside
    // conditionals.
    // Therefore instead of directly using a conditional to emit a concatenation
    // and emitting its inputs inside the cases a single block pointer is
    // emitted for all inputs, but all its properties (base, strides etc) get
    // generated conditionally on the position of the current thread block
    // within the concatenated dimension.

    // Index of concatenated dimension if present, -1 otherwise.
    int concat_dim_idx;
    // Offsets along the concatenated dimension at which operands change.
    std::vector<Value> concat_boundaries;
    // Block index along the concatenated dimension * block size.
    Value concat_dim_pid_offset;

    if (hlo->opcode() == HloOpcode::kConcatenate) {
      // For now only non-contracting dimension can be concatenated.
      concat_dim_idx = (side.scope == TritonFusionAnalysis::Scope::LHS)
                           ? dims_.lhs_noncontracting_dim_idx
                           : dims_.rhs_noncontracting_dim_idx;
      const DimProperties& properties = [&] {
        for (const DimProperties& dim : side.tiled_dims) {
          if (dim.index == concat_dim_idx) {
            return dim;
          }
        }
        LOG(FATAL) << "Missing dimension.";
      }();
      TF_RET_CHECK(bases.size() == hlo->operand_count());

      concat_boundaries.reserve(hlo->operand_count() - 1);
      for (int i = 0; i < hlo->operand_count() - 1; ++i) {
        const TensorIterationSpec::IterationSpecFragment& fragment =
            analysis_.IterSpec(side.scope, hlo->operand(i), concat_dim_idx)
                ->at(0);
        if (fragment.sliced_count % properties.block_size != 0) {
          return UncompilableMatmul(
              "Operand is not divisible by the block size.");
        }
        concat_boundaries.push_back(
            Cst32(-fragment.slice_start + fragment.sliced_count));
      }

      concat_dim_pid_offset =
          b_.create<ma::MulIOp>(properties.pid, Cst32(properties.block_size));
      TF_ASSIGN_OR_RETURN(base, EmitMultiSelect(b_, concat_dim_pid_offset,
                                                concat_boundaries, bases));
    } else {
      concat_dim_idx = -1;
      base = bases[0];
    }

    auto add_dim = [&](const DimProperties& properties) -> absl::Status {
      if (analysis_.IterSpec(side.scope, hlo, properties.index) == nullptr) {
        return absl::OkStatus();
      }
      Value pid_offset =
          (properties.pid == nullptr)
              ? Cst32(0)
              : b_.create<ma::MulIOp>(properties.pid,
                                      Cst32(properties.block_size));
      std::vector<const HloInstruction*> inputs;
      if (hlo->opcode() == HloOpcode::kConcatenate) {
        inputs.insert(inputs.end(), hlo->operands().cbegin(),
                      hlo->operands().cend());
      } else {
        inputs = {hlo};
      }
      std::vector<const TensorIterationSpec::DimIterationSpec*> specs;
      std::vector<Value> input_strides;
      std::vector<Value> input_offsets;
      std::vector<Value> input_bounds;
      specs.reserve(inputs.size());
      input_strides.reserve(inputs.size());
      input_offsets.reserve(inputs.size());
      input_bounds.reserve(inputs.size());
      for (const HloInstruction* input : inputs) {
        specs.push_back(
            analysis_.IterSpec(side.scope, input, properties.index));
        input_strides.push_back(Cst64(specs.back()->at(0).stride));
        input_offsets.push_back(b_.create<ma::AddIOp>(
            pid_offset, Cst32(specs.back()->at(0).slice_start)));
        input_bounds.push_back(Cst64(specs.back()->at(0).count));
      }
      TF_ASSIGN_OR_RETURN(Value select_value,
                          EmitMultiSelect(b_, concat_dim_pid_offset,
                                          concat_boundaries, input_strides));
      strides.push_back(select_value);
      if (properties.index == concat_dim_idx) {
        TF_ASSIGN_OR_RETURN(
            select_value,
            EmitMultiSelect(b_, pid_offset, concat_boundaries, input_offsets));
        block_offsets.push_back(select_value);
        TF_ASSIGN_OR_RETURN(
            select_value,
            EmitMultiSelect(b_, pid_offset, concat_boundaries, input_bounds));
        bounds.push_back(select_value);
      } else {
        block_offsets.push_back(pid_offset);
        int64_t count = specs.front()->at(0).count;
        if (side.scope == TritonFusionAnalysis::Scope::OUTPUT &&
            properties.index == dims_.out_lhs_noncontracting_dim_idx &&
            specs.front()->size() == 1 &&
            dims_.lhs_noncontracting_split.has_value()) {
          // Dimension of the output produced by the non-contracting LHS one
          // is logically split, major part is addressed using pid_batch.
          count /= *dims_.lhs_noncontracting_split;
        }
        bounds.push_back(Cst64(count));
        if (count % (properties.block_size * properties.split_value) != 0) {
          boundary_checks.push_back(bounds.size() - 1);
        }
      }
      tensor_offsets.push_back(Cst32(specs.front()->at(0).slice_start));
      block_dims.push_back(properties.block_size);
      dim_order.emplace(dim_order.begin(), dim_order.size());
      return absl::OkStatus();
    };

    for (const DimProperties& dim : side.tiled_dims) {
      TF_RETURN_IF_ERROR(add_dim(dim));
    }

    int64_t offset_batch = 0;
    bool has_batch_offset = false;
    Value batch_stride;

    // Return the batch stride of the HLO passed as a parameter. If the
    // parameter HLO has no batch dimension, a zero stride is returned.
    // Also sets offset_batch and updates has_batch_offset as a side effect.
    auto get_batch_stride =
        [this, &side, &offset_batch, &has_batch_offset](
            const HloInstruction* hlo_param) -> absl::StatusOr<Value> {
      int64_t stride_batch = 0;
      if (side.scope != TritonFusionAnalysis::Scope::RHS &&
          dims_.lhs_noncontracting_split) {
        const TensorIterationSpec::DimIterationSpec* spec =
            analysis_.IterSpec(side.scope, hlo_param, side.tiled_dims[0].index);
        if (spec != nullptr) {
          if (spec->size() > 1) {
            // Support one specific kind of output transpose that splits the
            // dimension originating from the split LHS non-contracting one.
            stride_batch = spec->at(1).stride;
          } else {
            // Because the major part of the split is implemented using the
            // batch logic stride_batch is populated here as the stride of
            // the minor part times its size.
            stride_batch =
                spec->at(0).stride *
                (spec->at(0).count / *dims_.lhs_noncontracting_split);
          }
          TF_RET_CHECK(stride_batch != 0);
        }
      } else if (side.batch_dim_idx.has_value()) {
        const TensorIterationSpec::DimIterationSpec* spec =
            analysis_.IterSpec(side.scope, hlo_param, *side.batch_dim_idx);
        if (spec != nullptr) {
          stride_batch = spec->at(0).stride;
          offset_batch = spec->at(0).slice_start;
          TF_RET_CHECK(stride_batch != 0);
        }
      }

      has_batch_offset |= stride_batch != 0;
      return Cst(stride_batch);
    };

    if (hlo->opcode() == HloOpcode::kConcatenate) {
      std::vector<Value> batch_strides;
      batch_strides.reserve(hlo->operands().size());
      for (const HloInstruction* operand : hlo->operands()) {
        TF_ASSIGN_OR_RETURN(Value op_stride, get_batch_stride(operand));
        batch_strides.push_back(op_stride);
      }
      TF_ASSIGN_OR_RETURN(batch_stride,
                          EmitMultiSelect(b_, concat_dim_pid_offset,
                                          concat_boundaries, batch_strides));
    } else {
      TF_ASSIGN_OR_RETURN(batch_stride, get_batch_stride(hlo));
    }

    // Avoid generating logic to compute batch offset if unnecessary.
    if (has_batch_offset) {
      Value pid_batch =
          b_.create<mt::GetProgramIdOp>(launch_config_.batch_program_id_dim);
      Value pid_offset_batch = b_.create<ma::MulIOp>(
          b_.create<ma::AddIOp>(Cst(offset_batch), ConvertScalar(pid_batch)),
          batch_stride);
      base = AddPtr(b_, base, pid_offset_batch);
    }

    if (dims_.out_split_k_dim_idx.has_value()) {
      const TensorIterationSpec::DimIterationSpec* spec = analysis_.IterSpec(
          TritonFusionAnalysis::Scope::OUTPUT, hlo, *dims_.out_split_k_dim_idx);
      if (spec != nullptr) {
        TF_RET_CHECK(pid_k != nullptr);
        base = AddPtr(b_, base,
                      b_.create<ma::MulIOp>(ConvertScalar(pid_k),
                                            Cst(spec->at(0).stride)));
      }
    }

    if (block_dims.empty()) {
      // Load of a scalar.
      return base;
    }
    auto tensor_ptr = mlir::cast<Value>(
        b_.create<mt::MakeTensorPtrOp>(base, bounds, strides, tensor_offsets,
                                       block_dims, dim_order)
            .getResult());
    tensor_ptr = b_.create<mt::AdvanceOp>(tensor_ptr.getType(), tensor_ptr,
                                          block_offsets);
    return tensor_ptr;
  }

 private:
  // Extend int32 indexes to int64, if necessary.
  Value ConvertScalar(Value value) {
    if (index_ty_.getIntOrFloatBitWidth() == 64) {
      return b_.create<ma::ExtSIOp>(index_ty_, value);
    }
    return value;
  }

  Value Cst(int64_t v) { return CreateConst(b_, index_ty_, v); }
  Value Cst32(int32_t v) { return CreateConst(b_, i32_ty_, v); }
  Value Cst64(int64_t v) { return CreateConst(b_, i64_ty_, v); }

  ImplicitLocOpBuilder& b_;
  absl::string_view libdevice_path_;
  const se::DeviceDescription& device_info_;
  const HloDotInstruction* dot_instr_;
  Type index_ty_;
  TritonFusionAnalysis analysis_;
  MatMulDims dims_;
  MatMulLaunchConfig launch_config_;
  Type i32_ty_ = b_.getI32Type();
  Type i64_ty_ = b_.getI64Type();
};

}  // namespace

absl::StatusOr<LaunchDimensions> GetMatMulLaunchDimensions(
    const TritonFusionAnalysis& analysis, const HloFusionAdaptor& fusion,
    const TritonGemmConfig& config) {
  auto dot = HloFindIf(fusion.GetRoots(), fusion, [](auto node) {
    return node.opcode() == HloOpcode::kDot;
  });
  TF_RET_CHECK(dot != std::nullopt);
  const auto& dot_instr =
      *static_cast<const HloDotInstruction*>(&dot->instruction());
  TF_ASSIGN_OR_RETURN(MatMulDims dims,
                      MatMulDims::Create(config, dot_instr, analysis));
  MatMulLaunchConfig launch_config(config, dot_instr, dims);
  return launch_config.launch_dims;
}

SmallVector<Value> GetArguments(mlir::triton::FuncOp fn,
                                const HloInstruction& input) {
  if (input.opcode() == HloOpcode::kParameter) {
    return {fn.getArgument(input.parameter_number())};
  } else if (input.opcode() == HloOpcode::kConcatenate) {
    SmallVector<Value> result;
    for (const HloInstruction* operand : input.operands()) {
      result.push_back(fn.getArgument(operand->parameter_number()));
    }
    return result;
  }
  LOG(FATAL) << "Unexpected opcode: " << input.opcode();
}

// Concatenations can currently only be applied directly to parameters;
// all concatenated parameters share the same block pointer. This function
// returns all inputs of a kernel: concatenations of parameters and standalone
// parameters.
ConstHloInstructionSet ScopeInputs(const TritonFusionAnalysis& analysis,
                                   const TritonFusionAnalysis::Scope scope) {
  ConstHloInstructionSet result;
  for (const HloInstruction* parameter : analysis.ScopeParameters(scope)) {
    if (absl::c_any_of(parameter->users(), [](const HloInstruction* user) {
          return user->opcode() == HloOpcode::kConcatenate;
        })) {
      // Concatenation is always the only user of its parameters by
      // construction.
      CHECK_EQ(parameter->users().size(), 1);
      for (const HloInstruction* operand : parameter->users()[0]->operands()) {
        // All operands of a concatenation have to be computation parameters.
        CHECK_EQ(operand->opcode(), HloOpcode::kParameter);
      }
      result.insert(parameter->users()[0]);
    } else {
      result.insert(parameter);
    }
  }
  return result;
}

// Truncates |input| of F32 type to the number representable in Bf16 toward
// zero.
// It is used for Emit6xBfloat16MatMul.
Value TruncateToBF16TowardsZero(ImplicitLocOpBuilder& b, Value input) {
  ShapedType input_type = mlir::dyn_cast<ShapedType>(input.getType());
  Type input_type_as_i32 = input_type.clone(b.getI32Type());
  Value input_as_i32 = b.create<mt::BitcastOp>(input_type_as_i32, input);
  Value mask = CreateConst<uint32_t>(b, b.getI32Type(), 0xFFFF0000u,
                                     input_type.getShape());
  Value high_bits = b.create<ma::AndIOp>(input_type_as_i32, input_as_i32, mask);

  return b.create<mt::BitcastOp>(input_type, high_bits);
}

// Finds the middle 8 bits of |input|'s mantissa.
// It is used for Emit6xBfloat16MatMul.
Value SoftMiddleEight(ImplicitLocOpBuilder& b, Value input) {
  Value high = TruncateToBF16TowardsZero(b, input);
  return b.create<ma::SubFOp>(input, high);
}

// Finds the low 8 bits of |input|'s mantissa.
// It is used for Emit6xBfloat16MatMul.
Value SoftLowEight(ImplicitLocOpBuilder& b, Value input) {
  // Find the middle bits of the middle bits, and these are the low eight
  // bits.
  return SoftMiddleEight(b, SoftMiddleEight(b, input));
}

// Rounds |input| to BF16 type.
// It is used for Emit6xBfloat16MatMul.
Value RoundToBF16(ImplicitLocOpBuilder& b, Value input) {
  return Cast(b, input, b.getBF16Type());
}

// Checks |input| is finite f32 (not Nan and not infinite).
// It is used for Emit6xBfloat16MatMul and Emit3xBfloat16MatMul.
Value CheckFiniteF32(ImplicitLocOpBuilder& b, Value input) {
  Value positive_inf = CreateConst<float>(
      b, b.getF32Type(), std::numeric_limits<float>::infinity(),
      mlir::cast<ShapedType>(input.getType()).getShape());
  Value abs_input = b.create<mm::AbsFOp>(input);
  return b.create<ma::CmpFOp>(ma::CmpFPredicate::OGT, positive_inf, abs_input);
}

// Leverages BF16 datatype for F32 matmul computation. It follows the guidance
// from https://arxiv.org/pdf/1904.06376.pdf.
absl::StatusOr<Value> Emit6xBfloat16MatMul(ImplicitLocOpBuilder& b, Value lhs,
                                           Value rhs, Value acc) {
  Type f32 = b.getF32Type();
  TF_RET_CHECK(mlir::cast<ShapedType>(lhs.getType()).getElementType() == f32);
  TF_RET_CHECK(mlir::cast<ShapedType>(rhs.getType()).getElementType() == f32);
  TF_RET_CHECK(mlir::cast<ShapedType>(acc.getType()).getElementType() == f32);

  Value lhs_high = RoundToBF16(b, TruncateToBF16TowardsZero(b, lhs));
  Value lhs_middle =
      RoundToBF16(b, TruncateToBF16TowardsZero(b, SoftMiddleEight(b, lhs)));
  Value lhs_low =
      RoundToBF16(b, TruncateToBF16TowardsZero(b, SoftLowEight(b, lhs)));

  Value rhs_high = RoundToBF16(b, TruncateToBF16TowardsZero(b, rhs));
  Value rhs_middle =
      RoundToBF16(b, TruncateToBF16TowardsZero(b, SoftMiddleEight(b, rhs)));
  Value rhs_low =
      RoundToBF16(b, TruncateToBF16TowardsZero(b, SoftLowEight(b, rhs)));

  auto bf16_dot = [&](Value lhs_bf16, Value rhs_bf16,
                      Value accumulator) -> Value {
    return b.create<mt::DotOp>(lhs_bf16, rhs_bf16, accumulator,
                               /*allowTF32=*/false,
                               /*maxNumImpreciseAcc=*/0);
  };

  Value local_acc = ZerosLike(b, acc);
  Value result = bf16_dot(lhs_middle, rhs_middle, local_acc);
  result = bf16_dot(lhs_low, rhs_high, result);
  result = bf16_dot(lhs_high, rhs_low, result);
  result = bf16_dot(lhs_middle, rhs_high, result);
  result = bf16_dot(lhs_high, rhs_middle, result);
  // If lhs is 1.0, we will have lhs_high = 1.0 and lhs_low = 0.0.
  // If rhs is +infinity, we will have:
  // +infinity * 1.0 = +infinity
  // +infinity * 0.0 = NaN
  // We would get the wrong result if we sum these partial products. Instead, we
  // must override any accumulated result if the last partial product is
  // non-finite. See b/115844437.
  Value is_finite = CheckFiniteF32(b, result);
  result = b.create<ma::SelectOp>(is_finite, result, ZerosLike(b, result));
  result = bf16_dot(lhs_high, rhs_high, result);
  result = b.create<ma::AddFOp>(acc, result);
  return result;
}

// Compute F32 matmul with 3 BF16 dots. It is less accurate than
// Emit6xBfloat16MatMul.
absl::StatusOr<Value> Emit3xBfloat16MatMul(ImplicitLocOpBuilder& b, Value lhs,
                                           Value rhs, Value acc) {
  Type f32 = b.getF32Type();
  TF_RET_CHECK(mlir::cast<ShapedType>(lhs.getType()).getElementType() == f32);
  TF_RET_CHECK(mlir::cast<ShapedType>(rhs.getType()).getElementType() == f32);
  TF_RET_CHECK(mlir::cast<ShapedType>(acc.getType()).getElementType() == f32);

  Value lhs_high = RoundToBF16(b, TruncateToBF16TowardsZero(b, lhs));
  Value lhs_low = RoundToBF16(b, SoftMiddleEight(b, lhs));

  Value rhs_high = RoundToBF16(b, TruncateToBF16TowardsZero(b, rhs));
  Value rhs_low = RoundToBF16(b, SoftMiddleEight(b, rhs));

  auto bf16_dot = [&](Value lhs_bf16, Value rhs_bf16,
                      Value accumulator) -> Value {
    return b.create<mt::DotOp>(lhs_bf16, rhs_bf16, accumulator,
                               /*allowTF32=*/false,
                               /*maxNumImpreciseAcc=*/0);
  };

  Value local_acc = ZerosLike(b, acc);
  Value result = bf16_dot(lhs_low, rhs_high, local_acc);
  result = bf16_dot(lhs_high, rhs_low, result);
  Value is_finite = CheckFiniteF32(b, result);
  result = b.create<ma::SelectOp>(is_finite, result, ZerosLike(b, result));
  result = bf16_dot(lhs_high, rhs_high, result);
  result = b.create<ma::AddFOp>(acc, result);
  return result;
}

namespace {

bool IsTf32Allowed(const HloDotInstruction* dot_instr) {
  const PrecisionConfig::Algorithm algorithm =
      dot_instr->precision_config().algorithm();

  if (algorithm == PrecisionConfig::ALG_UNSET) {
    return tsl::tensor_float_32_execution_enabled() &&
           absl::c_none_of(dot_instr->precision_config().operand_precision(),
                           [](const int precision) {
                             return precision != PrecisionConfig::DEFAULT;
                           });
  }

  return algorithm_util::HasTf32InputType(algorithm);
}

bool Is6xBfloat16MatMul(const HloDotInstruction* dot_instr,
                        mlir::OpBuilder& builder, Value dot_input_lhs,
                        Value dot_input_rhs,
                        const se::DeviceDescription& device_info) {
  const PrecisionConfig::Algorithm algorithm =
      dot_instr->precision_config().algorithm();

  if (algorithm == PrecisionConfig::ALG_UNSET) {
    const HloModule* hlo_module = dot_instr->GetModule();
    Type f32 = builder.getF32Type();
    return hlo_module->config()
               .debug_options()
               .xla_gpu_enable_bf16_6way_gemm() &&
           mlir::cast<ShapedType>(dot_input_lhs.getType()).getElementType() ==
               f32 &&
           mlir::cast<ShapedType>(dot_input_rhs.getType()).getElementType() ==
               f32;
  }

  return algorithm == PrecisionConfig::ALG_DOT_BF16_BF16_F32_X6;
}

bool Is3xBfloat16MatMul(const HloDotInstruction* dot_instr,
                        mlir::OpBuilder& builder, Value dot_input_lhs,
                        Value dot_input_rhs,
                        const se::DeviceDescription& device_info) {
  const PrecisionConfig::Algorithm algorithm =
      dot_instr->precision_config().algorithm();

  if (algorithm == PrecisionConfig::ALG_UNSET) {
    const HloModule* hlo_module = dot_instr->GetModule();
    Type f32 = builder.getF32Type();
    return hlo_module->config()
               .debug_options()
               .xla_gpu_enable_bf16_3way_gemm() &&
           mlir::cast<ShapedType>(dot_input_lhs.getType()).getElementType() ==
               f32 &&
           mlir::cast<ShapedType>(dot_input_rhs.getType()).getElementType() ==
               f32;
  }

  return algorithm == PrecisionConfig::ALG_DOT_BF16_BF16_F32_X3;
}

// This is a heuristic that serves as a proxy for register usage and code size.
//
// We have noticed that tilings with very long LLVM IR code are both slow to
// compile and slow to run. This can be for example due to register spills. So
// we should skip these tilings to save time. But it's better to skip them
// before the LLVM IR is generated. To do that, we came up with a formula that
// strongly correlates with the LLVM IR size. The formula is the size of the two
// input and the output thread block tiles divided by the number of warps. We
// read https://developer.nvidia.com/blog/cutlass-linear-algebra-cuda/ as a
// reference, and found the formula by trial and error.
//
// To regenerate the limit, we have to run an exhaustive search on all tilings
// for a few different HLOs, printing the runtimes and the heuristic values.
//
// From that, we can find a limit, such that all tilings within alpha *
// optimal_runtime have a heuristic value less than or equal to the limit.
//
// In our measurements, all tilings which were within 1.13 * optimal_runtime had
// a complexity_heuristic_value <= kComplexityHeuristicLimit.
//
// See go/tiling-heuristic for more details.
absl::Status CheckGemmTilingComplexityHeuristic(
    const TritonGemmConfig& config) {
  constexpr int64_t kComplexityHeuristicLimit = 9000;
  int64_t complexity_heuristic_value =
      (config.block_m * config.block_n +
       (config.block_m + config.block_n) * config.block_k) /
      config.num_warps;
  VLOG(2) << "Complexity heuristic: " << complexity_heuristic_value;
  if (complexity_heuristic_value > kComplexityHeuristicLimit) {
    return ResourceExhausted("Tiling complexity heuristic exceeded: %d > %d",
                             complexity_heuristic_value,
                             kComplexityHeuristicLimit);
  }
  return absl::OkStatus();
}

}  // namespace

// Variable naming: lhs [m, k] x rhs [k, n] -> out [m, n].
absl::Status EmitMatMul(mlir::OpBuilder builder,
                        absl::string_view libdevice_path,
                        const se::DeviceDescription& device_info,
                        const TritonFusionAnalysis& analysis,
                        const HloComputation* computation,
                        mlir::triton::FuncOp fn,
                        const TritonGemmConfig& config) {
  TF_RETURN_IF_ERROR(CheckGemmTilingComplexityHeuristic(config));

  const HloInstruction* instr =
      hlo_query::GetFirstInstructionWithOpcode(*computation, HloOpcode::kDot);
  const HloDotInstruction* dot_instr = DynCast<HloDotInstruction>(instr);
  bool is_sparse = dot_instr->sparse_operands() > 0;

  // Use 32-bit indexing if addressing any of the inputs or the output (which
  // could grow if split_k is set) does not cross the INT_MAX boundary.
  // Otherwise, fall back to 64-bit indexing, which is slower.
  bool use_64bit_indexing =
      ShapeUtil::ElementsIn(dot_instr->operand(0)->shape()) > INT_MAX ||
      ShapeUtil::ElementsIn(dot_instr->operand(1)->shape()) > INT_MAX ||
      ShapeUtil::ElementsIn(dot_instr->shape()) * config.split_k > INT_MAX;
  Type index_ty = builder.getIntegerType(use_64bit_indexing ? 64 : 32);

  const HloInstruction* root = dot_instr->parent()->root_instruction();
  TF_RET_CHECK(!root->shape().IsTuple());

  auto fusion_adaptor = HloFusionAdaptor::ForComputation(computation);
  HloInstructionAdaptor instr_adaptor{*instr, fusion_adaptor.get()};
  // TODO(b/320659359) Allow TF32 for 8-bit or less types with F32.
  bool is_8_bit_or_less_dot_with_F32 = HloAnyOf(
      instr_adaptor.GetOperands(), *fusion_adaptor,
      [&](HloInstructionAdaptor node) {
        if (node.opcode() != HloOpcode::kConvert) {
          return false;
        }
        Type in_type =
            TritonType(builder, node.GetOperand(0).shape().element_type());
        Type out_type = TritonType(builder, node.shape().element_type());
        return in_type.getIntOrFloatBitWidth() <= 8 && out_type.isF32();
      });

  // We'll be creating a lot of instructions from a single dot, use an
  // implicit loc builder so we don't have to pass around the location all the
  // time.
  auto loc = mlir::NameLoc::get(builder.getStringAttr(dot_instr->name()));
  ImplicitLocOpBuilder b(loc, builder);

  TF_RETURN_IF_ERROR(ValidateMatMulConfig(config, *dot_instr));
  const int split_k = config.split_k;
  const int block_m = config.block_m;
  const int block_k = config.block_k;
  const int block_n = config.block_n;

  TF_ASSIGN_OR_RETURN(const MatMulDims dims,
                      MatMulDims::Create(config, *dot_instr, analysis));
  const MatMulLaunchConfig launch_config(config, *dot_instr, dims);
  VLOG(6) << analysis.ToString();

  MatMulEmitterHelper emitter(libdevice_path, device_info, dot_instr, b,
                              index_ty, dims, launch_config, analysis);

  constexpr int group_m = 8;
  const int64_t width = group_m * launch_config.grid_n;

  auto c32 = [&](int64_t v) { return CreateConst(b, b.getI32Type(), v); };

  auto pid_nc =
      b.create<mt::GetProgramIdOp>(launch_config.noncontracting_program_id_dim);
  Value pid_k = (split_k > 1)
                    ? b.create<mt::GetProgramIdOp>(mt::ProgramIDDim::Z)
                    : Value{};

  auto group_id = b.create<ma::DivSIOp>(pid_nc, c32(width));
  ma::ConstantOp group_m_op = c32(group_m);
  auto first_pid_m = b.create<ma::MulIOp>(group_id, group_m_op);
  auto sub0 = b.create<ma::SubIOp>(c32(launch_config.grid_m), first_pid_m);
  auto group_size = b.create<ma::SelectOp>(
      b.create<ma::CmpIOp>(ma::CmpIPredicate::slt, sub0, group_m_op), sub0,
      group_m_op);

  auto pid_m = b.create<ma::AddIOp>(first_pid_m,
                                    b.create<ma::RemSIOp>(pid_nc, group_size));
  auto pid_n = b.create<ma::DivSIOp>(b.create<ma::RemSIOp>(pid_nc, c32(width)),
                                     group_size);

  mlir::FloatType acc_ty = emitter.GetDotAccumulatorType();

  ma::ConstantOp accumulator_init =
      CreateConst(b, acc_ty, 0, {block_m, block_n});

  // Parameters are passed to the loop in non-trivial order, these maps help
  // finding them and their attributes.
  absl::flat_hash_map<int, const HloInstruction*> iter_args_to_inputs;
  absl::flat_hash_map<int, std::vector<int32_t>> iter_args_to_boundary_checks;

  Side lhs{TritonFusionAnalysis::Scope::LHS,
           /*tiled_dims=*/
           {DimProperties(dims.lhs_noncontracting_dim_idx, pid_m, block_m,
                          /*split_value=*/1),
            DimProperties(dims.lhs_contracting_dim_idx, pid_k,
                          block_k / (1 + is_sparse), split_k)},
           dims.lhs_batch_dim_idx};
  Side rhs{
      TritonFusionAnalysis::Scope::RHS,
      /*tiled_dims=*/
      {DimProperties(dims.rhs_contracting_dim_idx, pid_k, block_k, split_k),
       DimProperties(dims.rhs_noncontracting_dim_idx, pid_n, block_n,
                     /*split_value=*/1)},
      dims.rhs_batch_dim_idx};
  Side out{TritonFusionAnalysis::Scope::OUTPUT,
           /*tiled_dims=*/
           {DimProperties(dims.out_lhs_noncontracting_dim_idx, pid_m, block_m,
                          /*split_value=*/1),
            DimProperties(dims.out_rhs_noncontracting_dim_idx, pid_n, block_n,
                          /*split_value=*/1)},
           dims.out_batch_dim_idx};

  std::vector<Side> scopes = {lhs, rhs};
  if (is_sparse) {
    scopes.push_back(
        {TritonFusionAnalysis::Scope::META,
         /*tiled_dims=*/
         {DimProperties(dims.lhs_noncontracting_dim_idx, pid_m, block_m,
                        /*split_value=*/1),
          DimProperties(dims.lhs_contracting_dim_idx, pid_k, block_k / 16,
                        split_k)},
         dims.lhs_batch_dim_idx});
  }

  constexpr size_t kLhsMetaOperandIdx = HloDotInstruction::kOperands;
  size_t lsize = ScopeInputs(analysis, TritonFusionAnalysis::Scope::LHS).size();
  size_t rsize = ScopeInputs(analysis, TritonFusionAnalysis::Scope::RHS).size();

  auto body_builder = [&](mlir::OpBuilder&, mlir::Location, Value ki,
                          ValueRange iter_args) {
    SmallVector<Value> iter_args_next;
    iter_args_next.reserve(iter_args.size());
    std::array<absl::flat_hash_map<const HloInstruction*, Value>, 3> values;

    // Load tiles of all parameters of LHS and RHS scopes and advance pointers.
    for (int i = 0; i < iter_args.size() - 1; ++i) {
      const int index = i < lsize ? 0 : i < lsize + rsize ? 1 : 2;
      Side& side = scopes[index];

      const HloInstruction* param_hlo = iter_args_to_inputs[i];
      Type param_ty = index == kLhsMetaOperandIdx
                          ? b.getI16Type()
                          : TritonType(b, param_hlo->shape().element_type());
      Type param_storage_ty = StorageType(b, param_ty);
      Value param_value =
          EmitParameterLoad(b, iter_args[i], iter_args_to_boundary_checks[i]);
      if (param_ty != param_storage_ty) {
        // For example cast i8 to i1.
        param_value = Cast(b, param_value, param_ty);
      }

      CHECK(values[index].insert({param_hlo, param_value}).second);
      SmallVector<Value> increments;
      for (const DimProperties& dim : side.tiled_dims) {
        const TensorIterationSpec::DimIterationSpec* spec =
            analysis.IterSpec(side.scope, iter_args_to_inputs[i], dim.index);
        if (spec == nullptr || spec->at(0).stride == 0) {
          continue;
        }
        // Only the contracting dimensions are advanced.
        if (dim.index == (index == 0 || index == kLhsMetaOperandIdx
                              ? dims.lhs_contracting_dim_idx
                              : dims.rhs_contracting_dim_idx)) {
          increments.push_back(c32(dim.block_size * split_k));
        } else {
          increments.push_back(c32(0));
        }
      }
      if (increments.empty()) {
        iter_args_next.push_back(iter_args[i]);
      } else {
        iter_args_next.push_back(b.create<mt::AdvanceOp>(
            iter_args[i].getType(), iter_args[i], increments));
      }
    }

    // Emit all operations of LHS and RHS scopes.
    Value dot_input_lhs = emitter.MakeInput(lhs, 0, values[0]);
    Value dot_input_rhs = emitter.MakeInput(rhs, 1, values[1]);
    Value dot_input_meta =
        is_sparse ? emitter.MakeInput(scopes.back(), 2, values[2]) : Value{};

    // Operation in the fusion before the dot can alter the elements of the
    // tiles that were zero masked during loads. These have to be zeroed here
    // again just before the dot so that they do not affect the output.
    // Only the K dimension needs masking here because unnecessary elements in
    // the other two get discarded by the masked store at the end.
    const bool need_masking = dims.k % (block_k * split_k) > 0;
    if (need_masking) {
      auto apply_mask = [&](int64_t dim, Value input, int denom) {
        auto elements_in_tile = b.create<ma::SubIOp>(c32(dims.k / denom), ki);
        int size = block_k / denom;
        auto range_k = Range(b, size);
        if (pid_k != nullptr) {
          range_k = b.create<ma::AddIOp>(
              range_k, Splat(b, b.create<ma::MulIOp>(pid_k, c32(size)), size));
        }
        auto ty = mlir::cast<mlir::RankedTensorType>(input.getType());
        TensorValue range_expanded = mlir::cast<TensorValue>(
            b.create<mt::ExpandDimsOp>(range_k, dim).getResult());
        Value mask = b.create<mt::BroadcastOp>(
            ty.clone(b.getI1Type()),
            b.create<ma::CmpIOp>(ma::CmpIPredicate::slt, range_expanded,
                                 Splat(b, elements_in_tile,
                                       range_expanded.getType().getShape())));
        return b.create<ma::SelectOp>(mask, input, ZerosLike(b, input));
      };
      dot_input_lhs = apply_mask(0, dot_input_lhs, is_sparse ? 2 : 1);
      dot_input_rhs = apply_mask(1, dot_input_rhs, 1);
      // Masking the metadata is not necessary, as the inputs are masked
      // (i.e. zeroed out), so the padded metadata can hold any values.
    }

    if (is_sparse) {
      iter_args_next.push_back(b.create<mt::gpu::SparseDotOp>(
          dot_input_lhs, dot_input_rhs, iter_args.back(), dot_input_meta));
      b.create<mlir::scf::YieldOp>(iter_args_next);
      return;
    }

    const HloModule* hlo_module = dot_instr->GetModule();
    if (hlo_module->config().debug_options().xla_gpu_enable_bf16_3way_gemm() &&
        hlo_module->config().debug_options().xla_gpu_enable_bf16_6way_gemm()) {
      LOG(WARNING) << "Both BF16 6way gemm and 3way gemm are enabled."
                   << " Fallback to BF16 6way gemm.";
    }

    Value accumulator_next;
    if (Is6xBfloat16MatMul(dot_instr, b, dot_input_lhs, dot_input_rhs,
                           device_info)) {
      absl::StatusOr<Value> accumulator_next_or = Emit6xBfloat16MatMul(
          b, dot_input_lhs, dot_input_rhs, iter_args.back());
      TF_CHECK_OK(accumulator_next_or.status());
      accumulator_next = accumulator_next_or.value();
    } else if (Is3xBfloat16MatMul(dot_instr, b, dot_input_lhs, dot_input_rhs,
                                  device_info)) {
      absl::StatusOr<Value> accumulator_next_or = Emit3xBfloat16MatMul(
          b, dot_input_lhs, dot_input_rhs, iter_args.back());
      TF_CHECK_OK(accumulator_next_or.status());
      accumulator_next = accumulator_next_or.value();
    } else {
      // Execute matrix multiplication of input tiles and pass the accumulator.
      // TODO(manany): Should be looked into once we enable Hopper workloads.
      // maxNumImpreciseAcc flag was introduced for Hopper to accumulate in a
      // lower precision than the output type. The change was introduced here:
      // https://github.com/openai/triton/commit/31b0c521427109a8eda609b58d756c380b21599a
      accumulator_next =
          b.create<mt::DotOp>(dot_input_lhs, dot_input_rhs, iter_args.back(),
                              /*allowTF32=*/IsTf32Allowed(dot_instr) &&
                                  !is_8_bit_or_less_dot_with_F32,
                              /*maxNumImpreciseAcc=*/0);
    }
    iter_args_next.push_back(accumulator_next);

    b.create<mlir::scf::YieldOp>(iter_args_next);
  };

  // Pointers to inputs of LHS scope, then RHS, then the accumulator
  // that change with every loop iteration and are passed between them.
  SmallVector<Value> iter_args;
  iter_args.reserve(lsize + rsize + 1 + is_sparse);

  for (const Side& side : scopes) {
    for (const HloInstruction* input : ScopeInputs(analysis, side.scope)) {
      TF_RET_CHECK(
          iter_args_to_inputs.insert({iter_args.size(), input}).second);
      TF_ASSIGN_OR_RETURN(Value tensor_ptr,
                          emitter.EmitTensorPointer(
                              input, side, GetArguments(fn, *input), pid_k,
                              iter_args_to_boundary_checks[iter_args.size()]));
      iter_args.push_back(tensor_ptr);
    }
  }

  iter_args.push_back(accumulator_init);
  Value acc_final = b.create<mlir::scf::ForOp>(
                         /*lowerBound=*/c32(0),
                         /*upperBound=*/c32(dims.k),
                         /*step=*/c32(block_k * split_k),
                         /*iterArgs=*/iter_args, body_builder)
                        .getResult(iter_args.size() - 1);
  absl::flat_hash_map<const HloInstruction*, Value> values_out;
  values_out[dot_instr] =
      Cast(b, acc_final, TritonType(b, dot_instr->shape().element_type()));

  // Emit the output scope.
  if (std::vector<const HloInstruction*> to_emit =
          emitter.EpiloguePostOrderTransitiveOperands(root);
      !to_emit.empty()) {
    for (const HloInstruction* input :
         ScopeInputs(analysis, TritonFusionAnalysis::Scope::OUTPUT)) {
      std::vector<int32_t> boundary_checks;
      TF_ASSIGN_OR_RETURN(
          Value tensor_pointer,
          emitter.EmitTensorPointer(input, out, GetArguments(fn, *input), pid_k,
                                    boundary_checks));
      TF_RET_CHECK(values_out
                       .insert({input, EmitParameterLoad(b, tensor_pointer,
                                                         boundary_checks)})
                       .second);
    }
    TF_RETURN_IF_ERROR(EmitScope(b, libdevice_path, device_info, &analysis,
                                 TritonFusionAnalysis::Scope::OUTPUT,
                                 out.tiled_dims, to_emit, values_out)
                           .status());
  }

  // Emit tensor store operations for all outputs.
  for (int i = 0;
       i < fn.getNumArguments() - dot_instr->parent()->num_parameters(); ++i) {
    const HloInstruction* producer =
        root->shape().IsTuple() ? root->operand(i) : root;
    std::vector<int32_t> boundary_checks;
    TF_ASSIGN_OR_RETURN(
        Value tensor_pointer,
        emitter.EmitTensorPointer(
            producer, out,
            {fn.getArgument(i + dot_instr->parent()->num_parameters())}, pid_k,
            boundary_checks));
    b.create<mt::StoreOp>(tensor_pointer, values_out[producer], boundary_checks,
                          mt::CacheModifier::NONE, mt::EvictionPolicy::NORMAL);
  }
  return absl::OkStatus();
}

// Computes the base pointer offset for the given pid and shape.
// `tile_offset_indexing` is a mapping from
// (program_id) -> [tile_offset0, ..., tile_offsetN]
Value ComputeBasePtrOffset(ImplicitLocOpBuilder b, Value pid,
                           const TiledHloInstruction& tiled_hlo) {
  const Shape& shape = tiled_hlo.hlo()->shape();
  ArrayRef<mlir::AffineExpr> dimension_exprs =
      tiled_hlo.block_id_to_tile_offsets_indexing().GetAffineMap().getResults();

  mlir::AffineExpr linear_index =
      mlir::getAffineConstantExpr(0, b.getContext());
  int64_t stride = 1;
  for (int i : shape.layout().minor_to_major()) {
    linear_index = linear_index + dimension_exprs[i] * stride;
    stride *= shape.dimensions(i);
  }

  return b.create<ma::IndexCastUIOp>(
      b.getI64Type(),
      mlir_converter::ApplyAffineExpr(linear_index, /*dims=*/pid,
                                      /*symbols=*/{}, b));
}

absl::Status EmitTiledSoftMax(mlir::OpBuilder builder,
                              absl::string_view libdevice_path,
                              const se::DeviceDescription& device_info,
                              SymbolicTileAnalysis* analysis,
                              const HloComputation* computation,
                              mlir::triton::FuncOp fn) {
  const HloInstruction* root = computation->root_instruction();
  auto loc = mlir::NameLoc::get(builder.getStringAttr(root->name()));
  ImplicitLocOpBuilder b(loc, builder);

  // Assumptions we make about the matcher:
  //   * matches Softmax "diamonds" on the last axis, along with any number of
  //     elementwise operations/bitcasts on any edge
  //   * within a given fusion, every argument to a Softmax diamond has the same
  //     shape
  //   * every reduction is on the last axis
  //   * the last axis of every reduction parameter has the same length
  //   * reductions only reduce a single operand
  //   * all the shapes have canonical layout (logical layout = physical layout)
  //   * the computation has a single output
  //   * we tile along a single dimension

  const HloInstruction* reduce = hlo_query::GetFirstInstructionWithOpcode(
      *computation, HloOpcode::kReduce);

  if (reduce == nullptr) {
    return absl::InvalidArgumentError("No reduce instruction found.");
  }

  const Shape& reduce_input_shape = reduce->operand(0)->shape();

  if (reduce->dimensions().size() != 1 ||
      reduce->dimensions(0) != reduce_input_shape.rank() - 1) {
    return absl::InvalidArgumentError(
        absl::StrCat("Reduce instruction must reduce inner-most dimension. ",
                     reduce->ToString()));
  }

  const Shape& root_shape = computation->root_instruction()->shape();
  if (!root_shape.IsArray() ||
      LayoutUtil::IsMonotonicWithDim0Minor(root_shape.layout())) {
    return absl::InvalidArgumentError(
        absl::StrCat("Root shape is not supported. ", root_shape.ToString()));
  }

  int row_len = reduce_input_shape.dimensions_minor(0);

  Value pid = b.create<ma::IndexCastUIOp>(
      b.getIndexType(), b.create<mt::GetProgramIdOp>(mt::ProgramIDDim::X));

  std::vector<int64_t> output_tile_sizes(
      computation->root_instruction()->shape().rank(), 1);
  output_tile_sizes.back() = row_len;

  TF_ASSIGN_OR_RETURN(
      std::vector<std::unique_ptr<TiledHloInstruction>> tiled_hlo_instructions,
      analysis->ComputeTiledHloInstructions(output_tile_sizes));

  // block_size must be a power of two.
  int result_block_size = llvm::PowerOf2Ceil(row_len);

  std::vector<int32_t> boundary_checks;
  if (result_block_size != row_len) {
    boundary_checks.push_back(0);
  }

  // Emits load instructions
  auto emit_param_load =
      [&](const TiledHloInstruction& tiled_hlo) -> absl::StatusOr<Value> {
    std::vector<Value> tile_sizes, tile_strides, tile_offsets;
    for (auto [size, stride] :
         llvm::zip(tiled_hlo.tile_sizes(), tiled_hlo.tile_strides())) {
      if (size == 1) continue;

      tile_sizes.push_back(CreateConst(b, b.getI64Type(), size));
      tile_strides.push_back(CreateConst(b, b.getI64Type(), stride));
      tile_offsets.push_back(CreateConst(b, b.getI32Type(), 0));
    }

    // Manually compute pointer offset to avoid materialized fully parallel
    // dimensions in the tile. Current codegen tried to avoid size-1 dims.
    Value ptr_offset = ComputeBasePtrOffset(b, pid, tiled_hlo);

    auto fn_arg = fn.getArgument(tiled_hlo.hlo()->parameter_number());
    auto tile_ptr = AddPtr(b, fn_arg, ptr_offset);

    if (tile_sizes.empty()) {
      return EmitParameterLoad(b, tile_ptr, boundary_checks);
    }

    Value emitted_tensor = b.create<mt::MakeTensorPtrOp>(
        /*base=*/tile_ptr,
        /*shape=*/tile_sizes,
        /*strides=*/tile_strides,
        /*offsets=*/tile_offsets,
        /*tensorShape=*/std::vector<int32_t>{result_block_size},
        /*order=*/std::vector<int32_t>{0});

    return EmitParameterLoad(b, emitted_tensor, boundary_checks);
  };

  absl::flat_hash_map<const TiledHloInstruction*, Value> values_out;
  TF_ASSIGN_OR_RETURN(
      Value result,
      EmitTiledScope(b, libdevice_path, device_info, tiled_hlo_instructions,
                     emit_param_load, values_out));

  Value ptr_offset =
      ComputeBasePtrOffset(b, pid, *tiled_hlo_instructions.back());

  Value store_tensor = b.create<mt::MakeTensorPtrOp>(
      /*base=*/AddPtr(b, fn.getArgument(computation->num_parameters()),
                      ptr_offset),
      /*shape=*/ValueRange{CreateConst(b, b.getI64Type(), row_len)},
      /*strides=*/ValueRange{CreateConst(b, b.getI64Type(), 1)},
      /*offsets=*/ValueRange{CreateConst(b, b.getI32Type(), 0)},
      /*tensorShape=*/std::vector<int32_t>{result_block_size},
      /*order=*/std::vector<int32_t>{0});

  b.create<mt::StoreOp>(store_tensor, result, std::vector<int32_t>{0},
                        mt::CacheModifier::NONE, mt::EvictionPolicy::NORMAL);

  return absl::OkStatus();
}

absl::Status EmitSoftMax(mlir::OpBuilder builder,
                         absl::string_view libdevice_path,
                         const se::DeviceDescription& device_info,
                         const TritonFusionAnalysis& analysis,
                         const HloComputation* computation,
                         mlir::triton::FuncOp fn,
                         const TritonGemmConfig& config) {
  SymbolicTileAnalysisOrError symbolic_tile_analysis_or =
      SymbolicTileAnalysis::AnalyzeComputation(*computation,
                                               builder.getContext());
  if (auto* symbolic_tile_analysis =
          std::get_if<SymbolicTileAnalysis>(&symbolic_tile_analysis_or)) {
    return EmitTiledSoftMax(builder, libdevice_path, device_info,
                            symbolic_tile_analysis, computation, fn);
  }

  const HloInstruction* root = computation->root_instruction();
  auto loc = mlir::NameLoc::get(builder.getStringAttr(root->name()));
  ImplicitLocOpBuilder b(loc, builder);

  // Assumptions we make about the matcher:
  //   * matches Softmax "diamonds" on the last axis, along with any number of
  //     elementwise operations/bitcasts on any edge
  //   * within a given fusion, every argument to a Softmax diamond has the same
  //     shape
  //   * every reduction is on the last axis
  //   * the last axis of every reduction parameter has the same length
  //   * reductions only reduce a single operand
  //   * all the shapes have canonical layout (logical layout = physical layout)
  //   * the computation has a single output
  //   * we tile along a single dimension

  // TODO(bchetioui): allow doing several rows per block (e.g. for when rows
  // are smaller than the minimum transaction size)

  const HloInstruction* reduce = hlo_query::GetFirstInstructionWithOpcode(
      *computation, HloOpcode::kReduce);

  TF_RET_CHECK(reduce != nullptr);

  Shape reduce_input_shape = reduce->operand(0)->shape();

  TF_RET_CHECK(reduce->opcode() == HloOpcode::kReduce);
  TF_RET_CHECK(reduce->dimensions().size() == 1);
  TF_RET_CHECK(reduce->dimensions()[0] == reduce_input_shape.rank() - 1);

  int row_len = reduce_input_shape.dimensions_minor(0);

  Value pid = b.create<ma::ExtSIOp>(
      b.getI64Type(), b.create<mt::GetProgramIdOp>(mt::ProgramIDDim::X));
  Value row_stride = CreateConst(b, b.getI32Type(), row_len);

  Value row_offset = b.create<ma::MulIOp>(
      pid, b.create<ma::ExtSIOp>(b.getI64Type(), row_stride));
  Value zero_offset = CreateConst(b, b.getI64Type(), 0);

  absl::flat_hash_map<const HloInstruction*, Value> values_out;
  std::vector<int32_t> boundary_checks;

  // block_size must be a power of two.
  int result_block_size = pow(2, ceil(log(row_len) / log(2)));

  if (result_block_size != row_len) {
    boundary_checks.push_back(0);
  }

  // Emits load instructions
  for (int param_idx = 0; param_idx < computation->num_parameters();
       ++param_idx) {
    HloInstruction* param = computation->parameter_instruction(param_idx);
    // Current tiling derivation assigns index 0 to the reduction dimension and
    // index 1 to the batch dimension.
    auto reduce_iterspec = analysis.IterSpec(
        TritonFusionAnalysis::Scope::OUTPUT, param, /*dimension=*/0);
    auto batch_iterspec = analysis.IterSpec(TritonFusionAnalysis::Scope::OUTPUT,
                                            param, /*dimension=*/1);

    // Make sure only batch and reduce dims are present in tiling
    TF_RET_CHECK(analysis.IterSpec(TritonFusionAnalysis::Scope::OUTPUT, param,
                                   /*dimension=*/2) == nullptr);

    if (!reduce_iterspec) {
      // This parameter's broadcast is along the reduce dimension, and so
      // each pid uses and broadcasts its own index.

      // If batchDimIterSpec is also not present, then this parameter is a
      // scalar, in which case we reuse this for each pid with offset.
      Value batch_offset = batch_iterspec ? pid : zero_offset;

      values_out[param] = EmitParameterLoad(
          b, AddPtr(b, fn.getArgument(param_idx), batch_offset),
          boundary_checks);
      continue;
    }

    TF_RET_CHECK(reduce_iterspec != nullptr);
    TF_RET_CHECK(reduce_iterspec->size() == 1);

    // TODO(b/310721908): The below assumes that we tile along a single dim.
    int reduce_dim_len = reduce_iterspec->front().count;
    int reduce_dim_stride = reduce_iterspec->front().stride;
    int slice_offset = reduce_iterspec->front().slice_start;

    // If the batch dimension is present in this parameter's tile, we must make
    // sure each batch idx is offset by the correct number of rows. If it is not
    // present, then the reduce dim data is reused without any offset.
    Value base_offset = batch_iterspec ? row_offset : zero_offset;

    // We assume that the reduced axis of this parameter has length row_len.
    // TODO(b/316637896): Relax assumption that param reduce_dim_len == row_len.
    TF_RET_CHECK(reduce_dim_len == row_len);

    // block_size must be a power of two.
    int block_size = pow(2, ceil(log(reduce_dim_len) / log(2)));

    // Verify that this param contains a single contiguous fragment.
    TF_RET_CHECK(reduce_iterspec->front().subfragments.size() == 1);

    Value emitted_tensor = b.create<mt::MakeTensorPtrOp>(
        /*base=*/AddPtr(b, fn.getArgument(param_idx), base_offset),
        /*shape=*/ValueRange{CreateConst(b, b.getI64Type(), reduce_dim_len)},
        /*strides=*/
        ValueRange{CreateConst(b, b.getI64Type(), reduce_dim_stride)},
        /*offsets=*/ValueRange{CreateConst(b, b.getI32Type(), slice_offset)},
        /*tensorShape=*/std::vector<int32_t>{block_size},
        /*order=*/std::vector<int32_t>{0});

    values_out[param] = EmitParameterLoad(b, emitted_tensor, boundary_checks);
  }

  // Dimension 0 is the reduced one by construction and it's the only one
  // present in the tile shapes.
  std::vector<DimProperties> tiled_dims = {DimProperties(
      /*index=*/0, pid, result_block_size, /*split_value=*/1)};
  TF_ASSIGN_OR_RETURN(
      Value result,
      EmitScope(b, libdevice_path, device_info, &analysis,
                TritonFusionAnalysis::Scope::OUTPUT, tiled_dims,
                computation->MakeInstructionPostOrder(), values_out));

  Value store_tensor = b.create<mt::MakeTensorPtrOp>(
      /*base=*/AddPtr(b, fn.getArgument(computation->num_parameters()),
                      row_offset),
      /*shape=*/ValueRange{CreateConst(b, b.getI64Type(), row_len)},
      /*strides=*/ValueRange{CreateConst(b, b.getI64Type(), 1)},
      /*offsets=*/ValueRange{CreateConst(b, b.getI32Type(), 0)},
      /*tensorShape=*/std::vector<int32_t>{result_block_size},
      /*order=*/std::vector<int32_t>{0});

  b.create<mt::StoreOp>(store_tensor, result, std::vector<int32_t>{0},
                        mt::CacheModifier::NONE, mt::EvictionPolicy::NORMAL);
  return absl::OkStatus();
}

// Simplified copy of translateLLVMToLLVMIR which in addition takes
// path to libdevice directly as an argument.
absl::StatusOr<std::unique_ptr<llvm::Module>> TranslateLLVMToLLVMIR(
    llvm::LLVMContext* llvmContext, mlir::ModuleOp module,
    absl::string_view libdevice_path) {
  mlir::DialectRegistry registry;
  mlir::registerBuiltinDialectTranslation(registry);
  mlir::registerLLVMDialectTranslation(registry);
  mlir::registerNVVMDialectTranslation(registry);
  mlir::registerROCDLDialectTranslation(registry);
  module->getContext()->appendDialectRegistry(registry);

  std::unique_ptr<llvm::Module> llvmModule =
      mlir::translateModuleToLLVMIR(module, *llvmContext);
  if (!llvmModule) {
    return Internal("Failed to emit LLVM IR.");
  }

  // Link external libraries before performing optimizations.
  TF_RETURN_IF_ERROR(nvptx::LinkLibdeviceIfNecessary(
      llvmModule.get(), std::string(libdevice_path)));

  auto optPipeline = mlir::makeOptimizingTransformer(
      /*optLevel=*/3, /*sizeLevel=*/0,
      /*targetMachine=*/nullptr);

  if (auto err = optPipeline(llvmModule.get())) {
    llvm::errs() << err;
    return Internal("Failed to optimize LLVM IR.");
  }

  return llvmModule;
}

absl::StatusOr<mlir::OwningOpRef<mlir::ModuleOp>> CreateTritonModule(
    const TritonFusionAnalysis& analysis, absl::string_view fn_name,
    const HloComputation* hlo_computation,
    const se::DeviceDescription& device_info, const TritonGemmConfig& config,
    TritonIrEmitter ir_emitter, mlir::MLIRContext& mlir_context) {
  mlir_context
      .loadDialect<mt::TritonDialect, mt::gpu::TritonGPUDialect,
                   mlir::arith::ArithDialect, mlir::affine::AffineDialect>();

  mlir::OpBuilder b(&mlir_context);
  auto loc = mlir::NameLoc::get(b.getStringAttr(hlo_computation->name()));
  mlir::OwningOpRef<mlir::ModuleOp> triton_module =
      llvm_ir::CreateMlirModuleOp(loc);
  b.setInsertionPointToEnd(triton_module->getBody());

  // Build Triton kernel.
  SmallVector<Type> fn_arg_types;
  for (HloInstruction* p : hlo_computation->parameter_instructions()) {
    PrimitiveType type = p->shape().element_type();
    Type ir_type = type != U16 ? TritonType(b, type) : b.getI16Type();
    fn_arg_types.push_back(
        mt::PointerType::get(StorageType(b, ir_type), mn::kGlobalMemorySpace));
  }

  for (const ShapeUtil::IndexedShape& s :
       ShapeUtil::GetLeafShapes(hlo_computation->root_instruction()->shape())) {
    fn_arg_types.push_back(mt::PointerType::get(
        StorageType(b, TritonType(b, s.shape.element_type())),
        mn::kGlobalMemorySpace));
  }

  auto fn = b.create<mt::FuncOp>(loc, fn_name,
                                 b.getFunctionType(fn_arg_types, std::nullopt));
  for (int i = 0; i < fn.getNumArguments(); ++i) {
    fn.setArgAttr(i, "tt.divisibility", b.getIntegerAttr(b.getI32Type(), 16));
  }
  fn.addEntryBlock();
  b.setInsertionPointToStart(&fn.front());

  TF_RETURN_IF_ERROR(ir_emitter(
      b, GetLibdevicePath(hlo_computation->parent()->config(), device_info),
      device_info, analysis, hlo_computation, fn, config));

  b.create<mt::ReturnOp>(loc);

  mlir::PassManager pm(&mlir_context);
  pm.addPass(mlir::createCanonicalizerPass());
  pm.addPass(mlir::createCSEPass());
  TF_RET_CHECK(pm.run(triton_module.get()).succeeded());

  VLOG(6) << llvm_ir::DumpToString(*triton_module);
  if (DumpingEnabledForHloModule(*hlo_computation->parent())) {
    DumpToFileInDirOrStdout(*hlo_computation->parent(), "triton_ir", "ttir",
                            llvm_ir::DumpToString(*triton_module));
  }

  TF_RET_CHECK(mlir::succeeded(mlir::verify(*triton_module)));
  return std::move(triton_module);
}

absl::StatusOr<TritonWrapperResult> TritonWrapper(
    const TritonFusionAnalysis& analysis, absl::string_view fn_name,
    const HloComputation* hlo_computation, const se::GpuComputeCapability& cc,
    const se::DeviceDescription& device_info, const TritonGemmConfig& config,
    llvm::Module* llvm_module, TritonIrEmitter ir_emitter,
    mlir::MLIRContext& mlir_context) {
  if (std::holds_alternative<se::CudaComputeCapability>(cc)) {
    auto ccCuda = std::get<se::CudaComputeCapability>(cc);
    if (!ccCuda.IsAtLeastAmpere()) {
      return absl::FailedPreconditionError(
          "Triton support is only enabled for Ampere GPUs and up.");
    }
  }

  auto debug_options = GetDebugOptionsFromFlags();
  if (debug_options.xla_gpu_enable_triton_hopper()) {
    // Set environment variables for consumption by Triton.
    tsl::setenv("ENABLE_MMA_V3", "true", true /*overwrite*/);
  }

  TF_ASSIGN_OR_RETURN(
      auto triton_module,
      CreateTritonModule(analysis, fn_name, hlo_computation, device_info,
                         config, ir_emitter, mlir_context));

  VLOG(3) << hlo_computation->ToString(HloPrintOptions::ShortParsable());
  VLOG(2) << config.ToString();

  // Compile Triton kernel to LLVM.
  const HloModule* hlo_module = hlo_computation->parent();
  return CompileTritonToLLVM(hlo_module->config(), hlo_module->name(), cc,
                             device_info, config, triton_module.get(),
                             llvm_module, mlir_context);
}

// TODO(b/325220878): Replace TritonGemmConfig with a more generic abstraction.
absl::StatusOr<TritonWrapperResult> CompileTritonToLLVM(
    const HloModuleConfig& hlo_config, absl::string_view hlo_module_name,
    const se::GpuComputeCapability& cc,
    const se::DeviceDescription& device_info, const TritonGemmConfig& config,
    mlir::ModuleOp triton_module, llvm::Module* llvm_module,
    mlir::MLIRContext& mlir_context) {
  if (std::holds_alternative<se::CudaComputeCapability>(cc)) {
    auto ccCuda = std::get<se::CudaComputeCapability>(cc);
    if (!ccCuda.IsAtLeastAmpere()) {
      return absl::FailedPreconditionError(
          "Triton support is only enabled for Ampere GPUs and up.");
    }
  }

  bool should_verify =
      (hlo_config.debug_options().xla_gpu_llvm_verification_level() >= 1);
#ifndef NDEBUG
  should_verify = true;
#endif

  mlir::PassManager pm(&mlir_context);
  pm.enableVerifier(should_verify);

  std::optional<llvm::raw_fd_ostream> log_stream;
  if (hlo_config.debug_options().xla_gpu_dump_llvmir()) {
    const std::string basename =
        absl::StrCat(absl::string_view(tsl::io::Basename(hlo_module_name)),
                     ".triton-passes.log");
    std::string outputs_dir;
    if (!tsl::io::GetTestUndeclaredOutputsDir(&outputs_dir)) {
      outputs_dir = hlo_config.debug_options().xla_dump_to();
    }
    if (!outputs_dir.empty()) {
      std::string path = tsl::io::JoinPath(outputs_dir, basename);
      std::error_code err;
      log_stream.emplace(path, err, llvm::sys::fs::OF_None);
      if (err) {
        log_stream.reset();
        LOG(ERROR) << err.message();
      } else {
        pm.getContext()->disableMultithreading();
        auto print_always = [](mlir::Pass*, mlir::Operation*) { return true; };
        pm.enableIRPrinting(/*shouldPrintBeforePass=*/print_always,
                            /*shouldPrintAfterPass=*/print_always,
                            /*printModuleScope=*/true,
                            /*printAfterOnlyOnChange=*/false,
                            /*printAfterOnlyOnFailure=*/true, *log_stream,
                            /*opPrintingFlags=*/{});
      }
    } else {
      LOG(ERROR) << "--xla_gpu_dump_llvmir is set, but neither the environment "
                 << "variable TEST_UNDECLARED_OUTPUTS_DIR nor the flag "
                 << "--xla_dump_to is set, so the llvm dumps are disabled.";
    }
  }

  // Lower affine expressions into arithmetic ops.
  pm.addPass(mlir::createLowerAffinePass());

  mlir::triton::nvidia_gpu::ClusterInfo cluster_info;
  if (!CreateTritonPipeline(pm, cc, config, /*out*/ cluster_info).ok()) {
    return Internal("Failed to create Triton pipeline.");
  }
  if (log_stream.has_value()) {
    pm.printAsTextualPipeline(log_stream.value());
    log_stream->write("\n\n", 2);
  }
  // Triton generates pointers to the global address space, while XLA needs a
  // kernel signature with pointers to the generic address space.
  pm.addPass(std::make_unique<GeneralizeKernelSignaturePass>());
  // llvm::Linker::linkModules() segfaults if we don't strip locations.
  pm.addPass(mlir::createStripDebugInfoPass());

  bool succeeded = mlir::succeeded(pm.run(triton_module));

  if (log_stream.has_value()) {
    log_stream->flush();
  }

  if (!succeeded) {
    return Internal("Failed to compile Triton kernel.");
  }

  const int shared_mem_bytes =
      triton_module->getAttrOfType<mlir::IntegerAttr>("triton_gpu.shared")
          .getInt();
  VLOG(2) << "Shared memory usage: " << shared_mem_bytes << " B";
  if (std::holds_alternative<se::CudaComputeCapability>(cc) &&
      shared_mem_bytes > device_info.shared_memory_per_block_optin()) {
    return absl::ResourceExhaustedError(absl::StrFormat(
        "Shared memory size limit exceeded: requested %d, available: %d",
        shared_mem_bytes, device_info.shared_memory_per_block_optin()));
  }

  TF_ASSIGN_OR_RETURN(
      std::unique_ptr<llvm::Module> ll_triton_module,
      TranslateLLVMToLLVMIR(&llvm_module->getContext(), triton_module,
                            GetLibdevicePath(hlo_config, device_info)));
  VLogModule(5, *ll_triton_module);
  if (should_verify) {
    VerifyModule(*ll_triton_module);
  }

  // Integrate LLVM matmul kernel into XLA's LLVM module.
  ll_triton_module->eraseNamedMDNode(
      ll_triton_module->getNamedMetadata("nvvm.annotations"));
  ll_triton_module->setDataLayout(llvm_module->getDataLayout());
  ll_triton_module->setTargetTriple(llvm_module->getTargetTriple());
  // Use override flag because libdevice functions can be present in both.
  TF_RET_CHECK(
      !llvm::Linker::linkModules(*llvm_module, std::move(ll_triton_module),
                                 llvm::Linker::Flags::OverrideFromSrc));
  VLogModule(5, *llvm_module);
  if (should_verify) {
    VerifyModule(*llvm_module);
  }

  // `cluster_info` must be read after pm.run().
  std::optional<se::ClusterDim> cluster_dim;
  if (config.num_ctas > 1) {
    VLOG(3) << "num_ctas: " << config.num_ctas
            << ", cluster_info: " << cluster_info.clusterDimX << ","
            << cluster_info.clusterDimY << "," << cluster_info.clusterDimZ;
    if (cluster_info.clusterDimX > 1 || cluster_info.clusterDimY > 1 ||
        cluster_info.clusterDimZ > 1) {
      cluster_dim =
          se::ClusterDim(cluster_info.clusterDimX, cluster_info.clusterDimY,
                         cluster_info.clusterDimZ);
    }
  } else {
    TF_RET_CHECK(cluster_info.clusterDimX == 1 &&
                 cluster_info.clusterDimY == 1 &&
                 cluster_info.clusterDimZ == 1);
  }
  return {{shared_mem_bytes, cluster_dim}};
}

}  // namespace gpu
}  // namespace xla
