// Keep the upstream parser and patch implementation in this translation unit
// so the SRS adapter can feed it already-opened files. This prevents HDiffPatch
// from resolving security-sensitive paths a second time after verification.
#include "hpatchz.c"

#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

typedef struct runtime_swapper_native_input {
  hpatch_TStreamInput base;
  intptr_t handle;
  int io_error;
#if defined(_WIN32)
  CRITICAL_SECTION lock;
#endif
} runtime_swapper_native_input;

typedef struct runtime_swapper_native_output {
  hpatch_TStreamOutput base;
  intptr_t handle;
  int io_error;
#if defined(_WIN32)
  CRITICAL_SECTION lock;
#endif
} runtime_swapper_native_output;

static void runtime_swapper_set_input_error(
    runtime_swapper_native_input* stream) {
#if defined(_WIN32)
  InterlockedExchange((volatile LONG*)&stream->io_error, 1);
#else
  __atomic_store_n(&stream->io_error, 1, __ATOMIC_RELAXED);
#endif
}

static void runtime_swapper_set_output_error(
    runtime_swapper_native_output* stream) {
#if defined(_WIN32)
  InterlockedExchange((volatile LONG*)&stream->io_error, 1);
#else
  __atomic_store_n(&stream->io_error, 1, __ATOMIC_RELAXED);
#endif
}

static hpatch_BOOL runtime_swapper_read_native(
    intptr_t native_handle, hpatch_StreamPos_t position, unsigned char* data,
    unsigned char* data_end, int* io_error
#if defined(_WIN32)
    , CRITICAL_SECTION* lock
#endif
) {
  size_t remaining;
  if (data > data_end) return hpatch_FALSE;
  remaining = (size_t)(data_end - data);
#if defined(_WIN32)
  EnterCriticalSection(lock);
  {
    HANDLE handle = (HANDLE)native_handle;
    LARGE_INTEGER offset;
    offset.QuadPart = (LONGLONG)position;
    if ((position > (hpatch_StreamPos_t)INT64_MAX) ||
        !SetFilePointerEx(handle, offset, 0, FILE_BEGIN)) {
      *io_error = 1;
      LeaveCriticalSection(lock);
      return hpatch_FALSE;
    }
    while (remaining != 0) {
      DWORD count = 0;
      DWORD requested = remaining > (size_t)0x40000000U
                            ? 0x40000000U
                            : (DWORD)remaining;
      if (!ReadFile(handle, data, requested, &count, 0) || count == 0) {
        *io_error = 1;
        LeaveCriticalSection(lock);
        return hpatch_FALSE;
      }
      data += count;
      remaining -= count;
    }
  }
  LeaveCriticalSection(lock);
#else
  if (position > (hpatch_StreamPos_t)INT64_MAX) {
    __atomic_store_n(io_error, 1, __ATOMIC_RELAXED);
    return hpatch_FALSE;
  }
  while (remaining != 0) {
    ssize_t count = pread((int)native_handle, data, remaining, (off_t)position);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      __atomic_store_n(io_error, 1, __ATOMIC_RELAXED);
      return hpatch_FALSE;
    }
    data += count;
    remaining -= (size_t)count;
    position += (hpatch_StreamPos_t)count;
  }
#endif
  return hpatch_TRUE;
}

static hpatch_BOOL runtime_swapper_input_read(
    const hpatch_TStreamInput* base, hpatch_StreamPos_t position,
    unsigned char* data, unsigned char* data_end) {
  runtime_swapper_native_input* stream =
      (runtime_swapper_native_input*)base->streamImport;
  size_t length;
  if (data > data_end) return hpatch_FALSE;
  length = (size_t)(data_end - data);
  if ((hpatch_StreamPos_t)length > base->streamSize ||
      position > base->streamSize - (hpatch_StreamPos_t)length) {
    runtime_swapper_set_input_error(stream);
    return hpatch_FALSE;
  }
  return runtime_swapper_read_native(stream->handle, position, data, data_end,
                                     &stream->io_error
#if defined(_WIN32)
                                     , &stream->lock
#endif
  );
}

static hpatch_BOOL runtime_swapper_output_read(
    const hpatch_TStreamOutput* base, hpatch_StreamPos_t position,
    unsigned char* data, unsigned char* data_end) {
  runtime_swapper_native_output* stream =
      (runtime_swapper_native_output*)base->streamImport;
  size_t length;
  if (data > data_end) return hpatch_FALSE;
  length = (size_t)(data_end - data);
  if ((hpatch_StreamPos_t)length > base->streamSize ||
      position > base->streamSize - (hpatch_StreamPos_t)length) {
    runtime_swapper_set_output_error(stream);
    return hpatch_FALSE;
  }
  return runtime_swapper_read_native(stream->handle, position, data, data_end,
                                     &stream->io_error
#if defined(_WIN32)
                                     , &stream->lock
#endif
  );
}

static hpatch_BOOL runtime_swapper_output_write(
    const hpatch_TStreamOutput* base, hpatch_StreamPos_t position,
    const unsigned char* data, const unsigned char* data_end) {
  runtime_swapper_native_output* stream =
      (runtime_swapper_native_output*)base->streamImport;
  size_t remaining;
  if (data > data_end) return hpatch_FALSE;
  remaining = (size_t)(data_end - data);
  if ((hpatch_StreamPos_t)remaining > base->streamSize ||
      position > base->streamSize - (hpatch_StreamPos_t)remaining) {
    runtime_swapper_set_output_error(stream);
    return hpatch_FALSE;
  }
#if defined(_WIN32)
  EnterCriticalSection(&stream->lock);
  {
    HANDLE handle = (HANDLE)stream->handle;
    LARGE_INTEGER offset;
    offset.QuadPart = (LONGLONG)position;
    if ((position > (hpatch_StreamPos_t)INT64_MAX) ||
        !SetFilePointerEx(handle, offset, 0, FILE_BEGIN)) {
      stream->io_error = 1;
      LeaveCriticalSection(&stream->lock);
      return hpatch_FALSE;
    }
    while (remaining != 0) {
      DWORD count = 0;
      DWORD requested = remaining > (size_t)0x40000000U
                            ? 0x40000000U
                            : (DWORD)remaining;
      if (!WriteFile(handle, data, requested, &count, 0) || count == 0) {
        stream->io_error = 1;
        LeaveCriticalSection(&stream->lock);
        return hpatch_FALSE;
      }
      data += count;
      remaining -= count;
    }
  }
  LeaveCriticalSection(&stream->lock);
#else
  if (position > (hpatch_StreamPos_t)INT64_MAX) {
    runtime_swapper_set_output_error(stream);
    return hpatch_FALSE;
  }
  while (remaining != 0) {
    ssize_t count = pwrite((int)stream->handle, data, remaining,
                           (off_t)position);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      runtime_swapper_set_output_error(stream);
      return hpatch_FALSE;
    }
    data += count;
    remaining -= (size_t)count;
    position += (hpatch_StreamPos_t)count;
  }
#endif
  return hpatch_TRUE;
}

static hpatch_BOOL runtime_swapper_native_size(intptr_t native_handle,
                                               hpatch_StreamPos_t* size) {
#if defined(_WIN32)
  LARGE_INTEGER value;
  if (!GetFileSizeEx((HANDLE)native_handle, &value) || value.QuadPart < 0)
    return hpatch_FALSE;
  *size = (hpatch_StreamPos_t)value.QuadPart;
#else
  struct stat status;
  if (fstat((int)native_handle, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size < 0)
    return hpatch_FALSE;
  *size = (hpatch_StreamPos_t)status.st_size;
#endif
  return hpatch_TRUE;
}

static hpatch_BOOL runtime_swapper_input_init(
    runtime_swapper_native_input* stream, intptr_t handle) {
  memset(stream, 0, sizeof(*stream));
  stream->handle = handle;
  stream->base.streamImport = stream;
  stream->base.read = runtime_swapper_input_read;
#if defined(_WIN32)
  InitializeCriticalSection(&stream->lock);
#endif
  return runtime_swapper_native_size(handle, &stream->base.streamSize);
}

static void runtime_swapper_output_init(runtime_swapper_native_output* stream,
                                        intptr_t handle,
                                        hpatch_StreamPos_t output_size) {
  memset(stream, 0, sizeof(*stream));
  stream->handle = handle;
  stream->base.streamImport = stream;
  stream->base.streamSize = output_size;
  stream->base.read_writed = runtime_swapper_output_read;
  stream->base.write = runtime_swapper_output_write;
#if defined(_WIN32)
  InitializeCriticalSection(&stream->lock);
#endif
}

static void runtime_swapper_input_destroy(runtime_swapper_native_input* stream) {
#if defined(_WIN32)
  DeleteCriticalSection(&stream->lock);
#else
  (void)stream;
#endif
}

static void runtime_swapper_output_destroy(
    runtime_swapper_native_output* stream) {
#if defined(_WIN32)
  DeleteCriticalSection(&stream->lock);
#else
  (void)stream;
#endif
}

int runtime_swapper_hpatch_handles(intptr_t source_handle,
                                   intptr_t patch_handle,
                                   intptr_t output_handle) {
  int result = HPATCH_SUCCESS;
  _THDiffInfos diff_infos;
  hpatch_TFileStreamInput diff_view;
  runtime_swapper_native_input source;
  runtime_swapper_native_input patch;
  runtime_swapper_native_output output;
  TPatchChecksumSet checksum_set = {
      0, hpatch_FALSE, hpatch_TRUE, hpatch_TRUE, hpatch_FALSE};
  memset(&diff_infos, 0, sizeof(diff_infos));
  memset(&diff_view, 0, sizeof(diff_view));

  if (!runtime_swapper_input_init(&source, source_handle))
    return HPATCH_OPENREAD_ERROR;
  if (!runtime_swapper_input_init(&patch, patch_handle)) {
    runtime_swapper_input_destroy(&source);
    return HPATCH_OPENREAD_ERROR;
  }
  diff_view.base = patch.base;
  result = _getHDiffInfos(&diff_infos, &diff_view, 1);
  if (result != HPATCH_SUCCESS || source.io_error || patch.io_error ||
      !diff_infos.isWindowDiff ||
      source.base.streamSize != diff_infos.diffInfo.oldDataSize) {
    if (result == HPATCH_SUCCESS) result = HPATCH_HDIFFINFO_ERROR;
    runtime_swapper_input_destroy(&patch);
    runtime_swapper_input_destroy(&source);
    return result;
  }

  runtime_swapper_output_init(&output, output_handle,
                              diff_infos.diffInfo.newDataSize);
  {
    _WinPatchListener_t context;
    struct winpatch_listener_t listener;
    TWindowPatchResult patch_result;
    memset(&context, 0, sizeof(context));
    memset(&listener, 0, sizeof(listener));
    context.decompressPlugin = &diff_infos._decompressPlugin;
    context.isLoadOldAll = hpatch_FALSE;
    context.patchCacheSize = 8U * 1024U * 1024U;
    context.checksumSet = &checksum_set;
    context.threadNum = 5;
    listener.import = &context;
    listener.onDiffInfo = _win_onDiffInfo;
    listener.onPatchFinish = _win_onPatchFinish;
    patch_result = patch_window_diff(&listener, &output.base, &source.base,
                                     &patch.base, 0, 5);
    switch (patch_result) {
      case kWindowPatch_ok:
        result = HPATCH_SUCCESS;
        break;
      case kWindowPatch_temp_mem_error:
        result = HPATCH_MEM_ERROR;
        break;
      case kWindowPatch_checksum_plugin_error:
        result = HPATCH_CHECKSUMSET_ERROR;
        break;
      case kWindowPatch_checksum_open_error:
        result = HPATCH_MEM_ERROR;
        break;
      case kWindowPatch_checksum_old_error:
        result = HPATCH_CHECKSUM_OLDDATA_ERROR;
        break;
      case kWindowPatch_checksum_new_error:
        result = HPATCH_CHECKSUM_NEWDATA_ERROR;
        break;
      case kWindowPatch_checksum_diff_error:
        result = HPATCH_CHECKSUM_DIFFDATA_ERROR;
        break;
      default:
        result = HPATCH_WINPATCH_ERROR;
        break;
    }
  }
  if (source.io_error || patch.io_error || output.io_error)
    result = HPATCH_FILEDATA_ERROR;
  {
    hpatch_StreamPos_t actual_size = 0;
    if (!runtime_swapper_native_size(output_handle, &actual_size) ||
        actual_size != output.base.streamSize)
      result = HPATCH_FILEDATA_ERROR;
  }
  runtime_swapper_output_destroy(&output);
  runtime_swapper_input_destroy(&patch);
  runtime_swapper_input_destroy(&source);
  return result;
}
