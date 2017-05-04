#include "Sort.h"

#include <algorithm>
#include <memory>
#include <queue>

template<typename RecordType>
uint32_t sort_single_buffer(
  int op_code, Verify *verify_set,
  uint8_t *buffer, uint8_t *buffer_end,
  uint8_t *write_buffer,
  uint32_t num_rows, SortPointer<RecordType> *sort_ptrs,
  uint32_t sort_ptrs_len, uint32_t row_upper_bound, uint32_t *num_comparisons,
  uint32_t *num_deep_comparisons) {

  check(sort_ptrs_len >= num_rows,
        "sort_single_buffer: sort_ptrs is not large enough (%d vs %d)\n", sort_ptrs_len, num_rows);

  RowReader r(buffer, buffer_end, verify_set);
  for (uint32_t i = 0; i < num_rows; i++) {
    r.read(&sort_ptrs[i], op_code);
  }

  std::sort(
    sort_ptrs, sort_ptrs + num_rows,
    [op_code, num_comparisons, num_deep_comparisons](const SortPointer<RecordType> &a,
                                                     const SortPointer<RecordType> &b) {
      (*num_comparisons)++;
      return a.less_than(&b, op_code, num_deep_comparisons);
    });

  RowWriter w(write_buffer, row_upper_bound);
  w.set_self_task_id(verify_set->get_self_task_id());
  for (uint32_t i = 0; i < num_rows; i++) {
    w.write(&sort_ptrs[i]);
  }
  w.close();
  printf("[%s] bytes read: %u, bytes_written is %u, buffer + byteswritten: %p\n", __FUNCTION__, (uint32_t)(buffer_end - buffer), w.bytes_written(), buffer+w.bytes_written());
  return w.bytes_written();
}

template<typename RecordType>
class MergeItem {
 public:
  SortPointer<RecordType> v;
  uint32_t reader_idx;
};

template<typename RecordType>
uint32_t external_merge(int op_code,
						Verify *verify_set,
						std::vector<uint8_t *> &runs,
						std::vector<uint8_t *> &run_ends,
						uint32_t run_start,
						uint32_t num_runs,
						SortPointer<RecordType> *sort_ptrs,
						uint32_t sort_ptrs_len,
						uint32_t row_upper_bound,
						uint8_t *scratch,
						uint32_t *num_comparisons,
						uint32_t *num_deep_comparisons) {

  check(sort_ptrs_len >= num_runs,
        "external_merge: sort_ptrs is not large enough (%d vs %d)\n", sort_ptrs_len, num_runs);

  debug("[%s] row_upper_bound is %u\n", __FUNCTION__, row_upper_bound);

  std::vector<StreamRowReader *> readers;
  for (uint32_t i = 0; i < num_runs; i++) {
	debug("[%s] constructing reader on %p to %p\n", __FUNCTION__, runs[run_start + i], run_ends[run_start + i]);
    readers.push_back(new StreamRowReader(runs[run_start + i], run_ends[run_start + i]));
  }

  auto compare = [op_code, num_comparisons, num_deep_comparisons](const MergeItem<RecordType> &a,
                                                                  const MergeItem<RecordType> &b) {
    (*num_comparisons)++;
    return b.v.less_than(&a.v, op_code, num_deep_comparisons);
  };
  std::priority_queue<MergeItem<RecordType>, std::vector<MergeItem<RecordType>>, decltype(compare)>
    queue(compare);
  debug("[%s] created priority queue\n", __FUNCTION__);
  for (uint32_t i = 0; i < num_runs; i++) {
	printf("[%s] run %u\n", __FUNCTION__, i);
    MergeItem<RecordType> item;
    item.v = sort_ptrs[i];
    readers[i]->read(&item.v, op_code);
    item.reader_idx = i;
    queue.push(item);
  }

  debug("[%s] merge start\n", __FUNCTION__);

  // Sort the runs into scratch
  RowWriter w(scratch, row_upper_bound);
  w.set_self_task_id(verify_set->get_self_task_id());
  while (!queue.empty()) {
    MergeItem<RecordType> item = queue.top();
    queue.pop();
    w.write(&item.v);
	//item.v.print()n;

    // Read another row from the same run that this one came from
    if (readers[item.reader_idx]->has_next()) {
      readers[item.reader_idx]->read(&item.v, op_code);
      queue.push(item);
    }
  }
  w.close();

  debug("[%s] merge is done\n", __FUNCTION__);

  // Overwrite the runs with scratch, merging them into one big run
  //memcpy(runs[run_start], scratch, w.bytes_written());

  for (uint32_t i = 0; i < num_runs; i++) {
    delete readers[i];
  }

  debug("[%s] readers are deleted, bytes written is %u\n", __FUNCTION__, w.bytes_written());
  return w.bytes_written();
}

template<typename RecordType>
void external_sort(int op_code,
                   Verify *verify_set,
                   uint32_t num_buffers,
                   uint8_t **buffer_list,
                   uint32_t *num_rows,
                   uint32_t row_upper_bound,
                   uint8_t *scratch,
				   uint32_t *final_len) {

  // Maximum number of rows we will need to store in memory at a time: the contents of the largest
  // buffer

  uint32_t max_num_rows = 0;
  for (uint32_t i = 0; i < num_buffers; i++) {
    if (max_num_rows < num_rows[i]) {
      max_num_rows = num_rows[i];
    }
  }
  uint32_t max_list_length = std::max(max_num_rows, MAX_NUM_STREAMS);

  // Actual record data, in arbitrary and unchanging order
  RecordType *data = (RecordType *) malloc(max_list_length * sizeof(RecordType));
  for (uint32_t i = 0; i < max_list_length; i++) {
    new(&data[i]) RecordType(row_upper_bound);
  }

  // Pointers to the record data. Only the pointers will be sorted, not the records themselves
  SortPointer<RecordType> *sort_ptrs = new SortPointer<RecordType>[max_list_length];
  for (uint32_t i = 0; i < max_list_length; i++) {
    sort_ptrs[i].init(&data[i]);
  }

  uint32_t num_comparisons = 0, num_deep_comparisons = 0;

  // Each buffer now forms a sorted run. Keep a pointer to the beginning of each run, plus a
  // sentinel pointer to the end of the last run
  std::vector<uint8_t *> runs;
  std::vector<uint8_t *> run_ends;

  // Sort each buffer individually
  uint32_t ss_offset = 0;
  runs.push_back(buffer_list[0]);
  for (uint32_t i = 0; i < num_buffers; i++) {
    debug("[%s] Sorting buffer %d with %d rows, out of %d buffers, opcode %d, buffer_list[i]: %p\n", 
		  __FUNCTION__, i, num_rows[i], num_buffers, op_code, buffer_list[i]);

    ss_offset += sort_single_buffer(op_code, verify_set,
									buffer_list[i], buffer_list[i+1], 
									scratch + ss_offset, num_rows[i], sort_ptrs, max_list_length,
									row_upper_bound, &num_comparisons, &num_deep_comparisons);

	if (i > 0) {
	  runs.push_back(run_ends.back());
	}
	run_ends.push_back(buffer_list[0] + ss_offset);
	printf("[%s] run %p, run_end %p\n", __FUNCTION__, runs[i], run_ends[i]);
  }
  memcpy(buffer_list[0], scratch, ss_offset);

  // Merge sorted runs, merging up to MAX_NUM_STREAMS runs at a time
  while (runs.size() > 1) {
    perf("external_sort: Merging %d runs, up to %d at a time\n",
         runs.size(), MAX_NUM_STREAMS);

    std::vector<uint8_t *> new_runs;
    std::vector<uint8_t *> new_run_ends;
	uint32_t offset_count = 0;
	new_runs.push_back(buffer_list[0]);
    for (uint32_t run_start = 0; run_start < runs.size(); run_start += MAX_NUM_STREAMS) {
      uint32_t num_runs =
        std::min(MAX_NUM_STREAMS, static_cast<uint32_t>(runs.size()) - run_start);

      debug("external_sort: Merging buffers %d-%d\n", run_start, run_start + num_runs - 1);

      offset_count += external_merge<RecordType>(op_code, verify_set,
												 runs, run_ends, 
												 run_start, num_runs, sort_ptrs, 
												 max_list_length, row_upper_bound, scratch+offset_count,
												 &num_comparisons, &num_deep_comparisons);

	  printf("[%s] offset_count is %u\n", __FUNCTION__, offset_count);
	  if (run_start > 0) {
		new_runs.push_back(new_run_ends.back());
	  }
      new_run_ends.push_back(buffer_list[0] + offset_count);
    }
    //new_runs.push_back(runs[runs.size() - 1]); // copy over the sentinel pointer

	memcpy(buffer_list[0], scratch, offset_count);
    runs = new_runs;
    run_ends = new_run_ends;
	*final_len = offset_count;
  }

  perf("external_sort: %d comparisons, %d deep comparisons\n",
       num_comparisons, num_deep_comparisons);

  delete[] sort_ptrs;
  for (uint32_t i = 0; i < max_list_length; i++) {
    data[i].~RecordType();
  }
  free(data);
}

template<typename RecordType>
void sample(Verify *verify_set,
            uint8_t *input_rows,
            uint32_t input_rows_len,
            uint32_t num_rows,
			uint8_t *output_rows,
            uint32_t *output_rows_size,
            uint32_t *num_output_rows) {

  uint32_t row_upper_bound = 0;
  {
    BlockReader b(input_rows, input_rows_len);
    uint8_t *block;
    uint32_t len, num_rows, result;
    b.read(&block, &len, &num_rows, &result);
    if (block == NULL) {
      *output_rows_size = 0;
      return;
    } else {
      row_upper_bound = result;
    }
  }
  
  // Sample ~5% of the rows or 1000 rows, whichever is greater
  unsigned char buf[2];
  uint16_t *buf_ptr = (uint16_t *) buf;

  uint16_t sampling_ratio;
  if (num_rows > 1000 * 20) {
    sampling_ratio = 3276; // 5% of 2^16
  } else {
    sampling_ratio = 16383;
  }

  RowReader r(input_rows, input_rows + input_rows_len, verify_set);
  RowWriter w(output_rows, row_upper_bound);
  w.set_self_task_id(verify_set->get_self_task_id());
  RecordType row;
  uint32_t num_output_rows_result = 0;
  for (uint32_t i = 0; i < num_rows; i++) {
    r.read(&row);
    sgx_read_rand(buf, 2);
    if (*buf_ptr <= sampling_ratio) {
      w.write(&row);
      num_output_rows_result++;
    }
  }

  w.close();
  *output_rows_size = w.bytes_written();
  *num_output_rows = num_output_rows_result;
}

template<typename RecordType>
void find_range_bounds(int op_code,
                       Verify *verify_set,
                       uint32_t num_partitions,
					   uint32_t num_buffers,
                       uint8_t **buffer_list,
                       uint32_t *num_rows,
                       uint32_t row_upper_bound,
                       uint8_t *output_rows,
                       uint32_t *output_rows_len,
                       uint8_t *scratch) {

  uint32_t final_len = 0;
  // Sort the input rows
  external_sort<RecordType>(op_code, verify_set, num_buffers, buffer_list, num_rows, row_upper_bound, scratch, &final_len);

  // Split them into one range per partition
  uint32_t total_num_rows = 0;
  for (uint32_t i = 0; i < num_buffers; i++) {
    total_num_rows += num_rows[i];
  }
  uint32_t num_rows_per_part = total_num_rows / num_partitions;

  RowReader r(buffer_list[0], buffer_list[num_buffers]);
  RowWriter w(output_rows, row_upper_bound);
  w.set_self_task_id(verify_set->get_self_task_id());
  RecordType row;
  uint32_t current_rows_in_part = 0;
  for (uint32_t i = 0; i < total_num_rows; i++) {
    r.read(&row);
    if (current_rows_in_part == num_rows_per_part) {
      w.write(&row);
      current_rows_in_part = 0;
	} else {
	  ++current_rows_in_part;
	}
  }

  w.close();
  *output_rows_len = w.bytes_written();
}

template<typename RecordType>
void partition_for_sort(int op_code,
                        Verify *verify_set,
                        uint8_t num_partitions,
                        uint32_t num_buffers,
                        uint8_t **buffer_list,
                        uint32_t *num_rows,
                        uint32_t row_upper_bound,
                        uint8_t *boundary_rows,
                        uint32_t boundary_rows_len,
                        uint8_t *output,
                        uint8_t **output_partition_ptrs,
                        uint32_t *output_partition_num_rows) {

  uint32_t input_length = buffer_list[num_buffers] - buffer_list[0];

  uint32_t total_num_rows = 0;
  for (uint32_t i = 0; i < num_buffers; ++i) {
    total_num_rows += num_rows[i];
  }

  std::vector<uint8_t *> tmp_output(num_partitions);
  std::vector<RowWriter *> writers(num_partitions);
  for (uint32_t i = 0; i < num_partitions; ++i) {
    // Worst case size for one output partition: the entire input length
    tmp_output[i] = new uint8_t[input_length];
    writers[i] = new RowWriter(tmp_output[i], row_upper_bound);
    writers[i]->set_self_task_id(verify_set->get_self_task_id());
  }

  // Read the (num_partitions - 1) boundary rows into memory for efficient repeated scans
  std::vector<RecordType *> boundary_row_records;
  RowReader b(boundary_rows, boundary_rows + boundary_rows_len, verify_set);
  for (uint32_t i = 0; i < num_partitions - 1; ++i) {
    boundary_row_records.push_back(new RecordType);
    b.read(boundary_row_records[i]);
    boundary_row_records[i]->print();
  }

  // Scan through the input rows and copy each to the appropriate output partition specified by the
  // ranges encoded in the given boundary_rows. A range contains all rows greater than or equal to
  // one boundary row and less than the next boundary row. The first range contains all rows less
  // than the first boundary row, and the last range contains all rows greater than or equal to the
  // last boundary row.
  //
  // We currently scan through all boundary rows sequentially for each row. TODO: consider using
  // binary search.
  RowReader r(buffer_list[0], buffer_list[num_buffers]);
  RecordType row;

  for (uint32_t i = 0; i < total_num_rows; ++i) {
    r.read(&row);

    // Scan to the matching boundary row for this row
    uint32_t j;
    for (j = 0; j < num_partitions - 1; ++j) {
      if (row.less_than(boundary_row_records[j], op_code)) {
        // Found an upper-bounding boundary row
        break;
      }
    }
    // If the row is greater than all boundary rows, the above loop will never break and j will end
    // up at num_partitions - 1.
    writers[j]->write(&row);
  }

  for (uint32_t i = 0; i < num_partitions - 1; ++i) {
    delete boundary_row_records[i];
  }

  // Copy the partitions to the output
  uint8_t *output_ptr = output;
  for (uint32_t i = 0; i < num_partitions; ++i) {
    writers[i]->close();

    memcpy(output_ptr, tmp_output[i], writers[i]->bytes_written());
    output_partition_ptrs[i] = output_ptr;
    output_partition_ptrs[i + 1] = output_ptr + writers[i]->bytes_written();
    output_partition_num_rows[i] = writers[i]->rows_written();

    debug("Writing %d bytes to output %p based on input of length %d. Total bytes written %d. Upper bound %d.\n",
          writers[i]->bytes_written(), output_ptr, input_length,
          output_ptr + writers[i]->bytes_written() - output, num_partitions * input_length);
    output_ptr += writers[i]->bytes_written();
    check(writers[i]->bytes_written() <= input_length,
          "output partition size %d was bigger than input size %d\n",
          writers[i]->bytes_written(), input_length);

    delete writers[i];
    delete[] tmp_output[i];
  }
}

template void external_sort<NewRecord>(
  int op_code,
  Verify *verify_set,
  uint32_t num_buffers,
  uint8_t **buffer_list,
  uint32_t *num_rows,
  uint32_t row_upper_bound,
  uint8_t *scratch,
  uint32_t *final_len);

template void external_sort<NewJoinRecord>(
  int op_code,
  Verify *verify_set,
  uint32_t num_buffers,
  uint8_t **buffer_list,
  uint32_t *num_rows,
  uint32_t row_upper_bound,
  uint8_t *scratch,
  uint32_t *final_len);

template void sample<NewRecord>(
  Verify *verify_set,
  uint8_t *input_rows,
  uint32_t input_rows_len,
  uint32_t num_rows,
  uint8_t *output_rows,
  uint32_t *output_rows_size,
  uint32_t *num_output_rows);

template void sample<NewJoinRecord>(
  Verify *verify_set,
  uint8_t *input_rows,
  uint32_t input_rows_len,
  uint32_t num_rows,
  uint8_t *output_rows,
  uint32_t *output_rows_size,
  uint32_t *num_output_rows);

template void find_range_bounds<NewRecord>(
  int op_code,
  Verify *verify_set,
  uint32_t num_partitions,
  uint32_t num_buffers,
  uint8_t **buffer_list,
  uint32_t *num_rows,
  uint32_t row_upper_bound,
  uint8_t *output_rows,
  uint32_t *output_rows_len,
  uint8_t *scratch);

template void find_range_bounds<NewJoinRecord>(
  int op_code,
  Verify *verify_set,
  uint32_t num_partitions,
  uint32_t num_buffers,
  uint8_t **buffer_list,
  uint32_t *num_rows,
  uint32_t row_upper_bound,
  uint8_t *output_rows,
  uint32_t *output_rows_len,
  uint8_t *scratch);

template void partition_for_sort<NewRecord>(
  int op_code,
  Verify *verify_set,
  uint8_t num_partitions,
  uint32_t num_buffers,
  uint8_t **buffer_list,
  uint32_t *num_rows,
  uint32_t row_upper_bound,
  uint8_t *boundary_rows,
  uint32_t boundary_rows_len,
  uint8_t *output,
  uint8_t **output_partition_ptrs,
  uint32_t *output_partition_num_rows);

template void partition_for_sort<NewJoinRecord>(
  int op_code,
  Verify *verify_set,
  uint8_t num_partitions,
  uint32_t num_buffers,
  uint8_t **buffer_list,
  uint32_t *num_rows,
  uint32_t row_upper_bound,
  uint8_t *boundary_rows,
  uint32_t boundary_rows_len,
  uint8_t *output,
  uint8_t **output_partition_ptrs,
  uint32_t *output_partition_num_rows);
