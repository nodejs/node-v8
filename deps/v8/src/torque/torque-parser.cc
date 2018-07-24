// Copyright 2018 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cctype>

#include "src/torque/earley-parser.h"
#include "src/torque/torque-parser.h"
#include "src/torque/utils.h"

namespace v8 {
namespace internal {
namespace torque {

using TypeList = std::vector<TypeExpression*>;
using GenericParameters = std::vector<std::string>;

struct ExpressionWithSource {
  Expression* expression;
  std::string source;
};

enum class ParseResultHolderBase::TypeId {
  kStdString,
  kBool,
  kStdVectorOfString,
  kExpressionPtr,
  kLocationExpressionPtr,
  kStatementPtr,
  kDeclarationPtr,
  kTypeExpressionPtr,
  kLabelBlockPtr,
  kNameAndTypeExpression,
  kStdVectorOfNameAndTypeExpression,
  kIncrementDecrementOperator,
  kOptionalStdString,
  kStdVectorOfStatementPtr,
  kStdVectorOfDeclarationPtr,
  kStdVectorOfExpressionPtr,
  kExpressionWithSource,
  kParameterList,
  kRangeExpression,
  kOptionalRangeExpression,
  kTypeList,
  kOptionalTypeList,
  kLabelAndTypes,
  kStdVectorOfLabelAndTypes,
  kStdVectorOfLabelBlockPtr,
  kOptionalStatementPtr,
  kOptionalExpressionPtr
};

template <>
const ParseResultTypeId ParseResultHolder<std::string>::id =
    ParseResultTypeId::kStdString;
template <>
const ParseResultTypeId ParseResultHolder<bool>::id = ParseResultTypeId::kBool;
template <>
const ParseResultTypeId ParseResultHolder<std::vector<std::string>>::id =
    ParseResultTypeId::kStdVectorOfString;
template <>
const ParseResultTypeId ParseResultHolder<Declaration*>::id =
    ParseResultTypeId::kDeclarationPtr;
template <>
const ParseResultTypeId ParseResultHolder<TypeExpression*>::id =
    ParseResultTypeId::kTypeExpressionPtr;
template <>
const ParseResultTypeId ParseResultHolder<LabelBlock*>::id =
    ParseResultTypeId::kLabelBlockPtr;
template <>
const ParseResultTypeId ParseResultHolder<Expression*>::id =
    ParseResultTypeId::kExpressionPtr;
template <>
const ParseResultTypeId ParseResultHolder<LocationExpression*>::id =
    ParseResultTypeId::kLocationExpressionPtr;
template <>
const ParseResultTypeId ParseResultHolder<Statement*>::id =
    ParseResultTypeId::kStatementPtr;
template <>
const ParseResultTypeId ParseResultHolder<NameAndTypeExpression>::id =
    ParseResultTypeId::kNameAndTypeExpression;
template <>
const ParseResultTypeId
    ParseResultHolder<std::vector<NameAndTypeExpression>>::id =
        ParseResultTypeId::kStdVectorOfNameAndTypeExpression;
template <>
const ParseResultTypeId ParseResultHolder<IncrementDecrementOperator>::id =
    ParseResultTypeId::kIncrementDecrementOperator;
template <>
const ParseResultTypeId ParseResultHolder<base::Optional<std::string>>::id =
    ParseResultTypeId::kOptionalStdString;
template <>
const ParseResultTypeId ParseResultHolder<std::vector<Statement*>>::id =
    ParseResultTypeId::kStdVectorOfStatementPtr;
template <>
const ParseResultTypeId ParseResultHolder<std::vector<Declaration*>>::id =
    ParseResultTypeId::kStdVectorOfDeclarationPtr;
template <>
const ParseResultTypeId ParseResultHolder<std::vector<Expression*>>::id =
    ParseResultTypeId::kStdVectorOfExpressionPtr;
template <>
const ParseResultTypeId ParseResultHolder<ExpressionWithSource>::id =
    ParseResultTypeId::kExpressionWithSource;
template <>
const ParseResultTypeId ParseResultHolder<ParameterList>::id =
    ParseResultTypeId::kParameterList;
template <>
const ParseResultTypeId ParseResultHolder<RangeExpression>::id =
    ParseResultTypeId::kRangeExpression;
template <>
const ParseResultTypeId ParseResultHolder<base::Optional<RangeExpression>>::id =
    ParseResultTypeId::kOptionalRangeExpression;
template <>
const ParseResultTypeId ParseResultHolder<TypeList>::id =
    ParseResultTypeId::kTypeList;
template <>
const ParseResultTypeId ParseResultHolder<base::Optional<TypeList>>::id =
    ParseResultTypeId::kOptionalTypeList;
template <>
const ParseResultTypeId ParseResultHolder<LabelAndTypes>::id =
    ParseResultTypeId::kLabelAndTypes;
template <>
const ParseResultTypeId ParseResultHolder<std::vector<LabelAndTypes>>::id =
    ParseResultTypeId::kStdVectorOfLabelAndTypes;
template <>
const ParseResultTypeId ParseResultHolder<std::vector<LabelBlock*>>::id =
    ParseResultTypeId::kStdVectorOfLabelBlockPtr;
template <>
const ParseResultTypeId ParseResultHolder<base::Optional<Statement*>>::id =
    ParseResultTypeId::kOptionalStatementPtr;
template <>
const ParseResultTypeId ParseResultHolder<base::Optional<Expression*>>::id =
    ParseResultTypeId::kOptionalExpressionPtr;

namespace {

base::Optional<ParseResult> AddGlobalDeclaration(
    ParseResultIterator* child_results) {
  auto declaration = child_results->NextAs<Declaration*>();
  CurrentAst::Get().declarations().push_back(declaration);
  return base::nullopt;
}

template <class T, class... Args>
T* MakeNode(Args... args) {
  return CurrentAst::Get().AddNode(std::unique_ptr<T>(
      new T(CurrentSourcePosition::Get(), std::move(args)...)));
}

base::Optional<ParseResult> MakeCall(ParseResultIterator* child_results) {
  auto callee = child_results->NextAs<std::string>();
  auto generic_args = child_results->NextAs<TypeList>();
  auto args = child_results->NextAs<std::vector<Expression*>>();
  auto labels = child_results->NextAs<std::vector<std::string>>();
  Expression* result =
      MakeNode<CallExpression>(callee, false, generic_args, args, labels);
  return ParseResult{result};
}

base::Optional<ParseResult> MakeBinaryOperator(
    ParseResultIterator* child_results) {
  auto left = child_results->NextAs<Expression*>();
  auto op = child_results->NextAs<std::string>();
  auto right = child_results->NextAs<Expression*>();
  Expression* result = MakeNode<CallExpression>(
      op, true, TypeList{}, std::vector<Expression*>{left, right},
      std::vector<std::string>{});
  return ParseResult{result};
}

base::Optional<ParseResult> MakeUnaryOperator(
    ParseResultIterator* child_results) {
  auto op = child_results->NextAs<std::string>();
  auto e = child_results->NextAs<Expression*>();
  Expression* result = MakeNode<CallExpression>(op, true, TypeList{},
                                                std::vector<Expression*>{e},
                                                std::vector<std::string>{});
  return ParseResult{result};
}

template <bool has_varargs>
base::Optional<ParseResult> MakeParameterListFromTypes(
    ParseResultIterator* child_results) {
  auto types = child_results->NextAs<TypeList>();
  ParameterList result;
  result.types = std::move(types);
  result.has_varargs = has_varargs;
  return ParseResult{std::move(result)};
}
template <bool has_varargs>
base::Optional<ParseResult> MakeParameterListFromNameAndTypeList(
    ParseResultIterator* child_results) {
  auto params = child_results->NextAs<std::vector<NameAndTypeExpression>>();
  std::string arguments_variable = "";
  if (child_results->HasNext()) {
    arguments_variable = child_results->NextAs<std::string>();
  }
  ParameterList result;
  for (NameAndTypeExpression& pair : params) {
    result.names.push_back(std::move(pair.name));
    result.types.push_back(pair.type);
  }
  result.has_varargs = has_varargs;
  result.arguments_variable = arguments_variable;
  return ParseResult{std::move(result)};
}

base::Optional<ParseResult> MakeAssertStatement(
    ParseResultIterator* child_results) {
  auto kind = child_results->NextAs<std::string>();
  auto expr_with_source = child_results->NextAs<ExpressionWithSource>();
  DCHECK(kind == "assert" || kind == "check");
  Statement* result = MakeNode<AssertStatement>(
      kind == "assert", expr_with_source.expression, expr_with_source.source);
  return ParseResult{result};
}

base::Optional<ParseResult> MakeDebugStatement(
    ParseResultIterator* child_results) {
  auto kind = child_results->NextAs<std::string>();
  DCHECK(kind == "unreachable" || kind == "debug");
  Statement* result = MakeNode<DebugStatement>(kind, kind == "unreachable");
  return ParseResult{result};
}

base::Optional<ParseResult> MakeVoidType(ParseResultIterator* child_results) {
  TypeExpression* result = MakeNode<BasicTypeExpression>(false, "void");
  return ParseResult{result};
}

base::Optional<ParseResult> MakeExternalMacro(
    ParseResultIterator* child_results) {
  auto operator_name = child_results->NextAs<base::Optional<std::string>>();
  auto name = child_results->NextAs<std::string>();
  auto generic_parameters = child_results->NextAs<GenericParameters>();
  auto args = child_results->NextAs<ParameterList>();
  auto return_type = child_results->NextAs<TypeExpression*>();
  auto labels = child_results->NextAs<LabelAndTypesVector>();
  MacroDeclaration* macro = MakeNode<ExternalMacroDeclaration>(
      name, operator_name, args, return_type, labels);
  Declaration* result;
  if (generic_parameters.empty()) {
    result = MakeNode<StandardDeclaration>(macro, nullptr);
  } else {
    result = MakeNode<GenericDeclaration>(macro, generic_parameters);
  }
  return ParseResult{result};
}

base::Optional<ParseResult> MakeTorqueMacroDeclaration(
    ParseResultIterator* child_results) {
  auto operator_name = child_results->NextAs<base::Optional<std::string>>();
  auto name = child_results->NextAs<std::string>();
  auto generic_parameters = child_results->NextAs<GenericParameters>();
  auto args = child_results->NextAs<ParameterList>();
  auto return_type = child_results->NextAs<TypeExpression*>();
  auto labels = child_results->NextAs<LabelAndTypesVector>();
  auto body = child_results->NextAs<base::Optional<Statement*>>();
  MacroDeclaration* macro = MakeNode<TorqueMacroDeclaration>(
      name, operator_name, args, return_type, labels);
  Declaration* result;
  if (generic_parameters.empty()) {
    if (!body) ReportError("A non-generic declaration needs a body.");
    result = MakeNode<StandardDeclaration>(macro, *body);
  } else {
    result = MakeNode<GenericDeclaration>(macro, generic_parameters, body);
  }
  return ParseResult{result};
}

base::Optional<ParseResult> MakeTorqueBuiltinDeclaration(
    ParseResultIterator* child_results) {
  auto javascript_linkage = child_results->NextAs<bool>();
  auto name = child_results->NextAs<std::string>();
  auto generic_parameters = child_results->NextAs<GenericParameters>();
  auto args = child_results->NextAs<ParameterList>();
  auto return_type = child_results->NextAs<TypeExpression*>();
  auto body = child_results->NextAs<base::Optional<Statement*>>();
  BuiltinDeclaration* builtin = MakeNode<TorqueBuiltinDeclaration>(
      javascript_linkage, name, args, return_type);
  Declaration* result;
  if (generic_parameters.empty()) {
    if (!body) ReportError("A non-generic declaration needs a body.");
    result = MakeNode<StandardDeclaration>(builtin, *body);
  } else {
    result = MakeNode<GenericDeclaration>(builtin, generic_parameters, body);
  }
  return ParseResult{result};
}

base::Optional<ParseResult> MakeConstDeclaration(
    ParseResultIterator* child_results) {
  auto name = child_results->NextAs<std::string>();
  auto type = child_results->NextAs<TypeExpression*>();
  auto expression = child_results->NextAs<Expression*>();
  Declaration* result =
      MakeNode<ConstDeclaration>(std::move(name), type, expression);
  return ParseResult{result};
}

base::Optional<ParseResult> MakeExternConstDeclaration(
    ParseResultIterator* child_results) {
  auto name = child_results->NextAs<std::string>();
  auto type = child_results->NextAs<TypeExpression*>();
  auto literal = child_results->NextAs<std::string>();
  Declaration* result = MakeNode<ExternConstDeclaration>(std::move(name), type,
                                                         std::move(literal));
  return ParseResult{result};
}

base::Optional<ParseResult> MakeTypeAliasDeclaration(
    ParseResultIterator* child_results) {
  auto name = child_results->NextAs<std::string>();
  auto type = child_results->NextAs<TypeExpression*>();
  Declaration* result = MakeNode<TypeAliasDeclaration>(std::move(name), type);
  return ParseResult{result};
}

base::Optional<ParseResult> MakeTypeDeclaration(
    ParseResultIterator* child_results) {
  auto name = child_results->NextAs<std::string>();
  auto extends = child_results->NextAs<base::Optional<std::string>>();
  auto generates = child_results->NextAs<base::Optional<std::string>>();
  auto constexpr_generates =
      child_results->NextAs<base::Optional<std::string>>();
  Declaration* result = MakeNode<TypeDeclaration>(
      std::move(name), std::move(extends), std::move(generates),
      std::move(constexpr_generates));
  return ParseResult{result};
}

base::Optional<ParseResult> MakeExplicitModuleDeclaration(
    ParseResultIterator* child_results) {
  auto name = child_results->NextAs<std::string>();
  auto declarations = child_results->NextAs<std::vector<Declaration*>>();
  Declaration* result = MakeNode<ExplicitModuleDeclaration>(
      std::move(name), std::move(declarations));
  return ParseResult{result};
}

base::Optional<ParseResult> MakeSpecializationDeclaration(
    ParseResultIterator* child_results) {
  auto name = child_results->NextAs<std::string>();
  auto generic_parameters =
      child_results->NextAs<std::vector<TypeExpression*>>();
  auto parameters = child_results->NextAs<ParameterList>();
  auto return_type = child_results->NextAs<TypeExpression*>();
  auto labels = child_results->NextAs<LabelAndTypesVector>();
  auto body = child_results->NextAs<Statement*>();
  Declaration* result = MakeNode<SpecializationDeclaration>(
      std::move(name), std::move(generic_parameters), std::move(parameters),
      return_type, std::move(labels), body);
  return ParseResult{result};
}

base::Optional<ParseResult> MakeStructDeclaration(
    ParseResultIterator* child_results) {
  auto name = child_results->NextAs<std::string>();
  auto fields = child_results->NextAs<std::vector<NameAndTypeExpression>>();
  Declaration* result =
      MakeNode<StructDeclaration>(std::move(name), std::move(fields));
  return ParseResult{result};
}

base::Optional<ParseResult> MakeExternalBuiltin(
    ParseResultIterator* child_results) {
  auto js_linkage = child_results->NextAs<bool>();
  auto name = child_results->NextAs<std::string>();
  auto generic_parameters = child_results->NextAs<GenericParameters>();
  auto args = child_results->NextAs<ParameterList>();
  auto return_type = child_results->NextAs<TypeExpression*>();
  BuiltinDeclaration* builtin =
      MakeNode<ExternalBuiltinDeclaration>(js_linkage, name, args, return_type);
  Declaration* result;
  if (generic_parameters.empty()) {
    result = MakeNode<StandardDeclaration>(builtin, nullptr);
  } else {
    result = MakeNode<GenericDeclaration>(builtin, generic_parameters);
  }
  return ParseResult{result};
}

base::Optional<ParseResult> MakeExternalRuntime(
    ParseResultIterator* child_results) {
  auto name = child_results->NextAs<std::string>();
  auto args = child_results->NextAs<ParameterList>();
  auto return_type = child_results->NextAs<TypeExpression*>();
  ExternalRuntimeDeclaration* runtime =
      MakeNode<ExternalRuntimeDeclaration>(name, args, return_type);
  Declaration* result = MakeNode<StandardDeclaration>(runtime, nullptr);
  return ParseResult{result};
}

base::Optional<ParseResult> StringLiteralUnquoteAction(
    ParseResultIterator* child_results) {
  return ParseResult{
      StringLiteralUnquote(child_results->NextAs<std::string>())};
}

base::Optional<ParseResult> MakeBasicTypeExpression(
    ParseResultIterator* child_results) {
  auto is_constexpr = child_results->NextAs<bool>();
  auto name = child_results->NextAs<std::string>();
  TypeExpression* result =
      MakeNode<BasicTypeExpression>(is_constexpr, std::move(name));
  return ParseResult{result};
}

base::Optional<ParseResult> MakeFunctionTypeExpression(
    ParseResultIterator* child_results) {
  auto parameters = child_results->NextAs<std::vector<TypeExpression*>>();
  auto return_type = child_results->NextAs<TypeExpression*>();
  TypeExpression* result =
      MakeNode<FunctionTypeExpression>(std::move(parameters), return_type);
  return ParseResult{result};
}

base::Optional<ParseResult> MakeUnionTypeExpression(
    ParseResultIterator* child_results) {
  auto a = child_results->NextAs<TypeExpression*>();
  auto b = child_results->NextAs<TypeExpression*>();
  TypeExpression* result = MakeNode<UnionTypeExpression>(a, b);
  return ParseResult{result};
}

base::Optional<ParseResult> MakeExpressionStatement(
    ParseResultIterator* child_results) {
  auto expression = child_results->NextAs<Expression*>();
  Statement* result = MakeNode<ExpressionStatement>(expression);
  return ParseResult{result};
}

base::Optional<ParseResult> MakeIfStatement(
    ParseResultIterator* child_results) {
  auto is_constexpr = child_results->NextAs<bool>();
  auto condition = child_results->NextAs<Expression*>();
  auto if_true = child_results->NextAs<Statement*>();
  auto if_false = child_results->NextAs<base::Optional<Statement*>>();
  Statement* result =
      MakeNode<IfStatement>(is_constexpr, condition, if_true, if_false);
  return ParseResult{result};
}

base::Optional<ParseResult> MakeWhileStatement(
    ParseResultIterator* child_results) {
  auto condition = child_results->NextAs<Expression*>();
  auto body = child_results->NextAs<Statement*>();
  Statement* result = MakeNode<WhileStatement>(condition, body);
  return ParseResult{result};
}

base::Optional<ParseResult> MakeReturnStatement(
    ParseResultIterator* child_results) {
  auto value = child_results->NextAs<base::Optional<Expression*>>();
  Statement* result = MakeNode<ReturnStatement>(value);
  return ParseResult{result};
}

base::Optional<ParseResult> MakeTailCallStatement(
    ParseResultIterator* child_results) {
  auto value = child_results->NextAs<Expression*>();
  Statement* result = MakeNode<TailCallStatement>(CallExpression::cast(value));
  return ParseResult{result};
}

base::Optional<ParseResult> MakeVarDeclarationStatement(
    ParseResultIterator* child_results) {
  auto kind = child_results->NextAs<std::string>();
  bool const_qualified = kind == "const";
  if (!const_qualified) DCHECK_EQ("let", kind);
  auto name = child_results->NextAs<std::string>();
  auto type = child_results->NextAs<TypeExpression*>();
  base::Optional<Expression*> initializer;
  if (child_results->HasNext())
    initializer = child_results->NextAs<Expression*>();
  Statement* result = MakeNode<VarDeclarationStatement>(
      const_qualified, std::move(name), type, initializer);
  return ParseResult{result};
}

base::Optional<ParseResult> MakeBreakStatement(
    ParseResultIterator* child_results) {
  Statement* result = MakeNode<BreakStatement>();
  return ParseResult{result};
}

base::Optional<ParseResult> MakeContinueStatement(
    ParseResultIterator* child_results) {
  Statement* result = MakeNode<ContinueStatement>();
  return ParseResult{result};
}

base::Optional<ParseResult> MakeGotoStatement(
    ParseResultIterator* child_results) {
  auto label = child_results->NextAs<std::string>();
  auto arguments = child_results->NextAs<std::vector<Expression*>>();
  Statement* result =
      MakeNode<GotoStatement>(std::move(label), std::move(arguments));
  return ParseResult{result};
}

base::Optional<ParseResult> MakeBlockStatement(
    ParseResultIterator* child_results) {
  auto deferred = child_results->NextAs<bool>();
  auto statements = child_results->NextAs<std::vector<Statement*>>();
  Statement* result = MakeNode<BlockStatement>(deferred, std::move(statements));
  return ParseResult{result};
}

base::Optional<ParseResult> MakeTryLabelStatement(
    ParseResultIterator* child_results) {
  auto try_block = child_results->NextAs<Statement*>();
  auto label_blocks = child_results->NextAs<std::vector<LabelBlock*>>();
  Statement* result =
      MakeNode<TryLabelStatement>(try_block, std::move(label_blocks));
  return ParseResult{result};
}

base::Optional<ParseResult> MakeForOfLoopStatement(
    ParseResultIterator* child_results) {
  auto var_decl = child_results->NextAs<Statement*>();
  auto iterable = child_results->NextAs<Expression*>();
  auto range = child_results->NextAs<base::Optional<RangeExpression>>();
  auto body = child_results->NextAs<Statement*>();
  Statement* result =
      MakeNode<ForOfLoopStatement>(var_decl, iterable, range, body);
  return ParseResult{result};
}

base::Optional<ParseResult> MakeForLoopStatement(
    ParseResultIterator* child_results) {
  auto var_decl = child_results->NextAs<base::Optional<Statement*>>();
  auto test = child_results->NextAs<Expression*>();
  auto action = child_results->NextAs<Expression*>();
  auto body = child_results->NextAs<Statement*>();
  Statement* result = MakeNode<ForLoopStatement>(var_decl, test, action, body);
  return ParseResult{result};
}

base::Optional<ParseResult> MakeLabelBlock(ParseResultIterator* child_results) {
  auto label = child_results->NextAs<std::string>();
  auto parameters = child_results->NextAs<ParameterList>();
  auto body = child_results->NextAs<Statement*>();
  LabelBlock* result =
      MakeNode<LabelBlock>(std::move(label), std::move(parameters), body);
  return ParseResult{result};
}

base::Optional<ParseResult> MakeRangeExpression(
    ParseResultIterator* child_results) {
  auto begin = child_results->NextAs<base::Optional<Expression*>>();
  auto end = child_results->NextAs<base::Optional<Expression*>>();
  RangeExpression result = {begin, end};
  return ParseResult{result};
}

base::Optional<ParseResult> MakeExpressionWithSource(
    ParseResultIterator* child_results) {
  auto e = child_results->NextAs<Expression*>();
  return ParseResult{
      ExpressionWithSource{e, child_results->matched_input().ToString()}};
}

base::Optional<ParseResult> MakeIdentifierExpression(
    ParseResultIterator* child_results) {
  auto name = child_results->NextAs<std::string>();
  auto generic_arguments =
      child_results->NextAs<std::vector<TypeExpression*>>();
  LocationExpression* result = MakeNode<IdentifierExpression>(
      std::move(name), std::move(generic_arguments));
  return ParseResult{result};
}

base::Optional<ParseResult> MakeFieldAccessExpression(
    ParseResultIterator* child_results) {
  auto object = child_results->NextAs<Expression*>();
  auto field = child_results->NextAs<std::string>();
  LocationExpression* result =
      MakeNode<FieldAccessExpression>(object, std::move(field));
  return ParseResult{result};
}

base::Optional<ParseResult> MakeElementAccessExpression(
    ParseResultIterator* child_results) {
  auto object = child_results->NextAs<Expression*>();
  auto field = child_results->NextAs<Expression*>();
  LocationExpression* result = MakeNode<ElementAccessExpression>(object, field);
  return ParseResult{result};
}

base::Optional<ParseResult> MakeStructExpression(
    ParseResultIterator* child_results) {
  auto name = child_results->NextAs<std::string>();
  auto expressions = child_results->NextAs<std::vector<Expression*>>();
  Expression* result =
      MakeNode<StructExpression>(std::move(name), std::move(expressions));
  return ParseResult{result};
}

base::Optional<ParseResult> MakeAssignmentExpression(
    ParseResultIterator* child_results) {
  auto location = child_results->NextAs<LocationExpression*>();
  auto op = child_results->NextAs<base::Optional<std::string>>();
  auto value = child_results->NextAs<Expression*>();
  Expression* result =
      MakeNode<AssignmentExpression>(location, std::move(op), value);
  return ParseResult{result};
}

base::Optional<ParseResult> MakeNumberLiteralExpression(
    ParseResultIterator* child_results) {
  auto number = child_results->NextAs<std::string>();
  Expression* result = MakeNode<NumberLiteralExpression>(std::move(number));
  return ParseResult{result};
}

base::Optional<ParseResult> MakeStringLiteralExpression(
    ParseResultIterator* child_results) {
  auto literal = child_results->NextAs<std::string>();
  Expression* result = MakeNode<StringLiteralExpression>(std::move(literal));
  return ParseResult{result};
}

base::Optional<ParseResult> MakeIncrementDecrementExpressionPostfix(
    ParseResultIterator* child_results) {
  auto location = child_results->NextAs<LocationExpression*>();
  auto op = child_results->NextAs<IncrementDecrementOperator>();
  Expression* result =
      MakeNode<IncrementDecrementExpression>(location, op, true);
  return ParseResult{result};
}

base::Optional<ParseResult> MakeIncrementDecrementExpressionPrefix(
    ParseResultIterator* child_results) {
  auto op = child_results->NextAs<IncrementDecrementOperator>();
  auto location = child_results->NextAs<LocationExpression*>();
  Expression* result =
      MakeNode<IncrementDecrementExpression>(location, op, false);
  return ParseResult{result};
}

base::Optional<ParseResult> MakeLogicalOrExpression(
    ParseResultIterator* child_results) {
  auto left = child_results->NextAs<Expression*>();
  auto right = child_results->NextAs<Expression*>();
  Expression* result = MakeNode<LogicalOrExpression>(left, right);
  return ParseResult{result};
}

base::Optional<ParseResult> MakeLogicalAndExpression(
    ParseResultIterator* child_results) {
  auto left = child_results->NextAs<Expression*>();
  auto right = child_results->NextAs<Expression*>();
  Expression* result = MakeNode<LogicalAndExpression>(left, right);
  return ParseResult{result};
}

base::Optional<ParseResult> MakeConditionalExpression(
    ParseResultIterator* child_results) {
  auto condition = child_results->NextAs<Expression*>();
  auto if_true = child_results->NextAs<Expression*>();
  auto if_false = child_results->NextAs<Expression*>();
  Expression* result =
      MakeNode<ConditionalExpression>(condition, if_true, if_false);
  return ParseResult{result};
}

base::Optional<ParseResult> MakeLabelAndTypes(
    ParseResultIterator* child_results) {
  auto name = child_results->NextAs<std::string>();
  auto types = child_results->NextAs<std::vector<TypeExpression*>>();
  return ParseResult{LabelAndTypes{std::move(name), std::move(types)}};
}

base::Optional<ParseResult> MakeNameAndType(
    ParseResultIterator* child_results) {
  auto name = child_results->NextAs<std::string>();
  auto type = child_results->NextAs<TypeExpression*>();
  return ParseResult{NameAndTypeExpression{std::move(name), type}};
}

base::Optional<ParseResult> ExtractAssignmentOperator(
    ParseResultIterator* child_results) {
  auto op = child_results->NextAs<std::string>();
  base::Optional<std::string> result = std::string(op.begin() + 1, op.end());
  return ParseResult(std::move(result));
}

struct TorqueGrammar : Grammar {
  static bool MatchWhitespace(InputPosition* pos) {
    while (true) {
      if (MatchChar(std::isspace, pos)) continue;
      if (MatchString("//", pos)) {
        while (MatchChar([](char c) { return c != '\n'; }, pos)) {
        }
        continue;
      }
      return true;
    }
  }

  static bool MatchIdentifier(InputPosition* pos) {
    if (!MatchChar(std::isalpha, pos)) return false;
    while (MatchChar(std::isalnum, pos) || MatchString("_", pos)) {
    }
    return true;
  }

  static bool MatchStringLiteral(InputPosition* pos) {
    InputPosition current = *pos;
    if (MatchString("\"", &current)) {
      while (
          (MatchString("\\", &current) && MatchAnyChar(&current)) ||
          MatchChar([](char c) { return c != '"' && c != '\n'; }, &current)) {
      }
      if (MatchString("\"", &current)) {
        *pos = current;
        return true;
      }
    }
    current = *pos;
    if (MatchString("'", &current)) {
      while (
          (MatchString("\\", &current) && MatchAnyChar(&current)) ||
          MatchChar([](char c) { return c != '\'' && c != '\n'; }, &current)) {
      }
      if (MatchString("'", &current)) {
        *pos = current;
        return true;
      }
    }
    return false;
  }

  static bool MatchHexLiteral(InputPosition* pos) {
    InputPosition current = *pos;
    MatchString("-", &current);
    if (MatchString("0x", &current) && MatchChar(std::isxdigit, &current)) {
      while (MatchChar(std::isxdigit, &current)) {
      }
      *pos = current;
      return true;
    }
    return false;
  }

  static bool MatchDecimalLiteral(InputPosition* pos) {
    InputPosition current = *pos;
    bool found_digit = false;
    MatchString("-", &current);
    while (MatchChar(std::isdigit, &current)) found_digit = true;
    MatchString(".", &current);
    while (MatchChar(std::isdigit, &current)) found_digit = true;
    if (!found_digit) return false;
    *pos = current;
    if ((MatchString("e", &current) || MatchString("E", &current)) &&
        (MatchString("+", &current) || MatchString("-", &current) || true) &&
        MatchChar(std::isdigit, &current)) {
      while (MatchChar(std::isdigit, &current)) {
      }
      *pos = current;
      return true;
    }
    return true;
  }

  TorqueGrammar() : Grammar(&file) { SetWhitespace(MatchWhitespace); }

  // Result: std::string
  Symbol identifier = {Rule({Pattern(MatchIdentifier)}, YieldMatchedInput)};

  // Result: std::string
  Symbol stringLiteral = {
      Rule({Pattern(MatchStringLiteral)}, YieldMatchedInput)};

  // Result: std::string
  Symbol externalString = {Rule({&stringLiteral}, StringLiteralUnquoteAction)};

  // Result: std::string
  Symbol decimalLiteral = {
      Rule({Pattern(MatchDecimalLiteral)}, YieldMatchedInput),
      Rule({Pattern(MatchHexLiteral)}, YieldMatchedInput)};

  // Result: TypeList
  Symbol* typeList = List<TypeExpression*>(&type, Token(","));

  // Result: TypeExpression*
  Symbol simpleType = {
      Rule({Token("("), &type, Token(")")}),
      Rule({CheckIf(Token("constexpr")), &identifier}, MakeBasicTypeExpression),
      Rule({Token("builtin"), Token("("), typeList, Token(")"), Token("=>"),
            &simpleType},
           MakeFunctionTypeExpression)};

  // Result: TypeExpression*
  Symbol type = {Rule({&simpleType}), Rule({&type, Token("|"), &simpleType},
                                           MakeUnionTypeExpression)};

  // Result: GenericParameters
  Symbol genericParameters = {
      Rule({Token("<"),
            List<std::string>(
                Sequence({&identifier, Token(":"), Token("type")}), Token(",")),
            Token(">")})};

  // Result: TypeList
  Symbol genericSpecializationTypeList = {
      Rule({Token("<"), typeList, Token(">")})};

  // Result: base::Optional<TypeList>
  Symbol* optionalGenericParameters = Optional<TypeList>(&genericParameters);

  // Result: ParameterList
  Symbol typeListMaybeVarArgs = {
      Rule({Token("("), List<TypeExpression*>(Sequence({&type, Token(",")})),
            Token("..."), Token(")")},
           MakeParameterListFromTypes<true>),
      Rule({Token("("), typeList, Token(")")},
           MakeParameterListFromTypes<false>)};

  // Result: LabelAndTypes
  Symbol labelParameter = {Rule(
      {&identifier,
       TryOrDefault<TypeList>(Sequence({Token("("), typeList, Token(")")}))},
      MakeLabelAndTypes)};

  // Result: TypeExpression*
  Symbol optionalReturnType = {Rule({Token(":"), &type}),
                               Rule({}, MakeVoidType)};

  // Result: LabelAndTypesVector
  Symbol* optionalLabelList{TryOrDefault<LabelAndTypesVector>(
      Sequence({Token("labels"),
                NonemptyList<LabelAndTypes>(&labelParameter, Token(","))}))};

  // Result: std::vector<std::string>
  Symbol* optionalOtherwise{TryOrDefault<std::vector<std::string>>(
      Sequence({Token("otherwise"),
                NonemptyList<std::string>(&identifier, Token(","))}))};

  // Result: NameAndTypeExpression
  Symbol nameAndType = {
      Rule({&identifier, Token(":"), &type}, MakeNameAndType)};

  // Result: ParameterList
  Symbol parameterListNoVararg = {
      Rule({Token("("), List<NameAndTypeExpression>(&nameAndType, Token(",")),
            Token(")")},
           MakeParameterListFromNameAndTypeList<false>)};

  // Result: ParameterList
  Symbol parameterListAllowVararg = {
      Rule({&parameterListNoVararg}),
      Rule({Token("("),
            NonemptyList<NameAndTypeExpression>(&nameAndType, Token(",")),
            Token(","), Token("..."), &identifier, Token(")")},
           MakeParameterListFromNameAndTypeList<true>)};

  // Result: std::string
  Symbol* OneOf(std::vector<std::string> alternatives) {
    Symbol* result = NewSymbol();
    for (const std::string& s : alternatives) {
      result->AddRule(Rule({Token(s)}, YieldMatchedInput));
    }
    return result;
  }

  // Result: Expression*
  Symbol* BinaryOperator(Symbol* nextLevel, Symbol* op) {
    Symbol* result = NewSymbol();
    *result = {Rule({nextLevel}),
               Rule({result, op, nextLevel}, MakeBinaryOperator)};
    return result;
  }

  // Result: Expression*
  Symbol* expression = &assignmentExpression;

  // Result: IncrementDecrementOperator
  Symbol incrementDecrementOperator = {
      Rule({Token("++")},
           YieldIntegralConstant<IncrementDecrementOperator,
                                 IncrementDecrementOperator::kIncrement>),
      Rule({Token("--")},
           YieldIntegralConstant<IncrementDecrementOperator,
                                 IncrementDecrementOperator::kDecrement>)};

  // Result: LocationExpression*
  Symbol locationExpression = {
      Rule(
          {&identifier, TryOrDefault<TypeList>(&genericSpecializationTypeList)},
          MakeIdentifierExpression),
      Rule({&primaryExpression, Token("."), &identifier},
           MakeFieldAccessExpression),
      Rule({&primaryExpression, Token("["), expression, Token("]")},
           MakeElementAccessExpression)};

  // Result: std::vector<Expression*>
  Symbol argumentList = {Rule(
      {Token("("), List<Expression*>(expression, Token(",")), Token(")")})};

  // Result: Expression*
  Symbol callExpression = {
      Rule({&identifier, TryOrDefault<TypeList>(&genericSpecializationTypeList),
            &argumentList, optionalOtherwise},
           MakeCall)};

  // Result: Expression*
  Symbol primaryExpression = {
      Rule({&callExpression}),
      Rule({&locationExpression},
           CastParseResult<LocationExpression*, Expression*>),
      Rule({&decimalLiteral}, MakeNumberLiteralExpression),
      Rule({&stringLiteral}, MakeStringLiteralExpression),
      Rule({&identifier, Token("{"), List<Expression*>(expression, Token(",")),
            Token("}")},
           MakeStructExpression),
      Rule({Token("("), expression, Token(")")})};

  // Result: Expression*
  Symbol unaryExpression = {
      Rule({&primaryExpression}),
      Rule({OneOf({"+", "-", "!", "~"}), &unaryExpression}, MakeUnaryOperator),
      Rule({&incrementDecrementOperator, &locationExpression},
           MakeIncrementDecrementExpressionPrefix),
      Rule({&locationExpression, &incrementDecrementOperator},
           MakeIncrementDecrementExpressionPostfix)};

  // Result: Expression*
  Symbol* multiplicativeExpression =
      BinaryOperator(&unaryExpression, OneOf({"*", "/", "%"}));

  // Result: Expression*
  Symbol* additiveExpression =
      BinaryOperator(multiplicativeExpression, OneOf({"+", "-"}));

  // Result: Expression*
  Symbol* shiftExpression =
      BinaryOperator(additiveExpression, OneOf({"<<", ">>", ">>>"}));

  // Do not allow expressions like a < b > c because this is never
  // useful and ambiguous with template parameters.
  // Result: Expression*
  Symbol relationalExpression = {
      Rule({shiftExpression}),
      Rule({shiftExpression, OneOf({"<", ">", "<=", ">="}), shiftExpression},
           MakeBinaryOperator)};

  // Result: Expression*
  Symbol* equalityExpression =
      BinaryOperator(&relationalExpression, OneOf({"==", "!="}));

  // Result: Expression*
  Symbol* bitwiseExpression =
      BinaryOperator(equalityExpression, OneOf({"&", "|"}));

  // Result: Expression*
  Symbol logicalAndExpression = {
      Rule({bitwiseExpression}),
      Rule({&logicalAndExpression, Token("&&"), bitwiseExpression},
           MakeLogicalAndExpression)};

  // Result: Expression*
  Symbol logicalOrExpression = {
      Rule({&logicalAndExpression}),
      Rule({&logicalOrExpression, Token("||"), &logicalAndExpression},
           MakeLogicalOrExpression)};

  // Result: Expression*
  Symbol conditionalExpression = {
      Rule({&logicalOrExpression}),
      Rule({&logicalOrExpression, Token("?"), expression, Token(":"),
            &conditionalExpression},
           MakeConditionalExpression)};

  // Result: base::Optional<std::string>
  Symbol assignmentOperator = {
      Rule({Token("=")}, YieldDefaultValue<base::Optional<std::string>>),
      Rule({OneOf({"*=", "/=", "%=", "+=", "-=", "<<=", ">>=", ">>>=", "&=",
                   "^=", "|="})},
           ExtractAssignmentOperator)};

  // Result: Expression*
  Symbol assignmentExpression = {
      Rule({&conditionalExpression}),
      Rule({&locationExpression, &assignmentOperator, &assignmentExpression},
           MakeAssignmentExpression)};

  // Result: Statement*
  Symbol block = {Rule({CheckIf(Token("deferred")), Token("{"),
                        List<Statement*>(&statement), Token("}")},
                       MakeBlockStatement)};

  // Result: LabelBlock*
  Symbol labelBlock = {
      Rule({Token("label"), &identifier,
            TryOrDefault<ParameterList>(&parameterListNoVararg), &block},
           MakeLabelBlock)};

  // Result: ExpressionWithSource
  Symbol expressionWithSource = {Rule({expression}, MakeExpressionWithSource)};

  // Result: RangeExpression
  Symbol rangeSpecifier = {
      Rule({Token("["), Optional<Expression*>(expression), Token(":"),
            Optional<Expression*>(expression), Token("]")},
           MakeRangeExpression)};

  // Result: Statement*
  Symbol varDeclaration = {
      Rule({OneOf({"let", "const"}), &identifier, Token(":"), &type},
           MakeVarDeclarationStatement)};

  // Result: Statement*
  Symbol varDeclarationWithInitialization = {
      Rule({OneOf({"let", "const"}), &identifier, Token(":"), &type, Token("="),
            expression},
           MakeVarDeclarationStatement)};

  // Disallow ambiguous dangling else by only allowing an {atomarStatement} as
  // a then-clause. Result: Statement*
  Symbol atomarStatement = {
      Rule({&block}),
      Rule({expression, Token(";")}, MakeExpressionStatement),
      Rule({Token("return"), Optional<Expression*>(expression), Token(";")},
           MakeReturnStatement),
      Rule({Token("tail"), &callExpression, Token(";")}, MakeTailCallStatement),
      Rule({Token("break"), Token(";")}, MakeBreakStatement),
      Rule({Token("continue"), Token(";")}, MakeContinueStatement),
      Rule({Token("goto"), &identifier,
            TryOrDefault<std::vector<Expression*>>(&argumentList), Token(";")},
           MakeGotoStatement),
      Rule({OneOf({"debug", "unreachable"}), Token(";")}, MakeDebugStatement)};

  // Result: Statement*
  Symbol statement = {
      Rule({&atomarStatement}),
      Rule({&varDeclaration, Token(";")}),
      Rule({&varDeclarationWithInitialization, Token(";")}),
      Rule({Token("if"), CheckIf(Token("constexpr")), Token("("), expression,
            Token(")"), &atomarStatement,
            Optional<Statement*>(Sequence({Token("else"), &statement}))},
           MakeIfStatement),
      Rule({Token("try"), &block, NonemptyList<LabelBlock*>(&labelBlock)},
           MakeTryLabelStatement),
      Rule({OneOf({"assert", "check"}), Token("("), &expressionWithSource,
            Token(")"), Token(";")},
           MakeAssertStatement),
      Rule({Token("while"), Token("("), expression, Token(")"),
            &atomarStatement},
           MakeWhileStatement),
      Rule({Token("for"), Token("("), &varDeclaration, Token("of"), expression,
            Optional<RangeExpression>(&rangeSpecifier), Token(")"),
            &atomarStatement},
           MakeForOfLoopStatement),
      Rule({Token("for"), Token("("),
            Optional<Statement*>(&varDeclarationWithInitialization), Token(";"),
            expression, Token(";"), expression, Token(")"), &atomarStatement},
           MakeForLoopStatement)};

  // Result: base::Optional<Statement*>
  Symbol optionalBody = {
      Rule({&block}, CastParseResult<Statement*, base::Optional<Statement*>>),
      Rule({Token(";")}, YieldDefaultValue<base::Optional<Statement*>>)};

  // Result: Declaration*
  Symbol declaration = {
      Rule({Token("const"), &identifier, Token(":"), &type, Token("="),
            expression, Token(";")},
           MakeConstDeclaration),
      Rule({Token("const"), &identifier, Token(":"), &type, Token("generates"),
            &externalString, Token(";")},
           MakeExternConstDeclaration),
      Rule({Token("type"), &identifier,
            Optional<std::string>(Sequence({Token("extends"), &identifier})),
            Optional<std::string>(
                Sequence({Token("generates"), &externalString})),
            Optional<std::string>(
                Sequence({Token("constexpr"), &externalString})),
            Token(";")},
           MakeTypeDeclaration),
      Rule({Token("type"), &identifier, Token("="), &type, Token(";")},
           MakeTypeAliasDeclaration),
      Rule({Token("extern"),
            Optional<std::string>(
                Sequence({Token("operator"), &externalString})),
            Token("macro"), &identifier,
            TryOrDefault<GenericParameters>(&genericParameters),
            &typeListMaybeVarArgs, &optionalReturnType, optionalLabelList,
            Token(";")},
           MakeExternalMacro),
      Rule({Token("extern"), CheckIf(Token("javascript")), Token("builtin"),
            &identifier, TryOrDefault<GenericParameters>(&genericParameters),
            &typeListMaybeVarArgs, &optionalReturnType, Token(";")},
           MakeExternalBuiltin),
      Rule({Token("extern"), Token("runtime"), &identifier,
            &typeListMaybeVarArgs, &optionalReturnType, Token(";")},
           MakeExternalRuntime),
      Rule({Optional<std::string>(
                Sequence({Token("operator"), &externalString})),
            Token("macro"), &identifier,
            TryOrDefault<GenericParameters>(&genericParameters),
            &parameterListNoVararg, &optionalReturnType, optionalLabelList,
            &optionalBody},
           MakeTorqueMacroDeclaration),
      Rule({CheckIf(Token("javascript")), Token("builtin"), &identifier,
            TryOrDefault<GenericParameters>(&genericParameters),
            &parameterListAllowVararg, &optionalReturnType, &optionalBody},
           MakeTorqueBuiltinDeclaration),
      Rule({&identifier, &genericSpecializationTypeList,
            &parameterListAllowVararg, &optionalReturnType, optionalLabelList,
            &block},
           MakeSpecializationDeclaration),
      Rule({Token("struct"), &identifier, Token("{"),
            List<NameAndTypeExpression>(Sequence({&nameAndType, Token(";")})),
            Token("}")},
           MakeStructDeclaration)};

  // Result: Declaration*
  Symbol moduleDeclaration = {
      Rule({Token("module"), &identifier, Token("{"),
            List<Declaration*>(&declaration), Token("}")},
           MakeExplicitModuleDeclaration)};

  Symbol file = {Rule({&file, &moduleDeclaration}, AddGlobalDeclaration),
                 Rule({&file, &declaration}, AddGlobalDeclaration), Rule({})};
};

}  // namespace

void ParseTorque(const std::string& input) { TorqueGrammar().Parse(input); }

}  // namespace torque
}  // namespace internal
}  // namespace v8
