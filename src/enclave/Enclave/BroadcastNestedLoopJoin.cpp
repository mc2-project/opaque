#include "BroadcastNestedLoopJoin.h"

#include "ExpressionEvaluation.h"
#include "FlatbuffersReaders.h"
#include "FlatbuffersWriters.h"
#include "common.h"

/** C++ implementation of a broadcast nested loop join.
 * Assumes outer_rows is streamed and inner_rows is broadcast.
 * DOES NOT rely on rows to be tagged primary or secondary, and that
 * assumption will break the implementation.
 */
void broadcast_nested_loop_join(
  uint8_t *join_expr, size_t join_expr_length,
  uint8_t *outer_rows, size_t outer_rows_length,
  uint8_t *inner_rows, size_t inner_rows_length,
  uint8_t **output_rows, size_t *output_rows_length) {

  FlatbuffersJoinExprEvaluator join_expr_eval(join_expr, join_expr_length);
  const tuix::JoinType join_type = join_expr_eval.get_join_type();

  switch(join_type) {
    case tuix::JoinType_LeftAnti:
      default_join(join_expr, join_expr_length,
                  outer_rows, outer_rows_length,
                  inner_rows, inner_rows_length,
                  output_rows, output_rows_length);
      break;
    case tuix::JoinType_LeftOuter:
      outer_join(join_expr, join_expr_length,
                outer_rows, outer_rows_length,
                inner_rows, inner_rows_length,
                output_rows, output_rows_length);
      break;
    default:
      throw std::runtime_error(
        std::string("Join type not supported: ")
        + std::string(to_string(join_type)));
  }
}

void outer_join(
  uint8_t *join_expr, size_t join_expr_length,
  uint8_t *outer_rows, size_t outer_rows_length,
  uint8_t *inner_rows, size_t inner_rows_length,
  uint8_t **output_rows, size_t *output_rows_length) {

  FlatbuffersJoinExprEvaluator join_expr_eval(join_expr, join_expr_length);
  const tuix::JoinType join_type = join_expr_eval.get_join_type();

  RowReader outer_r(BufferRefView<tuix::EncryptedBlocks>(outer_rows, outer_rows_length));
  RowWriter w;

  FlatbuffersTemporaryRow last_inner;

  while (outer_r.has_next()) {
    const tuix::Row *outer = outer_r.next();
    bool o_i_match = false;
    RowReader inner_r(BufferRefView<tuix::EncryptedBlocks>(inner_rows, inner_rows_length));

    // Use peek() to get the schema of the inner table
    const tuix::Row *inner = inner_r.peek();

    while (inner_r.has_next()) {
      inner = inner_r.next();
      bool condition_met = join_expr_eval.eval_condition(outer, inner);
      if (condition_met) {
        switch(join_type) {
          case tuix::JoinType_LeftOuter:
            w.append(outer, inner);
          default:
            break;
        }
      }
      o_i_match |= condition_met;
    }

    switch(join_type) {
      case tuix::JoinType_LeftOuter:
        if (!o_i_match) {
          // Values of inner do not matter: they are all set to null
          w.append(outer, inner, false, true);
        }
        break;
      default:
        break;
    }
  }
  w.output_buffer(output_rows, output_rows_length);
}

void default_join(
  uint8_t *join_expr, size_t join_expr_length,
  uint8_t *outer_rows, size_t outer_rows_length,
  uint8_t *inner_rows, size_t inner_rows_length,
  uint8_t **output_rows, size_t *output_rows_length) {

  FlatbuffersJoinExprEvaluator join_expr_eval(join_expr, join_expr_length);
  const tuix::JoinType join_type = join_expr_eval.get_join_type();

  RowReader outer_r(BufferRefView<tuix::EncryptedBlocks>(outer_rows, outer_rows_length));
  RowWriter w;

  while (outer_r.has_next()) {
    const tuix::Row *outer = outer_r.next();
    bool o_i_match = false;

    RowReader inner_r(BufferRefView<tuix::EncryptedBlocks>(inner_rows, inner_rows_length));
    const tuix::Row *inner;
    while (inner_r.has_next()) {
      inner = inner_r.next();
      o_i_match |= join_expr_eval.eval_condition(outer, inner);
    }

    switch(join_type) {
      case tuix::JoinType_LeftAnti:
        if (!o_i_match) {
          w.append(outer);
        }
        break;
      default:
        break;
    }
  }
  w.output_buffer(output_rows, output_rows_length);
}
