/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <onnxruntime_c_api.h>
#include <windows.h>

#include "common/msg.h"
#include "misc/bstr.h"
#include "misc/json.h"
#include "include/mpv/client.h"
#include "ta/ta_talloc.h"

#include "ocr_engine.h"

#define OCR_DICTIONARY_SIZE 18710
#define OCR_MAX_IMAGES 64
#define OCR_MAX_LINES 128
#define OCR_MAX_IMAGE_DIMENSION 8192
#define OCR_MAX_TOTAL_PIXELS (64U * 1024U * 1024U)
#define OCR_MAX_MODEL_BYTES (1024ULL * 1024ULL * 1024ULL)
#define OCR_MAX_DICTIONARY_BYTES (16U * 1024U * 1024U)
#define OCR_MAX_INPUT_WIDTH 3200
#define OCR_MIN_INPUT_WIDTH 320
#define OCR_INPUT_HEIGHT 48
#define OCR_MAX_TEXT_BYTES (1024U * 1024U)

struct line_box {
    int x0;
    int y0;
    int x1;
    int y1;
    int region;
};

struct prepared_line {
    float *data;
    size_t elements;
    int width;
};

struct mp_ocr_engine {
    struct mp_log *log;
    HMODULE runtime;
    const OrtApi *ort;
    OrtEnv *env;
    OrtSession *session;
    OrtRunOptions *run_options;
    OrtMemoryInfo *memory_info;
    char *input_name;
    char *output_name;
    char **dictionary;
    int dictionary_size;
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE idle;
    bool lock_initialized;
    bool running;
    bool destroying;
    LONG cancelled;
};

static void set_error(void *parent, char **error, const char *format, ...)
{
    if (!error)
        return;
    va_list args;
    va_start(args, format);
    *error = talloc_vasprintf(parent, format, args);
    va_end(args);
}

#ifndef OCR_ENGINE_DETERMINISTIC_ONLY
static bool utf8_valid(const char *value)
{
    return value && bstr_validate_utf8(bstr0(value)) >= 0;
}

static wchar_t *utf8_to_wide(void *parent, const char *value)
{
    if (!utf8_valid(value))
        return NULL;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1,
                                    NULL, 0);
    if (count <= 0)
        return NULL;
    wchar_t *wide = talloc_array(parent, wchar_t, count);
    if (!wide || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1,
                                     wide, count) != count)
        return NULL;
    return wide;
}

static bool get_file_size(const wchar_t *path, uint64_t maximum,
                          uint64_t *size, char **error, void *parent,
                          const char *description)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data) ||
        data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
        set_error(parent, error, "%s is not a readable file", description);
        return false;
    }
    uint64_t length = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    if (!length || length > maximum) {
        set_error(parent, error, "%s size is outside the supported range",
                  description);
        return false;
    }
    if (size)
        *size = length;
    return true;
}

static bool read_bounded_file(void *parent, const wchar_t *path,
                              uint64_t maximum, char **data, size_t *length,
                              char **error, const char *description)
{
    uint64_t file_size;
    if (!get_file_size(path, maximum, &file_size, error, parent, description))
        return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        set_error(parent, error, "could not open %s", description);
        return false;
    }
    char *buffer = talloc_array(parent, char, (size_t)file_size + 1);
    DWORD read = 0;
    bool ok = buffer &&
              ReadFile(file, buffer, (DWORD)file_size, &read, NULL) &&
              read == file_size;
    CloseHandle(file);
    if (!ok) {
        set_error(parent, error, "could not read %s", description);
        return false;
    }
    buffer[file_size] = '\0';
    *data = buffer;
    *length = file_size;
    return true;
}

static HMODULE load_runtime(void *parent, const char *runtime_path,
                            wchar_t **resolved, char **error)
{
    HMODULE module = NULL;
    wchar_t path[32768];
    path[0] = L'\0';

    if (runtime_path) {
        wchar_t *requested = utf8_to_wide(parent, runtime_path);
        if (!requested) {
            set_error(parent, error, "ONNX Runtime path is not valid UTF-8");
            return NULL;
        }
        DWORD length = GetFullPathNameW(requested, (DWORD)(sizeof(path) /
                                            sizeof(path[0])), path, NULL);
        if (!length || length >= sizeof(path) / sizeof(path[0])) {
            set_error(parent, error, "ONNX Runtime path is not a valid path");
            return NULL;
        }
        if (!GetModuleHandleExW(0, path, &module)) {
            module = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                               LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
    } else {
        if (!GetModuleHandleExW(0, L"onnxruntime.dll", &module)) {
            HMODULE self = NULL;
            if (!GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    (LPCWSTR)mp_ocr_engine_create, &self) || !self)
            {
                set_error(parent, error, "could not locate the mpv module");
                return NULL;
            }
            DWORD length = GetModuleFileNameW(self, path,
                                              sizeof(path) / sizeof(path[0]));
            wchar_t *slash = length ? wcsrchr(path, L'\\') : NULL;
            if (!slash || length >= sizeof(path) / sizeof(path[0]) - 16) {
                set_error(parent, error, "could not resolve the mpv module path");
                return NULL;
            }
            wcscpy(slash + 1, L"onnxruntime.dll");
            module = LoadLibraryExW(path, NULL,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                    LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
    }

    if (!module) {
        set_error(parent, error, "onnxruntime.dll is unavailable");
        return NULL;
    }
    DWORD length = GetModuleFileNameW(module, path,
                                      sizeof(path) / sizeof(path[0]));
    if (!length || length >= sizeof(path) / sizeof(path[0])) {
        FreeLibrary(module);
        set_error(parent, error, "could not resolve the loaded ONNX Runtime");
        return NULL;
    }
    *resolved = talloc_memdup(parent, path, (length + 1) * sizeof(wchar_t));
    return module;
}

static bool ort_status(mp_ocr_engine *engine, OrtStatus *status, void *parent,
                       char **error, const char *operation)
{
    if (!status)
        return true;
    const char *message = engine->ort->GetErrorMessage(status);
    set_error(parent, error, "%s: %s", operation,
              message ? message : "unknown ONNX Runtime error");
    engine->ort->ReleaseStatus(status);
    return false;
}

static bool required_ort_api_available(const OrtApi *api)
{
    return api->CreateEnv && api->CreateSessionOptions && api->CreateSession &&
           api->SetIntraOpNumThreads && api->SetInterOpNumThreads &&
           api->SetSessionExecutionMode && api->SetSessionGraphOptimizationLevel &&
           api->CreateRunOptions && api->RunOptionsSetTerminate &&
           api->RunOptionsUnsetTerminate && api->CreateCpuMemoryInfo &&
           api->CreateTensorWithDataAsOrtValue && api->Run &&
           api->GetTensorTypeAndShape && api->GetTensorMutableData &&
           api->GetDimensionsCount && api->GetDimensions &&
           api->GetTensorElementType && api->SessionGetInputCount &&
           api->SessionGetOutputCount && api->SessionGetInputName &&
           api->SessionGetOutputName && api->SessionGetInputTypeInfo &&
           api->SessionGetOutputTypeInfo && api->CastTypeInfoToTensorInfo &&
           api->GetAllocatorWithDefaultOptions && api->ReleaseTypeInfo &&
           api->ReleaseTensorTypeAndShapeInfo && api->ReleaseValue &&
           api->ReleaseMemoryInfo && api->ReleaseRunOptions &&
           api->ReleaseSession && api->ReleaseSessionOptions &&
           api->ReleaseEnv && api->AllocatorFree;
}

static bool load_dictionary(mp_ocr_engine *engine, const wchar_t *path,
                            char **error)
{
    void *tmp = talloc_new(NULL);
    char *contents = NULL;
    size_t length = 0;
    struct mpv_node root = {0};
    char *parse_error = NULL;
    bool ok = read_bounded_file(tmp, path, OCR_MAX_DICTIONARY_BYTES,
                                &contents, &length, &parse_error,
                                "OCR dictionary");
    if (!ok) {
        *error = talloc_steal(NULL, parse_error);
        talloc_free(tmp);
        return false;
    }
    if (memchr(contents, '\0', length)) {
        set_error(NULL, error, "OCR dictionary contains an embedded NUL");
        talloc_free(tmp);
        return false;
    }
    char *cursor = contents;
    if (!json_validate_strict(contents, MAX_JSON_DEPTH) ||
        json_parse(tmp, &root, &cursor, MAX_JSON_DEPTH) < 0)
    {
        set_error(NULL, error, "OCR dictionary is not valid JSON");
        talloc_free(tmp);
        return false;
    }
    json_skip_whitespace(&cursor);
    if (*cursor || root.format != MPV_FORMAT_NODE_ARRAY || !root.u.list ||
        root.u.list->num != OCR_DICTIONARY_SIZE)
    {
        set_error(NULL, error,
                  "OCR dictionary must contain exactly %d strings",
                  OCR_DICTIONARY_SIZE);
        talloc_free(tmp);
        return false;
    }

    engine->dictionary =
        talloc_zero_array(engine, char *, OCR_DICTIONARY_SIZE);
    if (!engine->dictionary) {
        set_error(NULL, error, "out of memory loading OCR dictionary");
        talloc_free(tmp);
        return false;
    }
    for (int n = 0; n < OCR_DICTIONARY_SIZE; n++) {
        struct mpv_node *entry = &root.u.list->values[n];
        if (entry->format != MPV_FORMAT_STRING || !utf8_valid(entry->u.string) ||
            strlen(entry->u.string) > 64)
        {
            set_error(NULL, error,
                      "OCR dictionary entry %d is not a bounded UTF-8 string", n);
            talloc_free(tmp);
            return false;
        }
        engine->dictionary[n] = talloc_strdup(engine->dictionary,
                                              entry->u.string);
        if (!engine->dictionary[n]) {
            set_error(NULL, error, "out of memory loading OCR dictionary");
            talloc_free(tmp);
            return false;
        }
    }
    if (engine->dictionary[0][0] ||
        strcmp(engine->dictionary[OCR_DICTIONARY_SIZE - 1], " ") != 0)
    {
        set_error(NULL, error,
                  "OCR dictionary blank and space sentinel entries are invalid");
        talloc_free(tmp);
        return false;
    }
    engine->dictionary_size = OCR_DICTIONARY_SIZE;
    talloc_free(tmp);
    return true;
}

static bool validate_tensor_type(mp_ocr_engine *engine, bool input,
                                 char **error)
{
    OrtTypeInfo *type = NULL;
    const OrtTensorTypeAndShapeInfo *tensor = NULL;
    OrtStatus *status = input
        ? engine->ort->SessionGetInputTypeInfo(engine->session, 0, &type)
        : engine->ort->SessionGetOutputTypeInfo(engine->session, 0, &type);
    if (!ort_status(engine, status, NULL, error,
                    input ? "query OCR input type" : "query OCR output type"))
        return false;
    status = engine->ort->CastTypeInfoToTensorInfo(type, &tensor);
    if (!ort_status(engine, status, NULL, error, "query OCR tensor type")) {
        engine->ort->ReleaseTypeInfo(type);
        return false;
    }
    size_t rank = 0;
    ONNXTensorElementDataType element = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    bool ok = ort_status(engine, engine->ort->GetDimensionsCount(tensor, &rank),
                         NULL, error, "query OCR tensor rank") &&
              ort_status(engine, engine->ort->GetTensorElementType(
                                    tensor, &element),
                         NULL, error, "query OCR tensor element type");
    int64_t dimensions[4] = {0};
    if (ok && rank <= 4) {
        ok = ort_status(engine, engine->ort->GetDimensions(
                                    tensor, dimensions, rank),
                        NULL, error, "query OCR tensor dimensions");
    }
    if (ok) {
        if (element != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            (input && (rank != 4 ||
                       (dimensions[0] > 0 && dimensions[0] != 1) ||
                       dimensions[1] != 3 || dimensions[2] != OCR_INPUT_HEIGHT ||
                       dimensions[3] > 0)) ||
            (!input && (rank != 3 ||
                        (dimensions[0] > 0 && dimensions[0] != 1) ||
                        dimensions[2] != OCR_DICTIONARY_SIZE)))
        {
            set_error(NULL, error,
                      "OCR model %s tensor has unsupported rank %zu shape "
                      "[%lld,%lld,%lld,%lld] type %d",
                      input ? "input" : "output", rank,
                      (long long)dimensions[0], (long long)dimensions[1],
                      (long long)dimensions[2], (long long)dimensions[3],
                      (int)element);
            ok = false;
        }
    } else if (rank > 4) {
        set_error(NULL, error, "OCR model tensor rank is unsupported");
    }
    engine->ort->ReleaseTypeInfo(type);
    return ok;
}

static bool initialize_session(mp_ocr_engine *engine, const wchar_t *model_path,
                               char **error)
{
    OrtSessionOptions *options = NULL;
    OrtAllocator *allocator = NULL;
    size_t input_count = 0;
    size_t output_count = 0;
    char *input_name = NULL;
    char *output_name = NULL;

    if (!ort_status(engine, engine->ort->CreateEnv(
                               ORT_LOGGING_LEVEL_WARNING, "mpv-subtitle-ocr",
                               &engine->env),
                    NULL, error, "create OCR environment") ||
        !ort_status(engine, engine->ort->CreateSessionOptions(&options),
                    NULL, error, "create OCR session options") ||
        !ort_status(engine, engine->ort->SetIntraOpNumThreads(options, 2),
                    NULL, error, "set OCR intra-op threads") ||
        !ort_status(engine, engine->ort->SetInterOpNumThreads(options, 1),
                    NULL, error, "set OCR inter-op threads") ||
        !ort_status(engine, engine->ort->SetSessionExecutionMode(
                               options, ORT_SEQUENTIAL),
                    NULL, error, "set OCR sequential execution") ||
        !ort_status(engine, engine->ort->SetSessionGraphOptimizationLevel(
                               options, ORT_ENABLE_ALL),
                    NULL, error, "enable OCR graph optimization") ||
        !ort_status(engine, engine->ort->CreateSession(
                               engine->env, model_path, options,
                               &engine->session),
                    NULL, error, "load OCR model"))
        goto fail;

    engine->ort->ReleaseSessionOptions(options);
    options = NULL;

    if (!ort_status(engine, engine->ort->CreateRunOptions(&engine->run_options),
                    NULL, error, "create OCR run options") ||
        !ort_status(engine, engine->ort->CreateCpuMemoryInfo(
                               OrtArenaAllocator, OrtMemTypeDefault,
                               &engine->memory_info),
                    NULL, error, "create OCR CPU memory descriptor") ||
        !ort_status(engine, engine->ort->SessionGetInputCount(
                               engine->session, &input_count),
                    NULL, error, "query OCR input count") ||
        !ort_status(engine, engine->ort->SessionGetOutputCount(
                               engine->session, &output_count),
                    NULL, error, "query OCR output count") ||
        input_count != 1 || output_count != 1)
    {
        if (!*error)
            set_error(NULL, error, "OCR model must have one input and one output");
        goto fail;
    }
    if (!ort_status(engine, engine->ort->GetAllocatorWithDefaultOptions(
                               &allocator),
                    NULL, error, "get ONNX Runtime allocator") ||
        !ort_status(engine, engine->ort->SessionGetInputName(
                               engine->session, 0, allocator, &input_name),
                    NULL, error, "query OCR input name") ||
        !ort_status(engine, engine->ort->SessionGetOutputName(
                               engine->session, 0, allocator, &output_name),
                    NULL, error, "query OCR output name"))
        goto fail;
    engine->input_name = talloc_strdup(engine, input_name);
    engine->output_name = talloc_strdup(engine, output_name);
    engine->ort->AllocatorFree(allocator, input_name);
    engine->ort->AllocatorFree(allocator, output_name);
    input_name = output_name = NULL;
    if (!engine->input_name || !engine->output_name) {
        set_error(NULL, error, "out of memory storing OCR model metadata");
        goto fail;
    }
    if (!validate_tensor_type(engine, true, error) ||
        !validate_tensor_type(engine, false, error))
        goto fail;
    return true;

fail:
    if (allocator && input_name)
        engine->ort->AllocatorFree(allocator, input_name);
    if (allocator && output_name)
        engine->ort->AllocatorFree(allocator, output_name);
    if (options)
        engine->ort->ReleaseSessionOptions(options);
    return false;
}

mp_ocr_engine *mp_ocr_engine_create(struct mp_log *log,
                                    const char *model_path,
                                    const char *dictionary_path,
                                    const char *runtime_path,
                                    char **error)
{
    bool discard_error = !error;
    char *ignored_error = NULL;
    if (!error)
        error = &ignored_error;
    *error = NULL;
    if (!model_path || !dictionary_path ||
        !utf8_valid(model_path) || !utf8_valid(dictionary_path))
    {
        set_error(NULL, error, "OCR paths must be valid UTF-8");
        if (discard_error)
            talloc_free(ignored_error);
        return NULL;
    }

    mp_ocr_engine *engine = talloc_zero(NULL, mp_ocr_engine);
    void *tmp = talloc_new(NULL);
    if (!engine || !tmp) {
        talloc_free(engine);
        talloc_free(tmp);
        set_error(NULL, error, "out of memory creating OCR engine");
        if (discard_error)
            talloc_free(ignored_error);
        return NULL;
    }
    engine->log = log;
    wchar_t *model_wide = utf8_to_wide(tmp, model_path);
    wchar_t *dictionary_wide = utf8_to_wide(tmp, dictionary_path);
    wchar_t *runtime_resolved = NULL;
    uint64_t unused;
    if (!model_wide || !dictionary_wide ||
        !get_file_size(model_wide, OCR_MAX_MODEL_BYTES, &unused, error, NULL,
                       "OCR model") ||
        !load_dictionary(engine, dictionary_wide, error))
        goto fail;

    engine->runtime = load_runtime(tmp, runtime_path, &runtime_resolved, error);
    if (!engine->runtime)
        goto fail;
    typedef const OrtApiBase *(*get_api_base_fn)(void);
    get_api_base_fn get_api_base =
        (get_api_base_fn)GetProcAddress(engine->runtime, "OrtGetApiBase");
    if (!get_api_base) {
        set_error(NULL, error, "onnxruntime.dll does not export OrtGetApiBase");
        goto fail;
    }
    const OrtApiBase *base = get_api_base();
    engine->ort = base ? base->GetApi(ORT_API_VERSION) : NULL;
    if (!engine->ort || !required_ort_api_available(engine->ort)) {
        set_error(NULL, error,
                  "onnxruntime.dll does not provide the required API v%d",
                  ORT_API_VERSION);
        goto fail;
    }
    InitializeCriticalSection(&engine->lock);
    InitializeConditionVariable(&engine->idle);
    engine->lock_initialized = true;
    if (!initialize_session(engine, model_wide, error))
        goto fail;
    mp_verbose(log, "Subtitle OCR loaded ONNX Runtime API v%d from %ls.\n",
               ORT_API_VERSION, runtime_resolved);
    talloc_free(tmp);
    return engine;

fail:
    talloc_free(tmp);
    mp_ocr_engine_destroy(engine);
    if (discard_error)
        talloc_free(ignored_error);
    return NULL;
}
#endif

static bool image_valid(const struct mp_ocr_image *image)
{
    return image && image->bgra && image->w > 0 && image->h > 0 &&
           image->w <= OCR_MAX_IMAGE_DIMENSION &&
           image->h <= OCR_MAX_IMAGE_DIMENSION &&
           image->stride >= image->w * 4 &&
           image->x <= INT_MAX - image->w &&
           image->y <= INT_MAX - image->h;
}

static bool alpha_row_visible(const struct mp_ocr_image *image, int y)
{
    const uint8_t *row = image->bgra + (size_t)y * image->stride;
    for (int x = 0; x < image->w; x++) {
        if (row[x * 4 + 3])
            return true;
    }
    return false;
}

static void visible_bounds(const struct mp_ocr_image *image, int y0, int y1,
                           struct line_box *box)
{
    box->x0 = image->w;
    box->x1 = 0;
    box->y0 = y1;
    box->y1 = y0;
    for (int y = y0; y < y1; y++) {
        const uint8_t *row = image->bgra + (size_t)y * image->stride;
        for (int x = 0; x < image->w; x++) {
            if (!row[x * 4 + 3])
                continue;
            if (x < box->x0)
                box->x0 = x;
            if (x + 1 > box->x1)
                box->x1 = x + 1;
            if (y < box->y0)
                box->y0 = y;
            if (y + 1 > box->y1)
                box->y1 = y + 1;
        }
    }
}

static bool should_merge_rows(int previous_start, int previous_end,
                              int next_start, int next_end)
{
    int gap = next_start - previous_end;
    int previous_height = previous_end - previous_start;
    int next_height = next_end - next_start;
    if (gap <= 2)
        return true;
    int small = previous_height < next_height ? previous_height : next_height;
    int large = previous_height > next_height ? previous_height : next_height;
    return small <= 3 && large >= small * 2 && gap <= 4;
}

static bool segment_images(const struct mp_ocr_image *images, int num_images,
                           struct line_box *boxes, int *num_boxes,
                           char **error, void *parent)
{
    if (!images || num_images <= 0 || num_images > OCR_MAX_IMAGES) {
        set_error(parent, error, "OCR image count is outside the supported range");
        return false;
    }
    uint64_t total_pixels = 0;
    int count = 0;
    for (int region = 0; region < num_images; region++) {
        const struct mp_ocr_image *image = &images[region];
        if (!image_valid(image)) {
            set_error(parent, error, "OCR image %d has invalid dimensions", region);
            return false;
        }
        total_pixels += (uint64_t)image->w * image->h;
        if (total_pixels > OCR_MAX_TOTAL_PIXELS) {
            set_error(parent, error, "OCR images exceed the pixel limit");
            return false;
        }

        int ranges[OCR_MAX_LINES][2];
        int range_count = 0;
        int y = 0;
        while (y < image->h) {
            while (y < image->h && !alpha_row_visible(image, y))
                y++;
            if (y == image->h)
                break;
            int start = y++;
            while (y < image->h && alpha_row_visible(image, y))
                y++;
            if (range_count >= OCR_MAX_LINES) {
                set_error(parent, error, "OCR image contains too many text bands");
                return false;
            }
            ranges[range_count][0] = start;
            ranges[range_count++][1] = y;
        }
        if (!range_count) {
            set_error(parent, error, "OCR image %d has no visible pixels", region);
            return false;
        }

        int merged_start = ranges[0][0];
        int merged_end = ranges[0][1];
        for (int n = 1; n <= range_count; n++) {
            if (n < range_count &&
                should_merge_rows(merged_start, merged_end,
                                  ranges[n][0], ranges[n][1]))
            {
                merged_end = ranges[n][1];
                continue;
            }
            if (count >= OCR_MAX_LINES) {
                set_error(parent, error, "OCR input contains too many lines");
                return false;
            }
            visible_bounds(image, merged_start, merged_end, &boxes[count]);
            boxes[count].x0 += image->x;
            boxes[count].x1 += image->x;
            boxes[count].y0 += image->y;
            boxes[count].y1 += image->y;
            boxes[count].region = region;
            count++;
            if (n < range_count) {
                merged_start = ranges[n][0];
                merged_end = ranges[n][1];
            }
        }
    }
    *num_boxes = count;
    return true;
}

static int compare_boxes(const void *left, const void *right)
{
    const struct line_box *a = left;
    const struct line_box *b = right;
    if (a->y0 != b->y0)
        return a->y0 < b->y0 ? -1 : 1;
    if (a->x0 != b->x0)
        return a->x0 < b->x0 ? -1 : 1;
    if (a->region != b->region)
        return a->region < b->region ? -1 : 1;
    return 0;
}

static float source_gray(const struct mp_ocr_image *image, int x, int y)
{
    const uint8_t *pixel = image->bgra + (size_t)y * image->stride + x * 4;
    unsigned int intensity = pixel[0];
    if (pixel[1] > intensity)
        intensity = pixel[1];
    if (pixel[2] > intensity)
        intensity = pixel[2];
    return 255.0f - ((float)pixel[3] * intensity / 255.0f);
}

static bool prepare_line(void *parent, const struct mp_ocr_image *image,
                         const struct line_box *absolute,
                         struct prepared_line *prepared, char **error)
{
    int x0 = absolute->x0 - image->x;
    int y0 = absolute->y0 - image->y;
    int x1 = absolute->x1 - image->x;
    int y1 = absolute->y1 - image->y;
    int visible_w = x1 - x0;
    int visible_h = y1 - y0;
    int pad = visible_h / 12 + 2;
    if (pad > 8)
        pad = 8;
    int crop_w = visible_w + pad * 2;
    int crop_h = visible_h + pad * 2;
    if (crop_w <= 0 || crop_h <= 0) {
        set_error(parent, error, "OCR line has no visible extent");
        return false;
    }
    int content_width = (int)lround((double)crop_w * OCR_INPUT_HEIGHT / crop_h);
    if (content_width < 1)
        content_width = 1;
    if (content_width > OCR_MAX_INPUT_WIDTH)
        content_width = OCR_MAX_INPUT_WIDTH;
    int input_width = content_width > OCR_MIN_INPUT_WIDTH
        ? content_width : OCR_MIN_INPUT_WIDTH;
    size_t plane = (size_t)OCR_INPUT_HEIGHT * input_width;
    if (plane > SIZE_MAX / (3 * sizeof(float))) {
        set_error(parent, error, "OCR tensor size overflow");
        return false;
    }
    float *data = talloc_zero_array(parent, float, plane * 3);
    if (!data) {
        set_error(parent, error, "out of memory preparing OCR line");
        return false;
    }

    for (int dy = 0; dy < OCR_INPUT_HEIGHT; dy++) {
        double source_y = ((dy + 0.5) * crop_h / OCR_INPUT_HEIGHT) - 0.5 - pad;
        int sy0 = (int)floor(source_y);
        int sy1 = sy0 + 1;
        double fy = source_y - sy0;
        for (int dx = 0; dx < content_width; dx++) {
            double source_x = ((dx + 0.5) * crop_w / content_width) - 0.5 - pad;
            int sx0 = (int)floor(source_x);
            int sx1 = sx0 + 1;
            double fx = source_x - sx0;
            float samples[4] = {255, 255, 255, 255};
            int xs[2] = {sx0, sx1};
            int ys[2] = {sy0, sy1};
            for (int iy = 0; iy < 2; iy++) {
                for (int ix = 0; ix < 2; ix++) {
                    int sx = x0 + xs[ix];
                    int sy = y0 + ys[iy];
                    if (sx >= 0 && sx < image->w && sy >= 0 && sy < image->h)
                        samples[iy * 2 + ix] = source_gray(image, sx, sy);
                }
            }
            double top = samples[0] + (samples[1] - samples[0]) * fx;
            double bottom = samples[2] + (samples[3] - samples[2]) * fx;
            float normalized =
                (float)((top + (bottom - top) * fy) / 127.5 - 1.0);
            size_t offset = (size_t)dy * input_width + dx;
            data[offset] = normalized;
            data[plane + offset] = normalized;
            data[plane * 2 + offset] = normalized;
        }
    }
    prepared->data = data;
    prepared->elements = plane * 3;
    prepared->width = input_width;
    return true;
}

#ifndef OCR_ENGINE_DETERMINISTIC_ONLY
static bool engine_cancelled(mp_ocr_engine *engine)
{
    return InterlockedCompareExchange(&engine->cancelled, 0, 0) != 0;
}

static bool decode_output(mp_ocr_engine *engine, void *parent, OrtValue *output,
                          char **text, float *confidence, char **error)
{
    OrtTensorTypeAndShapeInfo *info = NULL;
    float *probabilities = NULL;
    size_t rank = 0;
    int64_t dimensions[3] = {0};
    if (!ort_status(engine, engine->ort->GetTensorTypeAndShape(output, &info),
                    parent, error, "query OCR output") ||
        !ort_status(engine, engine->ort->GetDimensionsCount(info, &rank),
                    parent, error, "query OCR output rank") ||
        rank != 3 ||
        !ort_status(engine, engine->ort->GetDimensions(info, dimensions, 3),
                    parent, error, "query OCR output dimensions") ||
        dimensions[0] != 1 || dimensions[1] <= 0 ||
        dimensions[1] > 10000 || dimensions[2] != engine->dictionary_size ||
        !ort_status(engine, engine->ort->GetTensorMutableData(
                               output, (void **)&probabilities),
                    parent, error, "read OCR output"))
    {
        if (!*error)
            set_error(parent, error, "OCR output tensor has an unsupported shape");
        if (info)
            engine->ort->ReleaseTensorTypeAndShapeInfo(info);
        return false;
    }
    engine->ort->ReleaseTensorTypeAndShapeInfo(info);

    size_t capacity = 64;
    size_t used = 0;
    char *buffer = talloc_array(parent, char, capacity);
    if (!buffer) {
        set_error(parent, error, "out of memory decoding OCR output");
        return false;
    }
    int previous = -1;
    double confidence_sum = 0;
    int confidence_count = 0;
    for (int64_t step = 0; step < dimensions[1]; step++) {
        const float *row = probabilities + step * dimensions[2];
        int best = 0;
        float best_probability = row[0];
        if (!isfinite(best_probability)) {
            set_error(parent, error, "OCR output contains non-finite values");
            return false;
        }
        for (int token = 1; token < engine->dictionary_size; token++) {
            float probability = row[token];
            if (!isfinite(probability)) {
                set_error(parent, error, "OCR output contains non-finite values");
                return false;
            }
            if (probability > best_probability) {
                best_probability = probability;
                best = token;
            }
        }
        if (best_probability < 0.0f || best_probability > 1.0001f) {
            set_error(parent, error, "OCR output is not a probability tensor");
            return false;
        }
        if (best != 0 && best != previous) {
            size_t token_length = strlen(engine->dictionary[best]);
            if (used + token_length + 1 > OCR_MAX_TEXT_BYTES) {
                set_error(parent, error, "OCR output exceeds the text limit");
                return false;
            }
            if (used + token_length + 1 > capacity) {
                size_t next = capacity;
                while (next < used + token_length + 1)
                    next *= 2;
                buffer = talloc_realloc(parent, buffer, char, next);
                if (!buffer) {
                    set_error(parent, error, "out of memory decoding OCR output");
                    return false;
                }
                capacity = next;
            }
            memcpy(buffer + used, engine->dictionary[best], token_length);
            used += token_length;
            confidence_sum += best_probability;
            confidence_count++;
        }
        previous = best;
    }
    buffer[used] = '\0';
    if (!utf8_valid(buffer)) {
        set_error(parent, error, "OCR output is not valid UTF-8");
        return false;
    }
    *text = buffer;
    *confidence = confidence_count
        ? (float)(confidence_sum / confidence_count) : 0.0f;
    return true;
}

static bool recognize_line(mp_ocr_engine *engine, void *parent,
                           const struct prepared_line *prepared,
                           char **text, float *confidence, char **error)
{
    int64_t shape[] = {1, 3, OCR_INPUT_HEIGHT, prepared->width};
    OrtValue *input = NULL;
    OrtValue *output = NULL;
    const char *input_names[] = {engine->input_name};
    const char *output_names[] = {engine->output_name};
    if (!ort_status(engine, engine->ort->CreateTensorWithDataAsOrtValue(
                               engine->memory_info, prepared->data,
                               prepared->elements * sizeof(float),
                               shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                               &input),
                    parent, error, "create OCR input tensor"))
        return false;
    bool ok = ort_status(engine, engine->ort->Run(
                                    engine->session, engine->run_options,
                                    input_names, (const OrtValue *const *)&input,
                                    1, output_names, 1, &output),
                         parent, error, "run OCR model");
    engine->ort->ReleaseValue(input);
    if (!ok) {
        if (output)
            engine->ort->ReleaseValue(output);
        return false;
    }
    ok = decode_output(engine, parent, output, text, confidence, error);
    engine->ort->ReleaseValue(output);
    return ok;
}

static struct mp_ocr_result *failure_result(const char *message)
{
    struct mp_ocr_result *result = talloc_zero(NULL, struct mp_ocr_result);
    if (result)
        result->error = talloc_strdup(result, message ? message : "OCR failed");
    return result;
}

bool mp_ocr_engine_recognize(mp_ocr_engine *engine,
                             const struct mp_ocr_image *images,
                             int num_images,
                             struct mp_ocr_result **result)
{
    if (!result)
        return false;
    *result = NULL;
    if (!engine) {
        *result = failure_result("OCR engine is not initialized");
        return false;
    }

    EnterCriticalSection(&engine->lock);
    if (engine->running || engine->destroying) {
        LeaveCriticalSection(&engine->lock);
        *result = failure_result("OCR engine is already in use");
        return false;
    }
    char *run_error = NULL;
    if (!ort_status(engine,
                    engine->ort->RunOptionsUnsetTerminate(engine->run_options),
                    NULL, &run_error, "reset OCR cancellation"))
    {
        LeaveCriticalSection(&engine->lock);
        *result = failure_result(run_error);
        talloc_free(run_error);
        return false;
    }
    engine->running = true;
    LeaveCriticalSection(&engine->lock);

    void *tmp = talloc_new(NULL);
    struct line_box boxes[OCR_MAX_LINES];
    int num_boxes = 0;
    bool ok = tmp && segment_images(images, num_images, boxes, &num_boxes,
                                    &run_error, tmp);
    if (ok)
        qsort(boxes, num_boxes, sizeof(boxes[0]), compare_boxes);
    struct mp_ocr_result *output =
        ok ? talloc_zero(NULL, struct mp_ocr_result) : NULL;
    size_t total_text = 0;
    if (ok && !output) {
        set_error(tmp, &run_error, "out of memory creating OCR result");
        ok = false;
    }
    if (ok) {
        output->lines = talloc_zero_array(output, struct mp_ocr_line, num_boxes);
        if (!output->lines) {
            set_error(tmp, &run_error, "out of memory creating OCR lines");
            ok = false;
        }
    }

    for (int n = 0; ok && n < num_boxes; n++) {
        if (engine_cancelled(engine)) {
            set_error(tmp, &run_error, "OCR operation was cancelled");
            ok = false;
            break;
        }
        const struct line_box *box = &boxes[n];
        const struct mp_ocr_image *image = &images[box->region];
        struct prepared_line prepared = {0};
        char *line_error = NULL;
        if (!prepare_line(tmp, image, box, &prepared, &line_error) ||
            !recognize_line(engine, output, &prepared,
                            &output->lines[n].text,
                            &output->lines[n].confidence, &line_error))
        {
            run_error = talloc_steal(tmp, line_error);
            ok = false;
            break;
        }
        output->lines[n].x = box->x0;
        output->lines[n].y = box->y0;
        output->lines[n].w = box->x1 - box->x0;
        output->lines[n].h = box->y1 - box->y0;
        output->lines[n].region = box->region;
        size_t text_length = strlen(output->lines[n].text);
        if (text_length > OCR_MAX_TEXT_BYTES - total_text) {
            set_error(tmp, &run_error, "OCR result exceeds the text limit");
            ok = false;
            break;
        }
        total_text += text_length;
        output->num_lines++;
    }
    if (ok && engine_cancelled(engine)) {
        set_error(tmp, &run_error, "OCR operation was cancelled");
        ok = false;
    }

    EnterCriticalSection(&engine->lock);
    engine->running = false;
    WakeAllConditionVariable(&engine->idle);
    LeaveCriticalSection(&engine->lock);

    if (!ok) {
        talloc_free(output);
        *result = failure_result(run_error);
    } else {
        *result = output;
    }
    talloc_free(tmp);
    return ok;
}

void mp_ocr_engine_cancel(mp_ocr_engine *engine)
{
    if (!engine)
        return;
    InterlockedExchange(&engine->cancelled, 1);
    EnterCriticalSection(&engine->lock);
    if (engine->running) {
        OrtStatus *status =
            engine->ort->RunOptionsSetTerminate(engine->run_options);
        if (status) {
            mp_err(engine->log, "Could not terminate subtitle OCR: %s\n",
                   engine->ort->GetErrorMessage(status));
            engine->ort->ReleaseStatus(status);
        }
    }
    LeaveCriticalSection(&engine->lock);
}

void mp_ocr_engine_reset_cancel(mp_ocr_engine *engine)
{
    if (!engine)
        return;
    InterlockedExchange(&engine->cancelled, 0);
}

void mp_ocr_engine_destroy(mp_ocr_engine *engine)
{
    if (!engine)
        return;
    if (engine->lock_initialized) {
        EnterCriticalSection(&engine->lock);
        engine->destroying = true;
        while (engine->running)
            SleepConditionVariableCS(&engine->idle, &engine->lock, INFINITE);
        LeaveCriticalSection(&engine->lock);
    }
    if (engine->ort) {
        if (engine->memory_info)
            engine->ort->ReleaseMemoryInfo(engine->memory_info);
        if (engine->run_options)
            engine->ort->ReleaseRunOptions(engine->run_options);
        if (engine->session)
            engine->ort->ReleaseSession(engine->session);
        if (engine->env)
            engine->ort->ReleaseEnv(engine->env);
    }
    if (engine->runtime)
        FreeLibrary(engine->runtime);
    if (engine->lock_initialized)
        DeleteCriticalSection(&engine->lock);
    talloc_free(engine);
}
#endif
