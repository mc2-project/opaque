// -*- c-basic-offset: 2; fill-column: 100 -*-

#include <functional>
#include <typeinfo>

#include "Flatbuffers.h"

int printf(const char *fmt, ...);

#define check(test, ...) do {                   \
    bool result = test;                         \
    if (!result) {                              \
      printf(__VA_ARGS__);                      \
      assert(result);                           \
    }                                           \
  } while (0)

#ifndef EXPRESSION_EVALUATION_H
#define EXPRESSION_EVALUATION_H

using namespace edu::berkeley::cs::rise::opaque;

/**
 * Evaluate a binary arithmetic operation on two tuix::Fields. The operation (template parameter
 * Operation) must be a binary function object parameterized on its input type.
 *
 * The left and right Fields are the inputs to the binary operation. They may be temporary pointers
 * invalidated by further writes to builder; this function will not read them after invalidating.
 */
template<typename TuixExpr, template<typename T> class Operation>
flatbuffers::Offset<tuix::Field> eval_binary_arithmetic_op(
  flatbuffers::FlatBufferBuilder &builder,
  const tuix::Field *left,
  const tuix::Field *right) {

  check(left->value_type() == right->value_type(),
        "%s can't operate on values of different types (%s and %s)\n",
        typeid(TuixExpr).name(),
        tuix::EnumNameFieldUnion(left->value_type()),
        tuix::EnumNameFieldUnion(right->value_type()));
  bool result_is_null = left->is_null() || right->is_null();
  switch (left->value_type()) {
  case tuix::FieldUnion_IntegerField:
  {
    auto result = Operation<int32_t>()(
      static_cast<const tuix::IntegerField *>(left->value())->value(),
      static_cast<const tuix::IntegerField *>(right->value())->value());
    // Writing the result invalidates the left and right temporary pointers
    return tuix::CreateField(
      builder,
      tuix::FieldUnion_IntegerField,
      tuix::CreateIntegerField(builder, result).Union(),
      result_is_null);
  }
  case tuix::FieldUnion_LongField:
  {
    auto result = Operation<int64_t>()(
      static_cast<const tuix::LongField *>(left->value())->value(),
      static_cast<const tuix::LongField *>(right->value())->value());
    // Writing the result invalidates the left and right temporary pointers
    return tuix::CreateField(
      builder,
      tuix::FieldUnion_LongField,
      tuix::CreateLongField(builder, result).Union(),
      result_is_null);
  }
  case tuix::FieldUnion_FloatField:
  {
    auto result = Operation<float>()(
      static_cast<const tuix::FloatField *>(left->value())->value(),
      static_cast<const tuix::FloatField *>(right->value())->value());
    // Writing the result invalidates the left and right temporary pointers
    return tuix::CreateField(
      builder,
      tuix::FieldUnion_FloatField,
      tuix::CreateFloatField(builder, result).Union(),
      result_is_null);
  }
  case tuix::FieldUnion_DoubleField:
  {
    auto result = Operation<double>()(
      static_cast<const tuix::DoubleField *>(left->value())->value(),
      static_cast<const tuix::DoubleField *>(right->value())->value());
    // Writing the result invalidates the left and right temporary pointers
    return tuix::CreateField(
      builder,
      tuix::FieldUnion_DoubleField,
      tuix::CreateDoubleField(builder, result).Union(),
      result_is_null);
  }
  default:
    printf("Can't evaluate %s on %s\n",
           typeid(TuixExpr).name(),
           tuix::EnumNameFieldUnion(left->value_type()));
    assert(false);
    return flatbuffers::Offset<tuix::Field>();
  }
}

/**
 * Evaluate a binary number comparison operation on two tuix::Fields. The operation (template
 * parameter Operation) must be a binary function object parameterized on its input type.
 *
 * The left and right Fields are the inputs to the binary operation. They may be temporary pointers
 * invalidated by further writes to builder; this function will not read them after invalidating.
 */
template<typename TuixExpr, template<typename T> class Operation>
flatbuffers::Offset<tuix::Field> eval_binary_comparison(
  flatbuffers::FlatBufferBuilder &builder,
  const tuix::Field *left,
  const tuix::Field *right) {

  check(left->value_type() == right->value_type(),
        "%s can't operate on values of different types (%s and %s)\n",
        typeid(TuixExpr).name(),
        tuix::EnumNameFieldUnion(left->value_type()),
        tuix::EnumNameFieldUnion(right->value_type()));
  bool result_is_null = left->is_null() || right->is_null();
  bool result = false;
  if (!result_is_null) {
    switch (left->value_type()) {
    case tuix::FieldUnion_IntegerField:
    {
      result = Operation<int32_t>()(
        static_cast<const tuix::IntegerField *>(left->value())->value(),
        static_cast<const tuix::IntegerField *>(right->value())->value());
      break;
    }
    case tuix::FieldUnion_LongField:
    {
      result = Operation<int64_t>()(
        static_cast<const tuix::LongField *>(left->value())->value(),
        static_cast<const tuix::LongField *>(right->value())->value());
      break;
    }
    case tuix::FieldUnion_FloatField:
    {
      result = Operation<float>()(
        static_cast<const tuix::FloatField *>(left->value())->value(),
        static_cast<const tuix::FloatField *>(right->value())->value());
      break;
    }
    case tuix::FieldUnion_DoubleField:
    {
      result = Operation<double>()(
        static_cast<const tuix::DoubleField *>(left->value())->value(),
        static_cast<const tuix::DoubleField *>(right->value())->value());
      break;
    }
    default:
      printf("Can't evaluate %s on %s\n",
             typeid(TuixExpr).name(),
             tuix::EnumNameFieldUnion(left->value_type()));
      assert(false);
    }
  }
  // Writing the result invalidates the left and right temporary pointers
  return tuix::CreateField(
    builder,
    tuix::FieldUnion_BooleanField,
    tuix::CreateBooleanField(builder, result).Union(),
    result_is_null);

}

class FlatbuffersExpressionEvaluator {
public:
  FlatbuffersExpressionEvaluator(const tuix::Expr *expr) : builder(), expr(expr) {}

  /**
   * Evaluate the stored expression on the given row. Return a Field containing the result.
   * Warning: The Field points to internally-managed memory that may be overwritten the next time
   * eval is called. Therefore it is only valid until the next call to eval.
   */
  const tuix::Field *eval(const tuix::Row *row) {
    builder.Clear();
    flatbuffers::Offset<tuix::Field> result_offset = eval_helper(row, expr);
    return flatbuffers::GetTemporaryPointer<tuix::Field>(builder, result_offset);
  }

private:
  /**
   * Evaluate the given expression on the given row. Return the offset (within builder) of the Field
   * containing the result. This offset is only valid until the next call to eval.
   */
  flatbuffers::Offset<tuix::Field> eval_helper(const tuix::Row *row, const tuix::Expr *expr) {
    switch (expr->expr_type()) {
    case tuix::ExprUnion_Col:
    {
      uint32_t col_num = static_cast<const tuix::Col *>(expr->expr())->col_num();
      return flatbuffers_copy<tuix::Field>(
        row->field_values()->Get(col_num), builder);
    }

    case tuix::ExprUnion_Literal:
    {
      return flatbuffers_copy<tuix::Field>(
        static_cast<const tuix::Literal *>(expr->expr())->value(), builder);
    }

    // Arithmetic
    case tuix::ExprUnion_Add:
    {
      auto add = static_cast<const tuix::Add *>(expr->expr());
      return eval_binary_arithmetic_op<tuix::Add, std::plus>(
        builder,
        flatbuffers::GetTemporaryPointer(builder, eval_helper(row, add->left())),
        flatbuffers::GetTemporaryPointer(builder, eval_helper(row, add->right())));
    }

    case tuix::ExprUnion_Subtract:
    {
      auto subtract = static_cast<const tuix::Subtract *>(expr->expr());
      return eval_binary_arithmetic_op<tuix::Subtract, std::minus>(
        builder,
        flatbuffers::GetTemporaryPointer(builder, eval_helper(row, subtract->left())),
        flatbuffers::GetTemporaryPointer(builder, eval_helper(row, subtract->right())));
    }

    case tuix::ExprUnion_Multiply:
    {
      auto multiply = static_cast<const tuix::Multiply *>(expr->expr());
      return eval_binary_arithmetic_op<tuix::Multiply, std::multiplies>(
        builder,
        flatbuffers::GetTemporaryPointer(builder, eval_helper(row, multiply->left())),
        flatbuffers::GetTemporaryPointer(builder, eval_helper(row, multiply->right())));
    }

    // Predicates
    case tuix::ExprUnion_LessThan:
    {
      auto lt = static_cast<const tuix::LessThan *>(expr->expr());
      return eval_binary_comparison<tuix::LessThan, std::less>(
        builder,
        flatbuffers::GetTemporaryPointer(builder, eval_helper(row, lt->left())),
        flatbuffers::GetTemporaryPointer(builder, eval_helper(row, lt->right())));
    }

    case tuix::ExprUnion_LessThanOrEqual:
    {
      auto le = static_cast<const tuix::LessThanOrEqual *>(expr->expr());
      return eval_binary_comparison<tuix::LessThanOrEqual, std::less_equal>(
        builder,
        flatbuffers::GetTemporaryPointer(builder, eval_helper(row, le->left())),
        flatbuffers::GetTemporaryPointer(builder, eval_helper(row, le->right())));
    }

    case tuix::ExprUnion_GreaterThan:
    {
      auto gt = static_cast<const tuix::GreaterThan *>(expr->expr());
      return eval_binary_comparison<tuix::GreaterThan, std::greater>(
        builder,
        flatbuffers::GetTemporaryPointer(builder, eval_helper(row, gt->left())),
        flatbuffers::GetTemporaryPointer(builder, eval_helper(row, gt->right())));
    }

    case tuix::ExprUnion_GreaterThanOrEqual:
    {
      auto ge = static_cast<const tuix::GreaterThanOrEqual *>(expr->expr());
      return eval_binary_comparison<tuix::GreaterThanOrEqual, std::greater_equal>(
        builder,
        flatbuffers::GetTemporaryPointer(builder, eval_helper(row, ge->left())),
        flatbuffers::GetTemporaryPointer(builder, eval_helper(row, ge->right())));
    }

    case tuix::ExprUnion_EqualTo:
    {
      auto eq = static_cast<const tuix::EqualTo *>(expr->expr());
      return eval_binary_comparison<tuix::EqualTo, std::equal_to>(
        builder,
        flatbuffers::GetTemporaryPointer(builder, eval_helper(row, eq->left())),
        flatbuffers::GetTemporaryPointer(builder, eval_helper(row, eq->right())));
    }

    // String expressions
    case tuix::ExprUnion_Substring:
    {
      auto ss = static_cast<const tuix::Substring *>(expr->expr());
      // Note: These temporary pointers will be invalidated when we next write to builder
      const tuix::Field *str =
        flatbuffers::GetTemporaryPointer(builder, eval_helper(row, ss->str()));
      const tuix::Field *pos =
        flatbuffers::GetTemporaryPointer(builder, eval_helper(row, ss->pos()));
      const tuix::Field *len =
        flatbuffers::GetTemporaryPointer(builder, eval_helper(row, ss->len()));
      check(str->value_type() == tuix::FieldUnion_StringField &&
            pos->value_type() == tuix::FieldUnion_IntegerField &&
            len->value_type() == tuix::FieldUnion_IntegerField,
            "tuix::Substring requires str String, pos Integer, len Integer, not "
            "str %s, pos %s, len %s)\n",
            tuix::EnumNameFieldUnion(str->value_type()),
            tuix::EnumNameFieldUnion(pos->value_type()),
            tuix::EnumNameFieldUnion(len->value_type()));
      bool result_is_null = str->is_null() || pos->is_null() || len->is_null();
      if (!result_is_null) {
        auto str_field = static_cast<const tuix::StringField *>(str->value());
        auto pos_val = static_cast<const tuix::IntegerField *>(pos->value())->value();
        auto len_val = static_cast<const tuix::IntegerField *>(len->value())->value();
        auto start_idx = std::min(static_cast<uint32_t>(pos_val), str_field->length());
        auto end_idx = std::min(start_idx + len_val, str_field->length());
        // TODO: oblivious string lengths
        std::vector<uint8_t> substring(
          flatbuffers::VectorIterator<uint8_t, uint8_t>(str_field->value()->Data(),
                                                        start_idx),
          flatbuffers::VectorIterator<uint8_t, uint8_t>(str_field->value()->Data(),
                                                        end_idx));
        // Writing the result invalidates the str, pos, len temporary pointers
        return tuix::CreateField(
          builder,
          tuix::FieldUnion_StringField,
          tuix::CreateStringFieldDirect(builder, &substring, end_idx - start_idx).Union(),
          result_is_null);
      } else {
        // Writing the result invalidates the str, pos, len temporary pointers
        // TODO: oblivious string lengths
        return tuix::CreateField(
          builder,
          tuix::FieldUnion_StringField,
          tuix::CreateStringFieldDirect(builder, nullptr, 0).Union(),
          result_is_null);
      }
    }
    default:
      printf("Can't evaluate expression of type %s\n",
             tuix::EnumNameExprUnion(expr->expr_type()));
      assert(false);
      return flatbuffers::Offset<tuix::Field>();
    }
  }

  flatbuffers::FlatBufferBuilder builder;
  const tuix::Expr *expr;
};

class FlatbuffersSortOrderEvaluator {
public:
  FlatbuffersSortOrderEvaluator(const tuix::SortExpr *sort_expr)
    : sort_expr(sort_expr), builder() {
    for (auto sort_order_it = sort_expr->sort_order()->begin();
         sort_order_it != sort_expr->sort_order()->end(); ++sort_order_it) {
      sort_order_evaluators.emplace_back(
        std::unique_ptr<FlatbuffersExpressionEvaluator>(
          new FlatbuffersExpressionEvaluator(sort_order_it->child())));
    }
  }

  bool less_than(const tuix::Row *row1, const tuix::Row *row2) {
    builder.Clear();
    const tuix::Row *a = nullptr, *b = nullptr;
    printf("Comparing:\n");
    print(row1);
    print(row2);
    for (uint32_t i = 0; i < sort_order_evaluators.size(); i++) {
      printf("Comparing on sort order %d\n", i);
      switch (sort_expr->sort_order()->Get(i)->direction()) {
      case tuix::SortDirection_Ascending:
        a = row1;
        b = row2;
        break;
      case tuix::SortDirection_Descending:
        a = row2;
        b = row1;
        break;
      }

      const tuix::Field *a_eval_tmp = sort_order_evaluators[i]->eval(a);
      const tuix::Field *a_eval = flatbuffers::GetTemporaryPointer<tuix::Field>(
        builder, flatbuffers_copy(a_eval_tmp, builder));
      const tuix::Field *b_eval_tmp = sort_order_evaluators[i]->eval(b);
      const tuix::Field *b_eval = flatbuffers::GetTemporaryPointer<tuix::Field>(
        builder, flatbuffers_copy(b_eval_tmp, builder));
      printf("Field from a=");
      print(a_eval);
      printf(", field from b=");
      print(b_eval);
      printf("\n");

      bool a_less_than_b =
        static_cast<const tuix::BooleanField *>(
          flatbuffers::GetTemporaryPointer<tuix::Field>(
            builder,
            eval_binary_comparison<tuix::LessThan, std::less>(
              builder, a_eval, b_eval))
          ->value())->value();
      bool b_less_than_a =
        static_cast<const tuix::BooleanField *>(
          flatbuffers::GetTemporaryPointer<tuix::Field>(
            builder,
            eval_binary_comparison<tuix::LessThan, std::less>(
              builder, b_eval, a_eval))
          ->value())->value();

      if (a_less_than_b) {
        printf("a less than b\n");
        return true;
      } else if (b_less_than_a) {
        printf("a greater than b\n");
        return false;
      }
    }
    printf("a equal to b\n");
    return false;
  }

private:
  const tuix::SortExpr *sort_expr;
  flatbuffers::FlatBufferBuilder builder;
  std::vector<std::unique_ptr<FlatbuffersExpressionEvaluator>> sort_order_evaluators;
};

class FlatbuffersJoinExprEvaluator {
public:
  FlatbuffersJoinExprEvaluator(const tuix::JoinExpr *join_expr)
    : builder() {
    for (auto key_it = join_expr->left_keys()->begin();
         key_it != join_expr->left_keys()->end(); ++key_it) {
      left_key_evaluators.emplace_back(
        std::unique_ptr<FlatbuffersExpressionEvaluator>(
          new FlatbuffersExpressionEvaluator(*key_it)));
    }
    for (auto key_it = join_expr->right_keys()->begin();
         key_it != join_expr->right_keys()->end(); ++key_it) {
      right_key_evaluators.emplace_back(
        std::unique_ptr<FlatbuffersExpressionEvaluator>(
          new FlatbuffersExpressionEvaluator(*key_it)));
    }
  }

  /**
   * Return true if the given row is from the primary table, indicated by its first field, which
   * must be a BooleanField.
   */
  bool is_primary(const tuix::Row *row) {
    return static_cast<const tuix::BooleanField *>(
      row->field_values()->Get(0)->value())->value();
  }

  /** Return true if the two rows are from the same join group. */
  bool is_same_group(const tuix::Row *row1, const tuix::Row *row2) {
    builder.Clear();
    check(left_key_evaluators.size() == right_key_evaluators.size(),
          "Mismatched join key lengths\n");
    for (uint32_t i = 0; i < left_key_evaluators.size(); i++) {
      const tuix::Field *row1_eval_tmp = left_key_evaluators[i]->eval(row1);
      const tuix::Field *row1_eval = flatbuffers::GetTemporaryPointer<tuix::Field>(
        builder, flatbuffers_copy(row1_eval_tmp, builder));
      const tuix::Field *row2_eval_tmp = right_key_evaluators[i]->eval(row2);
      const tuix::Field *row2_eval = flatbuffers::GetTemporaryPointer<tuix::Field>(
        builder, flatbuffers_copy(row2_eval_tmp, builder));

      bool row1_equals_row2 =
        static_cast<const tuix::BooleanField *>(
          flatbuffers::GetTemporaryPointer<tuix::Field>(
            builder,
            eval_binary_comparison<tuix::EqualTo, std::equal_to>(
              builder, row1_eval, row2_eval))
          ->value())->value();

      if (!row1_equals_row2) {
        return false;
      }
    }
    return true;
  }

private:
  flatbuffers::FlatBufferBuilder builder;
  std::vector<std::unique_ptr<FlatbuffersExpressionEvaluator>> left_key_evaluators;
  std::vector<std::unique_ptr<FlatbuffersExpressionEvaluator>> right_key_evaluators;
};

#endif
