#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "mlir-hlo/Dialect/mhlo/transforms/PassDetail.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"  // TF:llvm-project
#include "mlir/IR/MLIRContext.h"              // TF:llvm-project
#include "mlir/Pass/Pass.h"                   // TF:local_config_mlir
#include "tensorflow/compiler/mlir/hlo/include/mlir-hlo/Dialect/mhlo/IR/hlo_ops.h"
#include "tensorflow/compiler/mlir/hlo/include/mlir-hlo/utils/hlo_utils.h"

namespace mlir {

using hlo::kCpu;
using hlo::kDiscShapeCalcAttr;
using hlo::kGpu;
using hlo::PlacementType;

namespace mhlo {
namespace {

// Check Op if it is a mhlo Op.
bool isMhloDialect(Operation* op) {
  return (op->getDialect() ==
          op->getContext()->getLoadedDialect<mhlo::MhloDialect>());
}

// This pass explicitly marks the shape calculating Op by adding an Attr. Nested
// FuncOps should be taken into consideration.
// Following Ops are shape Ops:
//  - i64 Scalar output
//  - Shape Op's operands
//  - Shape operands according to kShapeCalcOperandMap
// Following Ops regard as shape Ops:
//  - GetDimensionSizeOp, PrintOp
//  - ConstOp, SelectOp, IotaOp, DynamicIotaOp if type is i32
//  - mhlo.dynamic_gather and mhlo.gather if operand_0's type is i32
//  - Date operands but type is i32 according to kShapeCalcOperandMap
struct MarkShapeCalc : public MarkShapeCalculationPassBase<MarkShapeCalc> {
 public:
  using MarkShapeCalculationPassBase<
      MarkShapeCalc>::MarkShapeCalculationPassBase;

  MarkShapeCalc() = default;
  MarkShapeCalc(const MarkShapeCalc& o) {}

  StringRef getArgument() const final { return "mhlo-disc-mark-shape-calc"; }

  void runOnOperation() override;

 private:
  // Mark shape calculation subgraph
  void MarkShapeCalcOps();

  // Regard any mhlo Ops that calculates I32 as shape calculation Ops
  void MarkRegardAsShapeCalcOps();

  // for rule based placement strategy, the placement of the op in the list
  // is up to the placement of the dominant operand
  const DenseMap<TypeID, /*dominant operand index*/ int> kPlaceRuleMap = {
      {TypeID::get<DynamicGatherOp>(), /*operand*/ 0},
      {TypeID::get<GatherOp>(), /*operand*/ 0}};

  const DenseMap<TypeID, SmallVector<int, 3>> kShapeCalcOperandMap = {
      {TypeID::get<RealDynamicSliceOp>(),
       {/*start_indices*/ 1, /*limit_indices*/ 2, /*strides*/ 3}},
      {TypeID::get<DynamicPadOp>(),
       {/*edge_padding_low*/ 2, /*edge_padding_high*/ 3,
        /*interior_padding*/ 4}},
      {TypeID::get<DynamicReshapeOp>(), {/*shape*/ 1}},
      {TypeID::get<DynamicIotaOp>(), {/*shape*/ 0}},
      {TypeID::get<DynamicBroadcastInDimOp>(), {/*out_dim_size*/ 1}},
      {TypeID::get<DynamicGatherOp>(), {/*slice_sizes*/ 2}},
      {TypeID::get<DynamicConvOp>(), {/*paddings*/ 2}},
      {TypeID::get<IfOp>(), {/*pred*/ 0}}};

  // add output OP into marked set if it is a I64 scalar and placment is CPU.
  void markI64ReturnedCpuScalarOps(FuncOp func,
                                   DenseSet<Operation*>& marked_ops);
  // Update marked set.
  // If a OP is in marked set, add all of its operands to marked set.
  // Add some operands of dynamic shape OPs into marked set according to lookup
  // table.
  void markShapeCalculationOps(FuncOp func, DenseSet<Operation*>& marked_ops);

  // Get placement vector of func's output.
  SmallVector<llvm::StringRef, 4> getOutputPlacements(FuncOp main_func);

  // Get Op's placement according to its attr.
  PlacementType getOpPlacement(Operation* op);
};

void MarkShapeCalc::runOnOperation() {
  // Mark shape calculation subgraph
  MarkShapeCalcOps();

  // Mark any mhlo Ops that calculates I32 as shape calculation Ops
  MarkRegardAsShapeCalcOps();
};

// Mark the Ops that is the producer of any shape operands
// TODO(disc): handle when TupleOp exists in shape_calc_ops
void MarkShapeCalc::MarkShapeCalcOps() {
  ModuleOp module = getOperation();
  Builder builder(&getContext());
  llvm::DenseSet<Operation*> shape_calc_ops;

  module.walk([&](FuncOp func) {
    // Mark the i64 Scalar output as shape calculation Op.
    // TODO(disc): revisit this if we have outputs on CPU for TF in the future.
    if (func.getName() == "main") {
      markI64ReturnedCpuScalarOps(func, shape_calc_ops);
    }
    // Skip if this function is external
    if (func.isExternal()) return;
    // no target ops
    if (llvm::none_of(func.getBlocks().front(),
                      [](Operation& op) { return isMhloDialect(&op); })) {
      return;
    }
    markShapeCalculationOps(func, shape_calc_ops);
  });

  for (Operation* op : shape_calc_ops) {
    // We suppose that mhlo op only has single output, either having tensor
    // type or tuple type.
    if (auto tp = op->getResult(0).getType().dyn_cast<TupleType>()) {
      // If an op is placed on cpu, then we suppose all its outputs are
      // placed on cpu.
      SmallVector<Attribute, 4> attrs(tp.size(), builder.getBoolAttr(true));
      op->setAttr(kDiscShapeCalcAttr, ArrayAttr::get(tp.getContext(), attrs));
    } else {
      op->setAttr(kDiscShapeCalcAttr, builder.getBoolAttr(true));
    }
  }
}

// Regard any mhlo Ops that calculates i32 as shape Ops. This is an rule based
// optimization that mimicking the behavior of tensorflow
void MarkShapeCalc::MarkRegardAsShapeCalcOps() {
  ModuleOp module = getOperation();
  Builder builder(&getContext());

  module.walk([&](Operation* op) {
    if (!isMhloDialect(op)) return;

    if (isa<mhlo::TupleOp, mhlo::GetTupleElementOp, mhlo::WhileOp, mhlo::IfOp,
            mhlo::ReturnOp>(op)) {
      return;
    }
    // Skip the Op that is already marked shape Op
    auto attr = op->getAttrOfType<BoolAttr>(kDiscShapeCalcAttr);
    if ((attr != nullptr) && (attr.getValue() == true)) return;

    if (isa<mhlo::GetDimensionSizeOp, mhlo::PrintOp>(op)) {
      op->setAttr(kDiscShapeCalcAttr, builder.getBoolAttr(true));
      return;
    }

    // Ops that only cares about the output element type
    if (isa<mhlo::ConstOp, mhlo::SelectOp, mhlo::IotaOp, mhlo::DynamicIotaOp>(
            op)) {
      auto result_ty = op->getResult(0).getType().dyn_cast<RankedTensorType>();
      assert(result_ty && "unexpected non ranked type for ConstOp");
      auto elem_type = result_ty.getElementType();
      if (elem_type.isInteger(32)) {
        op->setAttr(kDiscShapeCalcAttr, builder.getBoolAttr(true));
      }
      return;
    }

    auto op_type_id = op->getAbstractOperation()->typeID;
    bool is_shape_calc_op = false;
    // Follow the rule of kPlaceRuleMap exist, or else follow
    // kShapeCalcOperandMap
    auto it = kPlaceRuleMap.find(op_type_id);
    if (it != kPlaceRuleMap.end()) {
      auto dominant_idx = it->second;
      auto operand_ty =
          op->getOperand(dominant_idx).getType().dyn_cast<RankedTensorType>();
      assert(operand_ty && "unexpected non unranked type of operand");
      if (operand_ty.getElementType().isInteger(32)) {
        is_shape_calc_op = true;
      }
    } else {
      SmallVector<int, 3> shape_operand_indices;
      auto iter = kShapeCalcOperandMap.find(op_type_id);
      if (iter != kShapeCalcOperandMap.end()) {
        shape_operand_indices = iter->second;
      }
      for (int idx : shape_operand_indices) {
        auto operand_ty =
            op->getOperand(idx).getType().dyn_cast<RankedTensorType>();
        if (!operand_ty) continue;
        auto elem_type = operand_ty.getElementType();
        if (elem_type.isInteger(32)) {
          is_shape_calc_op = true;
          break;
        }
      }
    }
    // Set attr if it is a shape Op
    if (is_shape_calc_op) {
      if (auto tp = op->getResult(0).getType().dyn_cast<TupleType>()) {
        SmallVector<Attribute, 4> attrs(tp.size(), builder.getBoolAttr(true));
        op->setAttr(kDiscShapeCalcAttr, ArrayAttr::get(tp.getContext(), attrs));
      } else {
        op->setAttr(kDiscShapeCalcAttr, builder.getBoolAttr(true));
      }
    }
    return;
  });
}

SmallVector<llvm::StringRef, 4> MarkShapeCalc::getOutputPlacements(
    FuncOp main_func) {
  auto dict_attr =
      main_func->getAttrOfType<DictionaryAttr>("tf.entry_function");
  assert(dict_attr && "main_func must has tf.entry_function attr");
  auto output_placements_attr = dict_attr.get(hlo::kOutputPlacementAttr);
  SmallVector<StringRef, 4> output_placements;
  if (!output_placements_attr) {
    // No placement attr is specified, thus using the inferred placement.
    return output_placements;
  }

  output_placements_attr.cast<mlir::StringAttr>().getValue().split(
      output_placements, ',', /*MaxSplit=*/-1, /*KeepEmpty=*/false);
  return output_placements;
}

void MarkShapeCalc::markI64ReturnedCpuScalarOps(
    FuncOp func, llvm::DenseSet<Operation*>& marked_ops) {
  assert(func.getName() == "main");
  auto return_op = func.front().getTerminator();
  if (!isa<mlir::ReturnOp>(return_op)) return;

  auto result_attrs = func.getAllResultAttrs();
  const auto& output_placements = getOutputPlacements(func);
  auto returned_ops = return_op->getOperands();
  assert(returned_ops.size() == output_placements.size());
  for (auto output : llvm::enumerate(returned_ops)) {
    auto idx = output.index();
    auto op = output.value().getDefiningOp();
    if (!op) continue;

    if (!isMhloDialect(op)) continue;

    if (auto type = op->getResult(0).getType().dyn_cast<RankedTensorType>()) {
      if ((output_placements[idx] == kCpu) &&
          type.getElementType().isInteger(64) && (type.getRank() == 0)) {
        marked_ops.insert(op);
      }
    }
  }
}

void MarkShapeCalc::markShapeCalculationOps(
    FuncOp func, llvm::DenseSet<Operation*>& marked_ops) {
  auto& block = func.getBlocks().front();
  for (Operation& op : block) {
    if (!isMhloDialect(&op)) return;

    // If the op is already in shape calculation op set, insert all of its
    // operands into shape calculation op set
    if (marked_ops.contains(&op)) {
      for (auto operand_value : op.getOperands()) {
        Operation* operand = operand_value.getDefiningOp();
        if (operand == nullptr) continue;
        if (!isMhloDialect(operand)) {
          continue;
        }
        marked_ops.insert(operand);
      }
    }
    // Mark operands into shape calculation set according to the lookup table.
    if (!marked_ops.contains(&op)) {
      auto op_type_id = op.getAbstractOperation()->typeID;
      auto iter = kShapeCalcOperandMap.find(op_type_id);
      if (iter != kShapeCalcOperandMap.end()) {
        for (auto operand_idx : iter->second) {
          auto operand = op.getOperand(operand_idx).getDefiningOp();
          if (operand == nullptr) continue;
          if (!isMhloDialect(operand)) {
            continue;
          }
          marked_ops.insert(operand);
        }
      }
    }
    // TODO(disc): If the operand of the op is a nested FuncOp, mark the
    // associated producer in the nested FuncOp
  };
}

PlacementType MarkShapeCalc::getOpPlacement(Operation* op) {
  auto attr = op->getAttrOfType<StringAttr>(hlo::kDiscShapeCalcAttr);
  if ((attr != nullptr) && (attr.getValue() == kCpu)) {
    return PlacementType::kCpu;
  }
  return PlacementType::kGpu;
}

}  // namespace

std::unique_ptr<OperationPass<ModuleOp>> createMarkShapeCalcOpPass() {
  return std::make_unique<MarkShapeCalc>();
}

}  // namespace mhlo
}  // namespace mlir
