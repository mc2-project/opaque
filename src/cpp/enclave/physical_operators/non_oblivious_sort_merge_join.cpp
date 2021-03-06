#include "non_oblivious_sort_merge_join.h"

#include "common.h"
#include "flatbuffer_helpers/expression_evaluation.h"
#include "flatbuffer_helpers/flatbuffers_readers.h"
#include "flatbuffer_helpers/flatbuffers_writers.h"

/**
 * C++ implementation of a non-oblivious sort merge join.
 * Rows MUST be tagged primary or secondary for this to work.
 */

void test_rows_same_group(FlatbuffersJoinExprEvaluator &join_expr_eval, const tuix::Row *primary,
                          const tuix::Row *current) {
  if (!join_expr_eval.is_same_group(primary, current)) {
    throw std::runtime_error(std::string("Invariant violation: rows of primary_group "
                                         "are not of the same group: ") +
                             to_string(primary) + std::string(" vs ") + to_string(current));
  }
}

void write_output_rows(RowWriter &input, RowWriter &output, tuix::JoinType join_type,
                       const tuix::Row *foreign_row = nullptr) {
  auto input_buffer = input.output_buffer();
  RowReader input_reader(input_buffer.view());

  while (input_reader.has_next()) {
    const tuix::Row *row = input_reader.next();
    if (foreign_row == nullptr) {
      output.append(row);
    } else if (join_type == tuix::JoinType_FullOuter || join_type == tuix::JoinType_LeftOuter) {
      output.append(row, foreign_row, false, true);
    } else if (join_type == tuix::JoinType_RightOuter) {
      output.append(foreign_row, row, true, false);
    } else {
      throw std::runtime_error(
          std::string("write_output_rows should not take a foreign row with join type ") +
          to_string(join_type));
    }
  }
}

/**
 * Sort merge equi join algorithm
 * Input: the rows are unioned from both the primary (or left) table and the
 * non-primary (or right) table
 *
 * Outer loop: iterate over all input rows
 *
 * If it's a row from the left table
 * - Add it to the current group
 * - Otherwise start a new group
 *   - If it's a left semi/anti join, output the
 * primary_matched_rows/primary_unmatched_rows
 *
 * If it's a row from the right table
 * - Inner join: iterate over current left group, output the joined row only if
 * the condition is satisfied
 * - Left semi/anti join: iterate over `primary_unmatched_rows`, add a matched
 * row to `primary_matched_rows` and remove from `primary_unmatched_rows`
 *
 * After loop: output the last group left semi/anti join
 */

void non_oblivious_sort_merge_join(uint8_t *join_expr, size_t join_expr_length,
                                   uint8_t *input_rows, size_t input_rows_length,
                                   uint8_t **output_rows, size_t *output_rows_length) {
  FlatbuffersJoinExprEvaluator join_expr_eval(join_expr, join_expr_length);
  tuix::JoinType join_type = join_expr_eval.get_join_type();
  RowReader r(BufferRefView<tuix::EncryptedBlocks>(input_rows, input_rows_length));
  RowWriter w;

  RowWriter primary_group;
  RowWriter primary_matched_rows,
      primary_unmatched_rows; // These are used for all joins but inner
  FlatbuffersTemporaryRow last_primary_of_group;

  // Used for outer rows to get the schema of the foreign table.
  // A "dummy" row with the desired schema is added for each partition,
  // so dummy_foreign_row.get() is guaranteed to not be null.
  FlatbuffersTemporaryRow dummy_foreign_row;

  // A "dummy" row needed for FullOuter joins so that
  // dummy_primary_row.get() is guaranteed to not be null
  FlatbuffersTemporaryRow dummy_primary_row;

  while (r.has_next()) {
    const tuix::Row *current = r.next();

    if (current->is_dummy()) {
      // For FullOuter join, dummy rows for both primary and foreign tables are provided.
      // Scala code ensures that the primary table dummy row appears first
      // and the foreign table dummy row appears second
      if (join_type == tuix::JoinType_FullOuter) {
        if (dummy_primary_row.get() == nullptr) {
          dummy_primary_row.set(current);
        } else if (dummy_foreign_row.get() == nullptr) {
          dummy_foreign_row.set(current);
        }
      } else {
        // Only a single foreign table dummy row provided for non-FullOuter join
        dummy_foreign_row.set(current);
      }

      continue;
    }

    if (join_expr_eval.is_primary(current)) {
      if (last_primary_of_group.get() &&
          join_expr_eval.is_same_group(last_primary_of_group.get(), current)) {

        // Add this primary row to the current group
        primary_group.append(current);
        if (join_type != tuix::JoinType_Inner) {
          primary_unmatched_rows.append(current);
        }
        last_primary_of_group.set(current);

      } else {
        // If a new primary group is encountered
        if (join_type == tuix::JoinType_LeftSemi) {
          write_output_rows(primary_matched_rows, w, join_type);
        } else if (join_type == tuix::JoinType_LeftAnti) {
          write_output_rows(primary_unmatched_rows, w, join_type);
        } else if (join_expr_eval.is_outer_join()) {
          // Dummy row is always guaranteed to be the first row, so dummy_foreign_row.get() cannot
          // be null.
          write_output_rows(primary_unmatched_rows, w, join_type, dummy_foreign_row.get());
        }

        primary_group.clear();
        primary_unmatched_rows.clear();
        primary_matched_rows.clear();

        primary_group.append(current);
        primary_unmatched_rows.append(current);
        last_primary_of_group.set(current);
      }
    } else {
      if (last_primary_of_group.get() &&
          join_expr_eval.is_same_group(last_primary_of_group.get(), current)) {
        if (join_type == tuix::JoinType_Inner || join_expr_eval.is_outer_join()) {
          auto primary_group_buffer = primary_group.output_buffer();
          RowReader primary_group_reader(primary_group_buffer.view());

          bool match_found = false;
          while (primary_group_reader.has_next()) {
            const tuix::Row *primary = primary_group_reader.next();
            test_rows_same_group(join_expr_eval, primary, current);

            if (join_expr_eval.eval_condition(primary, current)) {
              match_found = true;
              if (join_expr_eval.is_right_join()) {
                w.append(current, primary);
              } else {
                w.append(primary, current);
              }
            }
          }
          // Join condition not satisfied for any primary group rows; add (nulls, foreign row) to
          // output
          if (join_type == tuix::JoinType_FullOuter && !match_found) {
            w.append(dummy_primary_row.get(), current, true, false);
          }
        }
        if (join_type != tuix::JoinType_Inner) {
          auto primary_unmatched_rows_buffer = primary_unmatched_rows.output_buffer();
          RowReader primary_unmatched_rows_reader(primary_unmatched_rows_buffer.view());
          RowWriter new_primary_unmatched_rows;

          while (primary_unmatched_rows_reader.has_next()) {
            const tuix::Row *primary = primary_unmatched_rows_reader.next();
            test_rows_same_group(join_expr_eval, primary, current);
            if (join_expr_eval.eval_condition(primary, current)) {
              primary_matched_rows.append(primary);
            } else {
              new_primary_unmatched_rows.append(primary);
            }
          }

          // Reset primary_unmatched_rows
          primary_unmatched_rows.clear();
          write_output_rows(new_primary_unmatched_rows, primary_unmatched_rows, join_type);
        }
      } else if (join_type == tuix::JoinType_FullOuter) {
        // No match found for foreign row; need to add (nulls, foreign row) to output
        w.append(dummy_primary_row.get(), current, true, false);
      }
    }
  }

  switch (join_type) {
  case tuix::JoinType_LeftSemi:
    write_output_rows(primary_matched_rows, w, join_type);
    break;
  case tuix::JoinType_LeftAnti:
    write_output_rows(primary_unmatched_rows, w, join_type);
    break;
  case tuix::JoinType_FullOuter:
  case tuix::JoinType_LeftOuter:
  case tuix::JoinType_RightOuter:
    write_output_rows(primary_unmatched_rows, w, join_type, dummy_foreign_row.get());
    break;
  default:
    break;
  }

  w.output_buffer(output_rows, output_rows_length);
}
