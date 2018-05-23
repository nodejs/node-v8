// Copyright 2018 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/constant-folding-reducer.h"

#include "src/compiler/js-graph.h"
#include "src/objects-inl.h"

namespace v8 {
namespace internal {
namespace compiler {

ConstantFoldingReducer::ConstantFoldingReducer(Editor* editor, JSGraph* jsgraph)
    : AdvancedReducer(editor), jsgraph_(jsgraph) {}

ConstantFoldingReducer::~ConstantFoldingReducer() {}

Reduction ConstantFoldingReducer::Reduce(Node* node) {
  // Check if the output type is a singleton.  In that case we already know the
  // result value and can simply replace the node if it's eliminable.
  if (!NodeProperties::IsConstant(node) && NodeProperties::IsTyped(node) &&
      node->op()->HasProperty(Operator::kEliminatable)) {
    // TODO(v8:5303): We must not eliminate FinishRegion here. This special
    // case can be removed once we have separate operators for value and
    // effect regions.
    if (node->opcode() == IrOpcode::kFinishRegion) return NoChange();
    // We can only constant-fold nodes here, that are known to not cause any
    // side-effect, may it be a JavaScript observable side-effect or a possible
    // eager deoptimization exit (i.e. {node} has an operator that doesn't have
    // the Operator::kNoDeopt property).
    Type upper = NodeProperties::GetType(node);
    if (!upper.IsNone()) {
      Node* replacement = nullptr;
      if (upper.IsHeapConstant()) {
        replacement = jsgraph()->Constant(upper.AsHeapConstant()->Value());
      } else if (upper.Is(Type::MinusZero())) {
        Factory* factory = jsgraph()->isolate()->factory();
        replacement = jsgraph()->Constant(factory->minus_zero_value());
      } else if (upper.Is(Type::NaN())) {
        replacement = jsgraph()->NaNConstant();
      } else if (upper.Is(Type::Null())) {
        replacement = jsgraph()->NullConstant();
      } else if (upper.Is(Type::PlainNumber()) && upper.Min() == upper.Max()) {
        replacement = jsgraph()->Constant(upper.Min());
      } else if (upper.Is(Type::Undefined())) {
        replacement = jsgraph()->UndefinedConstant();
      }
      if (replacement) {
        // Make sure the node has a type.
        if (!NodeProperties::IsTyped(replacement)) {
          NodeProperties::SetType(replacement, upper);
        }
        ReplaceWithValue(node, replacement);
        return Changed(replacement);
      }
    }
  }
  return NoChange();
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8
