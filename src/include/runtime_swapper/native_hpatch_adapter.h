#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum runtime_swapper_hpatch_io_stage {
  runtime_swapper_hpatch_io_none = 0,
  runtime_swapper_hpatch_io_source_read = 1,
  runtime_swapper_hpatch_io_patch_read = 2,
  runtime_swapper_hpatch_io_output_read = 3,
  runtime_swapper_hpatch_io_output_write = 4,
  runtime_swapper_hpatch_io_output_size = 5,
  runtime_swapper_hpatch_io_output_flush = 6
};

typedef struct runtime_swapper_hpatch_diagnostics {
  int patch_result;
  int io_stage;
  int native_error;
  uint64_t position;
  uint64_t length;
  uint64_t expected_output_size;
  uint64_t actual_output_size;
} runtime_swapper_hpatch_diagnostics;

int runtime_swapper_hpatch_handles(
    intptr_t source_handle, intptr_t patch_handle, intptr_t output_handle,
    int thread_count, runtime_swapper_hpatch_diagnostics* diagnostics);

#ifdef __cplusplus
}
#endif
