#include "dirDiffPatch/dir_patch/dir_patch.h"
#include "libHDiffPatch/HPatch/patch_types.h"

#include <stddef.h>

int hpatch(const char* old_file_name, const char* diff_file_name,
           const char* out_new_file_name, hpatch_BOOL is_load_old_all,
           size_t patch_cache_size, hpatch_StreamPos_t diff_data_offset,
           hpatch_StreamPos_t diff_data_size, TPatchChecksumSet* checksum_set,
           size_t thread_count, size_t decompress_thread_count);

int runtime_swapper_hpatch_file(const char* source, const char* patch,
                                const char* output) {
  TPatchChecksumSet checksum_set = {
      0, hpatch_FALSE, hpatch_TRUE, hpatch_TRUE, hpatch_FALSE};
  return hpatch(source, patch, output, hpatch_FALSE, 8U * 1024U * 1024U, 0, 0,
                &checksum_set, 5, 1);
}
