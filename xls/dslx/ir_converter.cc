// Copyright 2020 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/dslx/ir_converter.h"

#include "absl/strings/str_replace.h"
#include "absl/types/variant.h"
#include "xls/dslx/cpp_ast.h"
#include "xls/dslx/deduce_ctx.h"
#include "xls/dslx/dslx_builtins.h"
#include "xls/ir/lsb_or_msb.h"

namespace xls::dslx {

using IrOp = xls::Op;
using IrLiteral = xls::Value;

/* static */ std::string IrConverter::ToString(const IrValue& value) {
  if (absl::holds_alternative<BValue>(value)) {
    return absl::StrFormat("%p", absl::get<BValue>(value).node());
  }
  return absl::StrFormat("%p", absl::get<CValue>(value).value.node());
}

IrConverter::IrConverter(const std::shared_ptr<Package>& package,
                         Module* module,
                         const std::shared_ptr<TypeInfo>& type_info,
                         bool emit_positions)
    : package_(package),
      module_(module),
      type_info_(type_info),
      emit_positions_(emit_positions),
      // TODO(leary): 2019-07-19 Create a way to get the file path from the
      // module.
      fileno_(package->GetOrCreateFileno("fake_file.x")) {
  XLS_VLOG(5) << "Constructed IR converter: " << this;
}

void IrConverter::InstantiateFunctionBuilder(absl::string_view mangled_name) {
  XLS_CHECK(function_builder_ == nullptr);
  function_builder_ =
      std::make_shared<FunctionBuilder>(mangled_name, package_.get());
}

void IrConverter::AddConstantDep(ConstantDef* constant_def) {
  XLS_VLOG(2) << "Adding consatnt dep: " << constant_def->ToString();
  constant_deps_.push_back(constant_def);
}

absl::StatusOr<BValue> IrConverter::DefAlias(AstNode* from, AstNode* to) {
  auto it = node_to_ir_.find(from);
  if (it == node_to_ir_.end()) {
    return absl::InternalError(absl::StrFormat(
        "Could not find AST node for aliasing: %s", from->ToString()));
  }
  IrValue value = it->second;
  XLS_VLOG(5) << absl::StreamFormat("Aliased node '%s' to be same as '%s': %s",
                                    to->ToString(), from->ToString(),
                                    ToString(value));
  node_to_ir_[to] = std::move(value);
  if (auto* name_def = dynamic_cast<NameDef*>(to)) {
    // Name the aliased node based on the identifier in the NameDef.
    if (absl::holds_alternative<BValue>(node_to_ir_.at(from))) {
      BValue ir_node = absl::get<BValue>(node_to_ir_.at(from));
      ir_node.SetName(name_def->identifier());
    }
  }
  return Use(to);
}

absl::StatusOr<BValue> IrConverter::DefWithStatus(
    AstNode* node,
    const std::function<absl::StatusOr<BValue>(absl::optional<SourceLocation>)>&
        ir_func) {
  absl::optional<SourceLocation> loc = ToSourceLocation(node->GetSpan());
  XLS_ASSIGN_OR_RETURN(BValue result, ir_func(loc));
  XLS_VLOG(4) << absl::StreamFormat(
      "Define node '%s' (%s) to be %s @ %s", node->ToString(),
      node->GetNodeTypeName(), ToString(result), SpanToString(node->GetSpan()));
  SetNodeToIr(node, result);
  return result;
}

BValue IrConverter::Def(
    AstNode* node,
    const std::function<BValue(absl::optional<SourceLocation>)>& ir_func) {
  return DefWithStatus(node,
                       [&ir_func](absl::optional<SourceLocation> loc)
                           -> absl::StatusOr<BValue> { return ir_func(loc); })
      .value();
}

IrConverter::CValue IrConverter::DefConst(
    AstNode* node, IrLiteral ir_value) {
  auto ir_func = [&](absl::optional<SourceLocation> loc) {
    return function_builder_->Literal(ir_value, loc);
  };
  BValue result = Def(node, ir_func);
  CValue c_value{ir_value, result};
  SetNodeToIr(node, c_value);
  return c_value;
}

absl::StatusOr<BValue> IrConverter::Use(AstNode* node) const {
  auto it = node_to_ir_.find(node);
  if (it == node_to_ir_.end()) {
    return absl::NotFoundError(
        absl::StrFormat("Exception resolving %s node: %s",
                        node->GetNodeTypeName(), node->ToString()));
  }
  const IrValue& ir_value = it->second;
  XLS_VLOG(5) << absl::StreamFormat("Using node '%s' (%p) as IR value %s.",
                                    node->ToString(), node, ToString(ir_value));
  if (absl::holds_alternative<BValue>(ir_value)) {
    return absl::get<BValue>(ir_value);
  }
  XLS_RET_CHECK(absl::holds_alternative<CValue>(ir_value));
  return absl::get<CValue>(ir_value).value;
}

void IrConverter::SetNodeToIr(AstNode* node, IrValue value) {
  XLS_VLOG(5) << absl::StreamFormat("Setting node '%s' (%p) to IR value %s.",
                                    node->ToString(), node, ToString(value));
  node_to_ir_[node] = value;
}
absl::optional<IrConverter::IrValue> IrConverter::GetNodeToIr(
    AstNode* node) const {
  auto it = node_to_ir_.find(node);
  if (it == node_to_ir_.end()) {
    return absl::nullopt;
  }
  return it->second;
}

absl::Status IrConverter::HandleUnop(Unop* node) {
  XLS_ASSIGN_OR_RETURN(BValue operand, Use(node->operand()));
  switch (node->kind()) {
    case UnopKind::kNegate: {
      Def(node, [&](absl::optional<SourceLocation> loc) {
        return function_builder_->AddUnOp(IrOp::kNeg, operand, loc);
      });
      return absl::OkStatus();
    }
    case UnopKind::kInvert: {
      Def(node, [&](absl::optional<SourceLocation> loc) {
        return function_builder_->AddUnOp(IrOp::kNot, operand, loc);
      });
      return absl::OkStatus();
    }
  }
  return absl::InternalError(
      absl::StrCat("Invalid UnopKind: ", static_cast<int64>(node->kind())));
}

absl::Status IrConverter::HandleConcat(Binop* node, BValue lhs, BValue rhs) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> output_type,
                       ResolveType(node));
  std::vector<BValue> pieces = {lhs, rhs};
  if (dynamic_cast<BitsType*>(output_type.get()) != nullptr) {
    Def(node, [&](absl::optional<SourceLocation> loc) {
      return function_builder_->Concat(pieces, loc);
    });
    return absl::OkStatus();
  }

  // Fallthrough case should be an ArrayType.
  auto* array_output_type = dynamic_cast<ArrayType*>(output_type.get());
  XLS_RET_CHECK(array_output_type != nullptr);
  Def(node, [&](absl::optional<SourceLocation> loc) {
    return function_builder_->ArrayConcat(pieces, loc);
  });
  return absl::OkStatus();
}

SymbolicBindings IrConverter::GetSymbolicBindingsTuple() const {
  absl::flat_hash_set<std::string> module_level_constant_identifiers;
  for (const ConstantDef* constant : module_->GetConstantDefs()) {
    module_level_constant_identifiers.insert(constant->identifier());
  }
  absl::flat_hash_map<std::string, int64> sans_module_level_constants;
  for (const auto& item : symbolic_binding_map_) {
    if (module_level_constant_identifiers.contains(item.first)) {
      continue;
    }
    sans_module_level_constants[item.first] = item.second;
  }
  return SymbolicBindings(std::move(sans_module_level_constants));
}

absl::Status IrConverter::HandleNumber(Number* node) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> type, ResolveType(node));
  XLS_ASSIGN_OR_RETURN(ConcreteTypeDim dim, type->GetTotalBitCount());
  int64 bit_count = absl::get<int64>(dim.value());
  XLS_ASSIGN_OR_RETURN(Bits bits, node->GetBits(bit_count));
  DefConst(node, Value(bits));
  return absl::OkStatus();
}

absl::Status IrConverter::HandleXlsTuple(XlsTuple* node) {
  std::vector<BValue> operands;
  for (Expr* o : node->members()) {
    XLS_ASSIGN_OR_RETURN(BValue v, Use(o));
    operands.push_back(v);
  }
  Def(node, [this, &operands](absl::optional<SourceLocation> loc) {
    return function_builder_->Tuple(std::move(operands), loc);
  });
  return absl::OkStatus();
}

absl::Status IrConverter::HandleConstantDef(ConstantDef* node,
                                            const VisitFunc& visit) {
  XLS_RETURN_IF_ERROR(visit(node->value()));
  XLS_VLOG(5) << "Aliasing NameDef for constant: "
              << node->name_def()->ToString();
  return DefAlias(node->value(), /*to=*/node->name_def()).status();
}

absl::Status IrConverter::HandleLet(Let* node, const VisitFunc& visit) {
  XLS_RETURN_IF_ERROR(visit(node->rhs()));
  if (node->name_def_tree()->is_leaf()) {
    XLS_RETURN_IF_ERROR(
        DefAlias(node->rhs(), /*to=*/ToAstNode(node->name_def_tree()->leaf()))
            .status());
    XLS_RETURN_IF_ERROR(visit(node->body()));
    XLS_RETURN_IF_ERROR(DefAlias(node->body(), node).status());
  } else {
    // Walk the tree of names we're trying to bind, performing tuple_index
    // operations on the RHS to get to the values we want to bind to those
    // names.
    XLS_ASSIGN_OR_RETURN(BValue rhs, Use(node->rhs()));
    std::vector<BValue> levels = {rhs};
    // Invoked at each level of the NameDefTree: binds the name in the
    // NameDefTree to the correponding value (being pattern matched).
    //
    // Args:
    //  x: Current subtree of the NameDefTree.
    //  level: Level (depth) in the NameDefTree, root is 0.
    //  index: Index of node in the current tree level (e.g. leftmost is 0).
    auto walk = [&](NameDefTree* x, int64 level, int64 index) -> absl::Status {
      levels.resize(level);
      levels.push_back(Def(x, [this, &levels, x,
                               index](absl::optional<SourceLocation> loc) {
        if (loc.has_value()) {
          loc = ToSourceLocation(x->is_leaf() ? ToAstNode(x->leaf())->GetSpan()
                                              : x->GetSpan());
        }
        return function_builder_->TupleIndex(levels.back(), index, loc);
      }));
      if (x->is_leaf()) {
        XLS_RETURN_IF_ERROR(DefAlias(x, ToAstNode(x->leaf())).status());
      }
      return absl::OkStatus();
    };

    XLS_RETURN_IF_ERROR(node->name_def_tree()->DoPreorder(walk));
    XLS_RETURN_IF_ERROR(visit(node->body()));
    XLS_RETURN_IF_ERROR(DefAlias(node->body(), /*to=*/node).status());
  }

  if (last_expression_ == nullptr) {
    last_expression_ = node->body();
  }
  return absl::OkStatus();
}

absl::Status IrConverter::HandleCast(Cast* node, const VisitFunc& visit) {
  XLS_RETURN_IF_ERROR(visit(node->expr()));
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> output_type,
                       ResolveType(node));
  if (auto* array_type = dynamic_cast<ArrayType*>(output_type.get())) {
    return CastToArray(node, *array_type);
  }
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> input_type,
                       ResolveType(node->expr()));
  if (dynamic_cast<ArrayType*>(input_type.get()) != nullptr) {
    return CastFromArray(node, *output_type);
  }
  XLS_ASSIGN_OR_RETURN(ConcreteTypeDim new_bit_count_ctd,
                       output_type->GetTotalBitCount());
  int64 new_bit_count = absl::get<int64>(new_bit_count_ctd.value());
  XLS_ASSIGN_OR_RETURN(ConcreteTypeDim input_bit_count_ctd,
                       input_type->GetTotalBitCount());
  int64 old_bit_count = absl::get<int64>(input_bit_count_ctd.value());
  if (new_bit_count < old_bit_count) {
    auto bvalue_status = DefWithStatus(
        node,
        [this, node, new_bit_count](
            absl::optional<SourceLocation> loc) -> absl::StatusOr<BValue> {
          XLS_ASSIGN_OR_RETURN(BValue input, Use(node->expr()));
          return function_builder_->BitSlice(input, 0, new_bit_count);
        });
    XLS_RETURN_IF_ERROR(bvalue_status.status());
  } else {
    XLS_ASSIGN_OR_RETURN(bool signed_input, IsSigned(*input_type));
    auto bvalue_status = DefWithStatus(
        node,
        [this, node, new_bit_count, signed_input](
            absl::optional<SourceLocation> loc) -> absl::StatusOr<BValue> {
          XLS_ASSIGN_OR_RETURN(BValue input, Use(node->expr()));
          if (signed_input) {
            return function_builder_->SignExtend(input, new_bit_count);
          }
          return function_builder_->ZeroExtend(input, new_bit_count);
        });
    XLS_RETURN_IF_ERROR(bvalue_status.status());
  }
  return absl::OkStatus();
}

absl::Status IrConverter::HandleMatch(Match* node, const VisitFunc& visit) {
  if (node->arms().empty() ||
      !node->arms().back()->patterns()[0]->IsIrrefutable()) {
    return absl::UnimplementedError(absl::StrFormat(
        "ConversionError: %s Only matches with trailing irrefutable patterns "
        "are currently supported for IR conversion.",
        node->span().ToString()));
  }

  XLS_RETURN_IF_ERROR(visit(node->matched()));
  XLS_ASSIGN_OR_RETURN(BValue matched, Use(node->matched()));
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> matched_type,
                       ResolveType(node->matched()));

  MatchArm* default_arm = node->arms().back();
  if (default_arm->patterns().size() != 1) {
    return absl::UnimplementedError(
        absl::StrFormat("ConversionError: %s Multiple patterns in default arm "
                        "is not currently supported for IR conversion.",
                        node->span().ToString()));
  }
  XLS_RETURN_IF_ERROR(
      HandleMatcher(default_arm->patterns()[0],
                    {static_cast<int64>(node->arms().size()) - 1}, matched,
                    *matched_type, visit)
          .status());
  XLS_RETURN_IF_ERROR(visit(default_arm->expr()));

  std::vector<BValue> arm_selectors;
  std::vector<BValue> arm_values;
  for (int64 i = 0; i < node->arms().size() - 1; ++i) {
    MatchArm* arm = node->arms()[i];

    // Visit all the MatchArm's patterns.
    std::vector<BValue> this_arm_selectors;
    for (NameDefTree* pattern : arm->patterns()) {
      XLS_ASSIGN_OR_RETURN(
          BValue selector,
          HandleMatcher(pattern, {i}, matched, *matched_type, visit));
      this_arm_selectors.push_back(selector);
    }

    // "Or" together the patterns in this arm, if necessary, to determine if the
    // arm is selected.
    if (this_arm_selectors.size() > 1) {
      arm_selectors.push_back(function_builder_->AddNaryOp(
          Op::kOr, this_arm_selectors, ToSourceLocation(arm->span())));
    } else {
      arm_selectors.push_back(this_arm_selectors[0]);
    }
    XLS_RETURN_IF_ERROR(visit(arm->expr()));
    XLS_ASSIGN_OR_RETURN(BValue arm_rhs_value, Use(arm->expr()));
    arm_values.push_back(arm_rhs_value);
  }

  // So now we have the following representation of the match arms:
  //   match x {
  //     42  => blah
  //     64  => snarf
  //     128 => yep
  //     _   => burp
  //   }
  //
  //   selectors:     [x==42, x==64, x==128]
  //   values:        [blah,  snarf,    yep]
  //   default_value: burp
  XLS_ASSIGN_OR_RETURN(BValue default_value, Use(default_arm->expr()));
  SetNodeToIr(node, function_builder_->MatchTrue(arm_selectors, arm_values,
                                                 default_value));
  last_expression_ = node;
  return absl::OkStatus();
}

absl::StatusOr<BValue> IrConverter::HandleMatcher(
    NameDefTree* matcher, absl::Span<const int64> index,
    const BValue& matched_value, const ConcreteType& matched_type,
    const VisitFunc& visit) {
  if (matcher->is_leaf()) {
    NameDefTree::Leaf leaf = matcher->leaf();
    XLS_VLOG(5) << absl::StreamFormat("Matcher is leaf: %s (%s)",
                                      ToAstNode(leaf)->ToString(),
                                      ToAstNode(leaf)->GetNodeTypeName());
    if (absl::holds_alternative<WildcardPattern*>(leaf)) {
      return Def(matcher, [&](absl::optional<SourceLocation> loc) {
        return function_builder_->Literal(UBits(1, 1), loc);
      });
    } else if (absl::holds_alternative<Number*>(leaf) ||
               absl::holds_alternative<ColonRef*>(leaf)) {
      XLS_RETURN_IF_ERROR(visit(ToAstNode(leaf)));
      XLS_ASSIGN_OR_RETURN(BValue to_match, Use(ToAstNode(leaf)));
      return Def(matcher, [&](absl::optional<SourceLocation> loc) {
        return function_builder_->Eq(to_match, matched_value);
      });
    } else if (absl::holds_alternative<NameRef*>(leaf)) {
      // Comparing for equivalence to a (referenced) name.
      auto* name_ref = absl::get<NameRef*>(leaf);
      auto* name_def = absl::get<NameDef*>(name_ref->name_def());
      XLS_ASSIGN_OR_RETURN(BValue to_match, Use(name_def));
      BValue result = Def(matcher, [&](absl::optional<SourceLocation> loc) {
        return function_builder_->Eq(to_match, matched_value);
      });
      XLS_RETURN_IF_ERROR(DefAlias(name_def, name_ref).status());
      return result;
    } else {
      XLS_RET_CHECK(absl::holds_alternative<NameDef*>(leaf));
      auto* name_def = absl::get<NameDef*>(leaf);
      BValue ok = Def(name_def, [&](absl::optional<SourceLocation> loc) {
        return function_builder_->Literal(UBits(1, 1));
      });
      SetNodeToIr(matcher, matched_value);
      SetNodeToIr(ToAstNode(leaf), matched_value);
      return ok;
    }
  }

  auto* matched_tuple_type = dynamic_cast<const TupleType*>(&matched_type);
  BValue ok = function_builder_->Literal(UBits(/*value=*/1, /*bit_count=*/1));
  for (int64 i = 0; i < matched_tuple_type->size(); ++i) {
    const ConcreteType& element_type = matched_tuple_type->GetMemberType(i);
    NameDefTree* element = matcher->nodes()[i];
    BValue member = function_builder_->TupleIndex(matched_value, i);
    std::vector<int64> sub_index(index.begin(), index.end());
    sub_index.push_back(i);
    XLS_ASSIGN_OR_RETURN(BValue cond, HandleMatcher(element, sub_index, member,
                                                    element_type, visit));
    ok = function_builder_->And(ok, cond);
  }
  return ok;
}

absl::StatusOr<BValue> IrConverter::DefMapWithBuiltin(
    Invocation* parent_node, NameRef* node, AstNode* arg,
    const SymbolicBindings& symbolic_bindings) {
  XLS_ASSIGN_OR_RETURN(
      const std::string mangled_name,
      MangleDslxName(node->identifier(), {}, module_, &symbolic_bindings));
  XLS_ASSIGN_OR_RETURN(BValue arg_value, Use(arg));
  XLS_VLOG(5) << "Mapping with builtin; arg: "
              << arg_value.GetType()->ToString();
  auto* array_type = arg_value.GetType()->AsArrayOrDie();
  if (!package_->HasFunctionWithName(mangled_name)) {
    FunctionBuilder fb(mangled_name, package_.get());
    BValue param = fb.Param("arg", array_type->element_type());
    const std::string& builtin_name = node->identifier();
    BValue result;
    if (builtin_name == "clz") {
      result = fb.Clz(param);
    } else if (builtin_name == "ctz") {
      result = fb.Ctz(param);
    } else {
      return absl::InternalError("Invalid builtin name for map: " +
                                 builtin_name);
    }
    XLS_RETURN_IF_ERROR(fb.Build().status());
  }

  XLS_ASSIGN_OR_RETURN(xls::Function * f, package_->GetFunction(mangled_name));
  return Def(parent_node, [&](absl::optional<SourceLocation> loc) {
    return function_builder_->Map(arg_value, f);
  });
}

absl::StatusOr<BValue> IrConverter::HandleMap(Invocation* node,
                                              const VisitFunc& visit) {
  for (Expr* arg : node->args().subspan(0, node->args().size() - 1)) {
    XLS_RETURN_IF_ERROR(visit(arg));
  }
  XLS_ASSIGN_OR_RETURN(BValue arg, Use(node->args()[0]));
  Expr* fn_node = node->args()[1];
  XLS_VLOG(5) << "Function being mapped AST: " << fn_node->ToString();
  absl::optional<const SymbolicBindings*> node_sym_bindings =
      GetInvocationBindings(node);

  std::string map_fn_name;
  Module* lookup_module = nullptr;
  if (auto* name_ref = dynamic_cast<NameRef*>(fn_node)) {
    map_fn_name = name_ref->identifier();
    if (GetParametricBuiltins().contains(map_fn_name)) {
      XLS_VLOG(5) << "Map of parametric builtin: " << map_fn_name;
      return DefMapWithBuiltin(node, name_ref, node->args()[0],
                               *node_sym_bindings.value());
    }
    lookup_module = module_;
  } else if (auto* colon_ref = dynamic_cast<ColonRef*>(fn_node)) {
    map_fn_name = colon_ref->attr();
    absl::optional<Import*> import_node = colon_ref->ResolveImportSubject();
    absl::optional<const ImportedInfo*> info =
        type_info_->GetImported(*import_node);
    lookup_module = (*info)->module.get();
  } else {
    return absl::UnimplementedError("Unhandled function mapping: " +
                                    fn_node->ToString());
  }

  absl::optional<Function*> mapped_fn = lookup_module->GetFunction(map_fn_name);
  std::vector<std::string> free = (*mapped_fn)->GetFreeParametricKeys();
  absl::btree_set<std::string> free_set(free.begin(), free.end());
  XLS_ASSIGN_OR_RETURN(
      std::string mangled_name,
      MangleDslxName((*mapped_fn)->identifier(), free_set, lookup_module,
                     node_sym_bindings.value()));
  XLS_VLOG(5) << "Getting function with mangled name: " << mangled_name
              << " from package: " << package_->name();
  XLS_ASSIGN_OR_RETURN(xls::Function * f, package_->GetFunction(mangled_name));
  return Def(node, [&](absl::optional<SourceLocation> loc) -> BValue {
    return function_builder_->Map(arg, f, loc);
  });
}

absl::Status IrConverter::HandleIndex(Index* node, const VisitFunc& visit) {
  XLS_RETURN_IF_ERROR(visit(node->lhs()));
  XLS_ASSIGN_OR_RETURN(BValue lhs, Use(node->lhs()));

  absl::optional<const ConcreteType*> lhs_type =
      type_info_->GetItem(node->lhs());
  XLS_RET_CHECK(lhs_type.has_value());
  if (dynamic_cast<const TupleType*>(lhs_type.value()) != nullptr) {
    // Tuple indexing requires a compile-time-constant RHS.
    XLS_RETURN_IF_ERROR(visit(ToAstNode(node->rhs())));
    XLS_ASSIGN_OR_RETURN(Bits rhs, GetConstBits(ToAstNode(node->rhs())));
    XLS_ASSIGN_OR_RETURN(uint64 index, rhs.ToUint64());
    Def(node, [&](absl::optional<SourceLocation> loc) {
      return function_builder_->TupleIndex(lhs, index, loc);
    });
  } else if (dynamic_cast<const BitsType*>(lhs_type.value()) != nullptr) {
    IndexRhs rhs = node->rhs();
    if (absl::holds_alternative<WidthSlice*>(rhs)) {
      auto* width_slice = absl::get<WidthSlice*>(rhs);
      XLS_RETURN_IF_ERROR(visit(width_slice->start()));
      XLS_ASSIGN_OR_RETURN(BValue start, Use(width_slice->start()));
      XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> output_type,
                           ResolveType(node));
      XLS_ASSIGN_OR_RETURN(ConcreteTypeDim output_type_dim,
                           output_type->GetTotalBitCount());
      int64 width = absl::get<int64>(output_type_dim.value());
      Def(node, [&](absl::optional<SourceLocation> loc) {
        return function_builder_->DynamicBitSlice(lhs, start, width, loc);
      });
    } else {
      auto* slice = absl::get<Slice*>(rhs);
      absl::optional<StartAndWidth> saw =
          type_info_->GetSliceStartAndWidth(slice, GetSymbolicBindingsTuple());
      XLS_RET_CHECK(saw.has_value());
      Def(node, [&](absl::optional<SourceLocation> loc) {
        return function_builder_->BitSlice(lhs, saw->start, saw->width, loc);
      });
    }
  } else {
    XLS_RETURN_IF_ERROR(visit(ToAstNode(node->rhs())));
    XLS_ASSIGN_OR_RETURN(BValue index, Use(ToAstNode(node->rhs())));
    Def(node, [&](absl::optional<SourceLocation> loc) {
      return function_builder_->ArrayIndex(lhs, {index}, loc);
    });
  }
  return absl::OkStatus();
}

absl::Status IrConverter::HandleArray(Array* node, const VisitFunc& visit) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> type, ResolveType(node));
  const ArrayType* array_type = dynamic_cast<ArrayType*>(type.get());
  XLS_RET_CHECK(array_type != nullptr);
  std::vector<BValue> members;
  for (Expr* member : node->members()) {
    XLS_RETURN_IF_ERROR(visit(member));
    XLS_ASSIGN_OR_RETURN(BValue member_value, Use(member));
    members.push_back(member_value);
  }

  if (node->has_ellipsis()) {
    ConcreteTypeDim array_size_ctd = array_type->size();
    int64 array_size = absl::get<int64>(array_size_ctd.value());
    while (members.size() < array_size) {
      members.push_back(members.back());
    }
  }
  Def(node, [&](absl::optional<SourceLocation> loc) {
    xls::Type* type = members[0].GetType();
    return function_builder_->Array(std::move(members), type, loc);
  });
  return absl::OkStatus();
}

absl::Status IrConverter::HandleColonRef(ColonRef* node,
                                         const VisitFunc& visit) {
  // Implementation note: ColonRef "invocation" are handled in Invocation (by
  // resolving the mangled callee name, which should have been IR converted in
  // dependency order).

  if (absl::optional<Import*> import = node->ResolveImportSubject()) {
    absl::optional<const ImportedInfo*> imported =
        type_info_->GetImported(import.value());
    XLS_RET_CHECK(imported.has_value());
    Module* imported_mod = (*imported)->module.get();
    XLS_ASSIGN_OR_RETURN(ConstantDef * constant_def,
                         imported_mod->GetConstantDef(node->attr()));
    XLS_RETURN_IF_ERROR(HandleConstantDef(constant_def, visit));
    return DefAlias(constant_def->name_def(), /*to=*/node).status();
  }

  EnumDef* enum_def;
  if (absl::holds_alternative<NameRef*>(node->subject())) {
    XLS_ASSIGN_OR_RETURN(enum_def,
                         DerefEnum(absl::get<NameRef*>(node->subject())));
  } else {
    XLS_ASSIGN_OR_RETURN(TypeDefinition type_definition,
                         ToTypeDefinition(ToAstNode(node->subject())));
    XLS_ASSIGN_OR_RETURN(enum_def, DerefEnum(type_definition));
  }
  XLS_ASSIGN_OR_RETURN(auto value, enum_def->GetValue(node->attr()));
  Expr* value_expr = ToExprNode(value);
  XLS_RETURN_IF_ERROR(visit(value_expr));
  return DefAlias(/*from=*/value_expr, /*to=*/node).status();
}

absl::Status IrConverter::HandleSplatStructInstance(SplatStructInstance* node,
                                                    const VisitFunc& visit) {
  XLS_RETURN_IF_ERROR(visit(node->splatted()));
  XLS_ASSIGN_OR_RETURN(BValue original, Use(node->splatted()));

  absl::flat_hash_map<std::string, BValue> updates;
  for (const auto& item : node->members()) {
    XLS_RETURN_IF_ERROR(visit(item.second));
    XLS_ASSIGN_OR_RETURN(updates[item.first], Use(item.second));
  }

  XLS_ASSIGN_OR_RETURN(StructDef * struct_def,
                       DerefStruct(ToTypeDefinition(node->struct_ref())));
  std::vector<BValue> members;
  for (int64 i = 0; i < struct_def->members().size(); ++i) {
    const std::string& k = struct_def->GetMemberName(i);
    if (auto it = updates.find(k); it != updates.end()) {
      members.push_back(it->second);
    } else {
      members.push_back(function_builder_->TupleIndex(original, i));
    }
  }

  Def(node, [this, &members](absl::optional<SourceLocation> loc) {
    return function_builder_->Tuple(std::move(members), loc);
  });
  return absl::OkStatus();
}

absl::Status IrConverter::HandleStructInstance(StructInstance* node,
                                               const VisitFunc& visit) {
  std::vector<BValue> operands;
  XLS_ASSIGN_OR_RETURN(StructDef * struct_def,
                       DerefStruct(ToTypeDefinition(node->struct_def())));
  bool all_are_constant = true;
  std::vector<Value> const_operands;
  for (auto [_, member_expr] : node->GetOrderedMembers(struct_def)) {
    XLS_RETURN_IF_ERROR(visit(member_expr));
    XLS_ASSIGN_OR_RETURN(BValue operand, Use(member_expr));
    operands.push_back(operand);
    if (!IsConstant(member_expr)) {
      all_are_constant = false;
    }
    if (all_are_constant) {
      XLS_ASSIGN_OR_RETURN(Value const_operand, GetConstValue(member_expr));
      const_operands.push_back(std::move(const_operand));
    }
  }

  BValue result =
      Def(node, [this, &operands](absl::optional<SourceLocation> loc) {
        return function_builder_->Tuple(std::move(operands), loc);
      });
  if (all_are_constant) {
    SetNodeToIr(node, CValue{Value::Tuple(std::move(const_operands)), result});
  }
  return absl::OkStatus();
}

absl::StatusOr<std::string> IrConverter::GetCalleeIdentifier(Invocation* node) {
  XLS_VLOG(3) << "Getting callee identifier for invocation: "
              << node->ToString();
  Expr* callee = node->callee();
  std::string callee_name;
  Module* m;
  if (auto* name_ref = dynamic_cast<NameRef*>(callee)) {
    callee_name = name_ref->identifier();
    m = module_;
  } else if (auto* colon_ref = dynamic_cast<ColonRef*>(callee)) {
    callee_name = colon_ref->attr();
    absl::optional<Import*> import = colon_ref->ResolveImportSubject();
    XLS_RET_CHECK(import.has_value());
    absl::optional<const ImportedInfo*> info = type_info_->GetImported(*import);
    m = (*info)->module.get();
  } else {
    return absl::InternalError("Invalid callee: " + callee->ToString());
  }

  absl::optional<Function*> f = m->GetFunction(callee_name);
  if (!f.has_value()) {
    // For e.g. builtins that are not in the module we just provide the name
    // directly.
    return callee_name;
  }

  std::vector<std::string> free_keys_vector = (*f)->GetFreeParametricKeys();
  absl::btree_set<std::string> free_keys(free_keys_vector.begin(),
                                         free_keys_vector.end());
  if (!(*f)->IsParametric()) {
    return MangleDslxName((*f)->identifier(), free_keys, m);
  }

  absl::optional<const SymbolicBindings*> resolved_symbolic_bindings =
      GetInvocationBindings(node);
  XLS_RET_CHECK(resolved_symbolic_bindings.has_value());
  XLS_VLOG(2) << absl::StreamFormat("Node %s @ %s symbolic bindings %s",
                                    node->ToString(), node->span().ToString(),
                                    (*resolved_symbolic_bindings)->ToString());
  XLS_RET_CHECK(!(*resolved_symbolic_bindings)->empty());
  return MangleDslxName((*f)->identifier(), free_keys, m,
                        resolved_symbolic_bindings.value());
}

absl::Status IrConverter::HandleBinop(Binop* node) {
  absl::optional<const ConcreteType*> lhs_type =
      type_info_->GetItem(node->lhs());
  XLS_RET_CHECK(lhs_type.has_value());
  auto* bits_type = dynamic_cast<const BitsType*>(lhs_type.value());
  bool signed_input = bits_type != nullptr && bits_type->is_signed();
  XLS_ASSIGN_OR_RETURN(BValue lhs, Use(node->lhs()));
  XLS_ASSIGN_OR_RETURN(BValue rhs, Use(node->rhs()));
  std::function<BValue(absl::optional<SourceLocation>)> ir_func;

  switch (node->kind()) {
    case BinopKind::kConcat:
      // Concat is handled out of line since it makes different IR ops for bits
      // and array kinds.
      return HandleConcat(node, lhs, rhs);
    // Arithmetic.
    case BinopKind::kAdd:
      ir_func = [&](absl::optional<SourceLocation> loc) {
        return function_builder_->Add(lhs, rhs, loc);
      };
      break;
    case BinopKind::kSub:
      ir_func = [&](absl::optional<SourceLocation> loc) {
        return function_builder_->Subtract(lhs, rhs, loc);
      };
      break;
    case BinopKind::kMul:
      ir_func = [&](absl::optional<SourceLocation> loc) {
        if (signed_input) {
          return function_builder_->SMul(lhs, rhs, loc);
        }
        return function_builder_->UMul(lhs, rhs, loc);
      };
      break;
    case BinopKind::kDiv:
      ir_func = [&](absl::optional<SourceLocation> loc) {
        return function_builder_->UDiv(lhs, rhs, loc);
      };
      break;
    // Comparisons.
    case BinopKind::kEq:
      ir_func = [&](absl::optional<SourceLocation> loc) {
        return function_builder_->Eq(lhs, rhs, loc);
      };
      break;
    case BinopKind::kNe:
      ir_func = [&](absl::optional<SourceLocation> loc) {
        return function_builder_->Ne(lhs, rhs, loc);
      };
      break;
    case BinopKind::kGe:
      ir_func = [&](absl::optional<SourceLocation> loc) {
        if (signed_input) {
          return function_builder_->SGe(lhs, rhs, loc);
        }
        return function_builder_->UGe(lhs, rhs, loc);
      };
      break;
    case BinopKind::kGt:
      ir_func = [&](absl::optional<SourceLocation> loc) {
        if (signed_input) {
          return function_builder_->SGt(lhs, rhs, loc);
        }
        return function_builder_->UGt(lhs, rhs, loc);
      };
      break;
    case BinopKind::kLe:
      ir_func = [&](absl::optional<SourceLocation> loc) {
        if (signed_input) {
          return function_builder_->SLe(lhs, rhs, loc);
        }
        return function_builder_->ULe(lhs, rhs, loc);
      };
      break;
    case BinopKind::kLt:
      ir_func = [&](absl::optional<SourceLocation> loc) {
        if (signed_input) {
          return function_builder_->SLt(lhs, rhs, loc);
        }
        return function_builder_->ULt(lhs, rhs, loc);
      };
      break;
    // Shifts.
    case BinopKind::kShrl:
      ir_func = [&](absl::optional<SourceLocation> loc) {
        return function_builder_->Shrl(lhs, rhs, loc);
      };
      break;
    case BinopKind::kShll:
      ir_func = [&](absl::optional<SourceLocation> loc) {
        return function_builder_->Shll(lhs, rhs, loc);
      };
      break;
    case BinopKind::kShra:
      ir_func = [&](absl::optional<SourceLocation> loc) {
        return function_builder_->Shra(lhs, rhs, loc);
      };
      break;
    // Bitwise.
    case BinopKind::kXor:
      ir_func = [&](absl::optional<SourceLocation> loc) {
        return function_builder_->Xor(lhs, rhs, loc);
      };
      break;
    case BinopKind::kAnd:
      ir_func = [&](absl::optional<SourceLocation> loc) {
        return function_builder_->And(lhs, rhs, loc);
      };
      break;
    case BinopKind::kOr:
      ir_func = [&](absl::optional<SourceLocation> loc) {
        return function_builder_->Or(lhs, rhs, loc);
      };
      break;
    // Logical.
    case BinopKind::kLogicalAnd:
      ir_func = [&](absl::optional<SourceLocation> loc) {
        return function_builder_->And(lhs, rhs, loc);
      };
      break;
    case BinopKind::kLogicalOr:
      ir_func = [&](absl::optional<SourceLocation> loc) {
        return function_builder_->Or(lhs, rhs, loc);
      };
      break;
  }
  Def(node, ir_func);
  return absl::OkStatus();
}

absl::Status IrConverter::HandleAttr(Attr* node) {
  absl::optional<const ConcreteType*> lhs_type =
      type_info()->GetItem(node->lhs());
  XLS_RET_CHECK(lhs_type.has_value());
  auto* tuple_type = dynamic_cast<const TupleType*>(lhs_type.value());
  const std::string& identifier = node->attr()->identifier();
  XLS_ASSIGN_OR_RETURN(int64 index, tuple_type->GetMemberIndex(identifier));
  XLS_ASSIGN_OR_RETURN(BValue lhs, Use(node->lhs()));
  BValue ir = Def(node, [&](absl::optional<SourceLocation> loc) {
    return function_builder_->TupleIndex(lhs, index, loc);
  });
  // Give the tuple-index instruction a meaningful name based on the identifier.
  if (lhs.HasAssignedName()) {
    ir.SetName(absl::StrCat(lhs.GetName(), "_", identifier));
  } else {
    ir.SetName(identifier);
  }
  return absl::OkStatus();
}

absl::Status IrConverter::HandleTernary(Ternary* node) {
  XLS_ASSIGN_OR_RETURN(BValue arg0, Use(node->test()));
  XLS_ASSIGN_OR_RETURN(BValue arg1, Use(node->consequent()));
  XLS_ASSIGN_OR_RETURN(BValue arg2, Use(node->alternate()));
  Def(node, [&](absl::optional<SourceLocation> loc) {
    return function_builder_->Select(arg0, arg1, arg2, loc);
  });
  return absl::OkStatus();
}

absl::Status IrConverter::HandleBuiltinAndReduce(Invocation* node) {
  XLS_RET_CHECK_EQ(node->args().size(), 1);
  XLS_ASSIGN_OR_RETURN(BValue arg, Use(node->args()[0]));
  Def(node, [&](absl::optional<SourceLocation> loc) {
    return function_builder_->AndReduce(arg, loc);
  });
  return absl::OkStatus();
}

absl::Status IrConverter::HandleBuiltinBitSlice(Invocation* node) {
  XLS_RET_CHECK_EQ(node->args().size(), 3);
  XLS_ASSIGN_OR_RETURN(BValue arg, Use(node->args()[0]));
  XLS_ASSIGN_OR_RETURN(Bits start_bits, GetConstBits(node->args()[1]));
  XLS_ASSIGN_OR_RETURN(uint64 start, start_bits.ToUint64());
  XLS_ASSIGN_OR_RETURN(Bits width_bits, GetConstBits(node->args()[2]));
  XLS_ASSIGN_OR_RETURN(uint64 width, width_bits.ToUint64());
  Def(node, [&](absl::optional<SourceLocation> loc) {
    return function_builder_->BitSlice(arg, start, width, loc);
  });
  return absl::OkStatus();
}

absl::Status IrConverter::HandleBuiltinClz(Invocation* node) {
  XLS_RET_CHECK_EQ(node->args().size(), 1);
  XLS_ASSIGN_OR_RETURN(BValue arg, Use(node->args()[0]));
  Def(node, [&](absl::optional<SourceLocation> loc) {
    return function_builder_->Clz(arg, loc);
  });
  return absl::OkStatus();
}

absl::Status IrConverter::HandleBuiltinCtz(Invocation* node) {
  XLS_RET_CHECK_EQ(node->args().size(), 1);
  XLS_ASSIGN_OR_RETURN(BValue arg, Use(node->args()[0]));
  Def(node, [&](absl::optional<SourceLocation> loc) {
    return function_builder_->Ctz(arg, loc);
  });
  return absl::OkStatus();
}

absl::Status IrConverter::HandleBuiltinOneHot(Invocation* node) {
  XLS_RET_CHECK_EQ(node->args().size(), 2);
  XLS_ASSIGN_OR_RETURN(BValue input, Use(node->args()[0]));
  XLS_ASSIGN_OR_RETURN(Bits lsb_prio, GetConstBits(node->args()[1]));
  XLS_ASSIGN_OR_RETURN(uint64 lsb_prio_value, lsb_prio.ToUint64());

  Def(node, [&](absl::optional<SourceLocation> loc) {
    return function_builder_->OneHot(
        input, lsb_prio_value ? LsbOrMsb::kLsb : LsbOrMsb::kMsb, loc);
  });
  return absl::OkStatus();
}

absl::Status IrConverter::HandleBuiltinOneHotSel(Invocation* node) {
  XLS_RET_CHECK_EQ(node->args().size(), 2);
  XLS_ASSIGN_OR_RETURN(BValue selector, Use(node->args()[0]));

  const Expr* cases_arg = node->args()[1];
  std::vector<BValue> cases;
  const auto* array = dynamic_cast<const Array*>(cases_arg);
  XLS_RET_CHECK_NE(array, nullptr);
  for (const auto& sel_case : array->members()) {
    XLS_ASSIGN_OR_RETURN(BValue bvalue_case, Use(sel_case));
    cases.push_back(bvalue_case);
  }

  Def(node, [&](absl::optional<SourceLocation> loc) {
    return function_builder_->OneHotSelect(
        selector, cases, loc);
  });
  return absl::OkStatus();
}

absl::Status IrConverter::HandleBuiltinOrReduce(Invocation* node) {
  XLS_RET_CHECK_EQ(node->args().size(), 1);
  XLS_ASSIGN_OR_RETURN(BValue arg, Use(node->args()[0]));
  Def(node, [&](absl::optional<SourceLocation> loc) {
    return function_builder_->OrReduce(arg, loc);
  });
  return absl::OkStatus();
}

absl::Status IrConverter::HandleBuiltinRev(Invocation* node) {
  XLS_RET_CHECK_EQ(node->args().size(), 1);
  XLS_ASSIGN_OR_RETURN(BValue arg, Use(node->args()[0]));
  Def(node, [&](absl::optional<SourceLocation> loc) {
    return function_builder_->Reverse(arg, loc);
  });
  return absl::OkStatus();
}

absl::Status IrConverter::HandleBuiltinSignex(Invocation* node) {
  XLS_RET_CHECK_EQ(node->args().size(), 2);
  XLS_ASSIGN_OR_RETURN(BValue arg, Use(node->args()[0]));

  // Remember - it's the _type_ of the RHS of a signex that gives the new bit
  // count, not the value!
  auto* bit_count = dynamic_cast<Number*>(node->args()[1]);
  XLS_RET_CHECK_NE(bit_count, nullptr);
  XLS_RET_CHECK(bit_count->type());
  auto* type_annot = dynamic_cast<BuiltinTypeAnnotation*>(bit_count->type());
  int64 new_bit_count = type_annot->GetBitCount();

  Def(node, [&](absl::optional<SourceLocation> loc) {
    return function_builder_->SignExtend(arg, new_bit_count, loc);
  });
  return absl::OkStatus();
}

absl::Status IrConverter::HandleBuiltinUpdate(Invocation* node) {
  XLS_RET_CHECK_EQ(node->args().size(), 3);
  XLS_ASSIGN_OR_RETURN(BValue arg, Use(node->args()[0]));
  XLS_ASSIGN_OR_RETURN(BValue index, Use(node->args()[1]));
  XLS_ASSIGN_OR_RETURN(BValue new_value, Use(node->args()[2]));
  Def(node, [&](absl::optional<SourceLocation> loc) {
    return function_builder_->ArrayUpdate(arg, new_value, {index}, loc);
  });
  return absl::OkStatus();
}

absl::Status IrConverter::HandleBuiltinXorReduce(Invocation* node) {
  XLS_RET_CHECK_EQ(node->args().size(), 1);
  XLS_ASSIGN_OR_RETURN(BValue arg, Use(node->args()[0]));
  Def(node, [&](absl::optional<SourceLocation> loc) {
    return function_builder_->XorReduce(arg, loc);
  });
  return absl::OkStatus();
}

/* static */ absl::StatusOr<Value> IrConverter::InterpValueToValue(
    const InterpValue& iv) {
  switch (iv.tag()) {
    case InterpValueTag::kSBits:
    case InterpValueTag::kUBits:
    case InterpValueTag::kEnum:
      return Value(iv.GetBitsOrDie());
    case InterpValueTag::kTuple:
    case InterpValueTag::kArray: {
      std::vector<Value> ir_values;
      for (const InterpValue& e : iv.GetValuesOrDie()) {
        XLS_ASSIGN_OR_RETURN(Value ir_value, InterpValueToValue(e));
        ir_values.push_back(std::move(ir_value));
      }
      if (iv.tag() == InterpValueTag::kTuple) {
        return Value::Tuple(std::move(ir_values));
      }
      return Value::Array(std::move(ir_values));
    }
    default:
      return absl::InvalidArgumentError(
          "Cannot convert interpreter value with tag: " +
          TagToString(iv.tag()));
  }
}

absl::Status IrConverter::CastToArray(Cast* node,
                                      const ArrayType& output_type) {
  XLS_ASSIGN_OR_RETURN(BValue bits, Use(node->expr()));
  std::vector<BValue> slices;
  XLS_ASSIGN_OR_RETURN(ConcreteTypeDim element_bit_count_dim,
                       output_type.element_type().GetTotalBitCount());
  int64 element_bit_count = absl::get<int64>(element_bit_count_dim.value());
  int64 array_size = absl::get<int64>(output_type.size().value());
  // MSb becomes lowest-indexed array element.
  for (int64 i = 0; i < array_size; ++i) {
    slices.push_back(function_builder_->BitSlice(bits, i * element_bit_count,
                                                 element_bit_count));
  }
  std::reverse(slices.begin(), slices.end());
  xls::Type* element_type = package_->GetBitsType(element_bit_count);
  Def(node, [this, &slices, element_type](absl::optional<SourceLocation> loc) {
    return function_builder_->Array(std::move(slices), element_type, loc);
  });
  return absl::OkStatus();
}

absl::Status IrConverter::CastFromArray(Cast* node,
                                        const ConcreteType& output_type) {
  XLS_ASSIGN_OR_RETURN(BValue array, Use(node->expr()));
  XLS_ASSIGN_OR_RETURN(xls::Type * input_type, ResolveTypeToIr(node->expr()));
  xls::ArrayType* array_type = input_type->AsArrayOrDie();
  const int64 array_size = array_type->size();
  std::vector<BValue> pieces;
  for (int64 i = 0; i < array_size; ++i) {
    BValue index = function_builder_->Literal(UBits(i, 32));
    pieces.push_back(function_builder_->ArrayIndex(array, {index}));
  }
  Def(node, [this, &pieces](absl::optional<SourceLocation> loc) {
    return function_builder_->Concat(std::move(pieces), loc);
  });
  return absl::OkStatus();
}

absl::StatusOr<IrConverter::DerefVariant> IrConverter::DerefStructOrEnum(
    TypeDefinition node) {
  while (absl::holds_alternative<TypeDef*>(node)) {
    auto* type_def = absl::get<TypeDef*>(node);
    TypeAnnotation* annotation = type_def->type();
    if (auto* type_ref_annotation =
            dynamic_cast<TypeRefTypeAnnotation*>(annotation)) {
      node = type_ref_annotation->type_ref()->type_definition();
    } else {
      return absl::UnimplementedError(
          "Unhandled typedef for resolving to struct-or-enum: " +
          annotation->ToString());
    }
  }

  if (absl::holds_alternative<StructDef*>(node)) {
    return absl::get<StructDef*>(node);
  }
  if (absl::holds_alternative<EnumDef*>(node)) {
    return absl::get<EnumDef*>(node);
  }

  XLS_RET_CHECK(absl::holds_alternative<ColonRef*>(node));
  auto* colon_ref = absl::get<ColonRef*>(node);
  absl::optional<Import*> import = colon_ref->ResolveImportSubject();
  XLS_RET_CHECK(import.has_value());
  absl::optional<const ImportedInfo*> info = type_info_->GetImported(*import);
  Module* imported_mod = (*info)->module.get();
  XLS_ASSIGN_OR_RETURN(TypeDefinition td,
                       imported_mod->GetTypeDefinition(colon_ref->attr()));
  // Recurse to resolve the typedef within the imported module.
  return DerefStructOrEnum(td);
}

absl::StatusOr<StructDef*> IrConverter::DerefStruct(TypeDefinition node) {
  XLS_ASSIGN_OR_RETURN(DerefVariant v, DerefStructOrEnum(node));
  XLS_RET_CHECK(absl::holds_alternative<StructDef*>(v));
  return absl::get<StructDef*>(v);
}

absl::StatusOr<EnumDef*> IrConverter::DerefEnum(TypeDefinition node) {
  XLS_ASSIGN_OR_RETURN(DerefVariant v, DerefStructOrEnum(node));
  XLS_RET_CHECK(absl::holds_alternative<EnumDef*>(v));
  return absl::get<EnumDef*>(v);
}

/* static */ absl::StatusOr<InterpValue> IrConverter::ValueToInterpValue(
    const Value& v) {
  switch (v.kind()) {
    case ValueKind::kBits:
      return InterpValue::MakeBits(InterpValueTag::kUBits, v.bits());
    case ValueKind::kArray:
    case ValueKind::kTuple: {
      std::vector<InterpValue> members;
      for (const Value& e : v.elements()) {
        XLS_ASSIGN_OR_RETURN(InterpValue iv, ValueToInterpValue(e));
        members.push_back(iv);
      }
      return InterpValue::MakeTuple(std::move(members));
    }
    default:
      return absl::InvalidArgumentError(
          "Cannot convert IR value to interpreter value: " + v.ToString());
  }
}

absl::StatusOr<ConcreteTypeDim> IrConverter::ResolveDim(ConcreteTypeDim dim) {
  while (
      absl::holds_alternative<ConcreteTypeDim::OwnedParametric>(dim.value())) {
    ParametricExpression& original =
        *absl::get<ConcreteTypeDim::OwnedParametric>(dim.value());
    ParametricExpression::Evaluated evaluated = original.Evaluate(
        ToParametricEnv(SymbolicBindings(symbolic_binding_map_)));
    dim = ConcreteTypeDim(std::move(evaluated));
  }
  return dim;
}

absl::StatusOr<std::unique_ptr<ConcreteType>> IrConverter::ResolveType(
    AstNode* node) {
  absl::optional<const ConcreteType*> t = type_info_->GetItem(node);
  if (!t.has_value()) {
    return ConversionErrorStatus(
        node->GetSpan(),
        absl::StrFormat(
            "Failed to convert IR because type was missing for AST node: %s",
            node->ToString()));
  }

  return t.value()->MapSize(
      [this](ConcreteTypeDim dim) { return ResolveDim(dim); });
}

absl::StatusOr<Value> IrConverter::GetConstValue(AstNode* node) const {
  absl::optional<IrValue> ir_value = GetNodeToIr(node);
  if (!ir_value.has_value()) {
    return absl::InternalError(absl::StrFormat(
        "AST node had no associated IR value: %s", node->ToString()));
  }
  if (!absl::holds_alternative<CValue>(*ir_value)) {
    return absl::InternalError(absl::StrFormat(
        "AST node had a non-const IR value: %s", node->ToString()));
  }
  return absl::get<CValue>(*ir_value).ir_value;
}

absl::StatusOr<Bits> IrConverter::GetConstBits(AstNode* node) const {
  XLS_ASSIGN_OR_RETURN(Value value, GetConstValue(node));
  return value.GetBitsWithStatus();
}

absl::Status IrConverter::HandleConstantArray(ConstantArray* node) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> type, ResolveType(node));
  auto* array_type = dynamic_cast<ArrayType*>(type.get());

  std::vector<IrLiteral> values;
  for (Expr* n : node->members()) {
    // All elements are invariants of the given ConstantArray node.
    XLS_RET_CHECK(IsConstant(n));
    auto ir_value = GetNodeToIr(n);
    XLS_RET_CHECK(ir_value.has_value());
    XLS_RET_CHECK(absl::holds_alternative<CValue>(ir_value.value()));
    values.push_back(absl::get<CValue>(ir_value.value()).ir_value);
  }
  if (node->has_ellipsis()) {
    while (values.size() < absl::get<int64>(array_type->size().value())) {
      values.push_back(values.back());
    }
  }
  Value ir_value = IrLiteral::Array(std::move(values)).value();
  DefConst(node, ir_value);
  return absl::OkStatus();
}

absl::StatusOr<std::string> MangleDslxName(
    absl::string_view function_name,
    const absl::btree_set<std::string>& free_keys, Module* module,
    const SymbolicBindings* symbolic_bindings) {
  absl::btree_set<std::string> symbolic_bindings_keys;
  std::vector<int64> symbolic_bindings_values;
  if (symbolic_bindings != nullptr) {
    for (const SymbolicBinding& item : symbolic_bindings->bindings()) {
      symbolic_bindings_keys.insert(item.identifier);
      symbolic_bindings_values.push_back(item.value);
    }
  }
  absl::btree_set<std::string> difference;
  absl::c_set_difference(free_keys, symbolic_bindings_keys,
                         std::inserter(difference, difference.begin()));
  if (!difference.empty()) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Not enough symbolic bindings to convert function "
                        "'%s'; need {%s} got {%s}",
                        function_name, absl::StrJoin(free_keys, ", "),
                        absl::StrJoin(symbolic_bindings_keys, ", ")));
  }

  std::string module_name = absl::StrReplaceAll(module->name(), {{".", "_"}});
  if (symbolic_bindings_values.empty()) {
    return absl::StrFormat("__%s__%s", module_name, function_name);
  }
  std::string suffix = absl::StrJoin(symbolic_bindings_values, "_");
  return absl::StrFormat("__%s__%s__%s", module_name, function_name, suffix);
}

absl::Status ConversionErrorStatus(const absl::optional<Span>& span,
                                   absl::string_view message) {
  return absl::InternalError(
      absl::StrFormat("ConversionErrorStatus: %s %s",
                      span ? span->ToString() : "<no span>", message));
}

absl::StatusOr<xls::Type*> IrConverter::ResolveTypeToIr(AstNode* node) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<ConcreteType> concrete_type,
                       ResolveType(node));
  return TypeToIr(*concrete_type);
}

absl::StatusOr<xls::Type*> IrConverter::TypeToIr(
    const ConcreteType& concrete_type) {
  XLS_VLOG(4) << "Converting concrete type to IR: " << concrete_type;
  if (auto* array_type = dynamic_cast<const ArrayType*>(&concrete_type)) {
    XLS_ASSIGN_OR_RETURN(xls::Type * element_type,
                         TypeToIr(array_type->element_type()));
    int64 element_count = absl::get<int64>(array_type->size().value());
    xls::Type* result = package_->GetArrayType(element_count, element_type);
    XLS_VLOG(4) << "Converted type to IR; concrete type: " << concrete_type
                << " ir: " << result->ToString()
                << " element_count: " << element_count;
    return result;
  }
  if (auto* bits_type = dynamic_cast<const BitsType*>(&concrete_type)) {
    int64 bit_count = absl::get<int64>(bits_type->size().value());
    return package_->GetBitsType(bit_count);
  }
  if (auto* enum_type = dynamic_cast<const EnumType*>(&concrete_type)) {
    int64 bit_count = absl::get<int64>(enum_type->size().value());
    return package_->GetBitsType(bit_count);
  }
  auto* tuple_type = dynamic_cast<const TupleType*>(&concrete_type);
  XLS_RET_CHECK(tuple_type != nullptr) << concrete_type;
  std::vector<xls::Type*> members;
  for (const ConcreteType* m : tuple_type->GetUnnamedMembers()) {
    XLS_ASSIGN_OR_RETURN(xls::Type * type, TypeToIr(*m));
    members.push_back(type);
  }
  return package_->GetTupleType(std::move(members));
}

}  // namespace xls::dslx
