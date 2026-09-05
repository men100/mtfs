#include "mtfs_ra8p1_validation.h"

#if MTFS_ENABLE_STORAGE_SENTINEL_INFERENCE

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ff.h"
#include "mtfs_ra8p1_sentinel_runtime.h"
#include "mtfs_ra8p1_tflm_ethosu.h"
#include "extensions/security/sealed_blob/mtfs_secure_zero.h"
#include "sentinel/mtfs_sentinel_inference.h"
#include "sentinel/mtfs_sentinel_sha256.h"

using UB = unsigned char;
extern "C" int tm_printf(UB* format, ...);
extern "C" uint32_t mtfs_ra8p1_heap_call_count(void);

namespace {

constexpr char kModelPath[] = "0:/SRA_VAL.TFL";
constexpr char kCorpusPath[] = "0:/SRA_VAL.BIN";
constexpr uint32_t kModelSize = 3376U;
constexpr uint32_t kHeaderSize = 192U;
constexpr uint32_t kRecordSize = 128U;
constexpr uint32_t kRecordCount = 200U;
constexpr uint32_t kCorpusSize = kHeaderSize + kRecordSize * kRecordCount;
constexpr uint32_t kTimeoutMs = 2000U;
constexpr uint64_t kExpectedThresholdQ8 = 8U;
constexpr uint32_t kExpectedProfileId = UINT32_C(0x30cdf67d);

constexpr uint8_t kCanonicalHash[32] = {
    0x7d, 0xf6, 0xcd, 0x30, 0xc3, 0x08, 0xf2, 0xc9,
    0x3a, 0xf4, 0x1a, 0x55, 0x89, 0x74, 0xa7, 0xd7,
    0xaa, 0x50, 0x12, 0xb6, 0xdb, 0x3e, 0xa3, 0xfc,
    0x3c, 0xd7, 0xaa, 0x46, 0xdb, 0xf8, 0x8e, 0x8a};
constexpr uint8_t kOptimizedHash[32] = {
    0xe5, 0xcb, 0x46, 0xbd, 0xaf, 0xfb, 0xe2, 0x7f,
    0x26, 0xd6, 0x45, 0xc6, 0x51, 0x87, 0x36, 0x2d,
    0xe1, 0xba, 0x68, 0xa7, 0x89, 0xfa, 0x78, 0x66,
    0xe1, 0x52, 0x99, 0x35, 0xca, 0x63, 0x93, 0xc6};
constexpr uint8_t kConversionHash[32] = {
    0x00, 0xd5, 0x3c, 0xf3, 0xc2, 0x92, 0x8d, 0x15,
    0x15, 0x13, 0x00, 0x85, 0x67, 0x6d, 0xd8, 0x54,
    0xdd, 0xf6, 0x89, 0x3e, 0x58, 0x18, 0x72, 0x5c,
    0x42, 0x65, 0xd4, 0xdb, 0x65, 0x80, 0x23, 0xfc};

alignas(32) uint8_t g_model[kModelSize];
alignas(32) uint8_t g_corpus[kCorpusSize];
alignas(32) uint8_t g_persistent[MTFS_RA8P1_ETHOSU_PERSISTENT_SIZE];

uint16_t Load16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8U);
}

uint32_t Load32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8U) |
         (static_cast<uint32_t>(data[2]) << 16U) |
         (static_cast<uint32_t>(data[3]) << 24U);
}

uint64_t Load64(const uint8_t* data) {
  return static_cast<uint64_t>(Load32(data)) |
         (static_cast<uint64_t>(Load32(data + 4U)) << 32U);
}

bool ReadExact(const char* path, void* destination, uint32_t size) {
  FIL file{};
  UINT count = 0U;
  uint8_t extra = 0U;
  if (f_open(&file, path, FA_READ | FA_OPEN_EXISTING) != FR_OK) return false;
  const bool ok = f_read(&file, destination, size, &count) == FR_OK &&
                  count == size &&
                  f_read(&file, &extra, 1U, &count) == FR_OK && count == 0U;
  return f_close(&file) == FR_OK && ok;
}

bool HashMatches(const void* data, uint32_t size, const uint8_t expected[32]) {
  uint8_t digest[32];
  const bool ok = mtfs_sentinel_sha256(data, size, digest) == MTFS_OK &&
                  std::memcmp(digest, expected, sizeof(digest)) == 0;
  mtfs_secure_zero(digest, sizeof(digest));
  return ok;
}

bool ValidateHeader() {
  return std::memcmp(g_corpus, "MTFSRAV1", 8U) == 0 &&
      Load16(g_corpus + 8U) == 1U &&
      Load16(g_corpus + 10U) == kHeaderSize &&
      Load16(g_corpus + 12U) == kRecordSize &&
      Load16(g_corpus + 14U) == MTFS_SENTINEL_FEATURE_DIMENSION &&
      Load32(g_corpus + 16U) == kRecordCount &&
      Load32(g_corpus + 20U) == UINT32_C(0x52413850) &&
      Load32(g_corpus + 24U) == UINT32_C(0x53504920) &&
      Load32(g_corpus + 28U) == kExpectedProfileId &&
      Load64(g_corpus + 56U) == kExpectedThresholdQ8 &&
      std::memcmp(g_corpus + 64U, kCanonicalHash, 32U) == 0 &&
      std::memcmp(g_corpus + 96U, kOptimizedHash, 32U) == 0 &&
      HashMatches(g_corpus + kHeaderSize, kRecordSize * kRecordCount,
                  g_corpus + 160U);
}

uint32_t AbsoluteDelta(int first, int second) {
  const int delta = first - second;
  return static_cast<uint32_t>(delta < 0 ? -delta : delta);
}

void PrintHistogram(const char* name, const uint32_t histogram[10]) {
  tm_printf(reinterpret_cast<UB*>(const_cast<char*>(
      "[RA1-VALIDATION] %s=0:%u,1:%u,2:%u,3:%u,4:%u,5:%u,6:%u,7:%u,8:%u,9+:%u\n")),
      reinterpret_cast<UB*>(const_cast<char*>(name)), histogram[0],
      histogram[1], histogram[2], histogram[3], histogram[4], histogram[5],
      histogram[6], histogram[7], histogram[8], histogram[9]);
}

}  // namespace

extern "C" int mtfs_ra8p1_validation_run(uint32_t repeats) {
  mtfs_ra8p1_tflm_ethosu_t target{};
  mtfs_sentinel_npu_provider_config_t config{};
  mtfs_sentinel_npu_actual_info_t actual{};
  mtfs_sentinel_runtime_info_t runtime{};
  uint32_t raw_histogram[10]{};
  uint32_t q4_histogram[10]{};
  uint32_t score_histogram[10]{};
  uint32_t max_raw = 0U, max_q4 = 0U;
  uint64_t max_score = 0U, minimum_margin = UINT64_MAX;
  uint32_t decision_disagreements = 0U, interval_violations = 0U;
  uint32_t repeatability_failures = 0U, failures = 0U, near_threshold = 0U;
  const uint32_t heap_before = mtfs_ra8p1_heap_call_count();
  bool installed = false, locked = false;

  if (repeats == 0U || repeats > 100U ||
      !ReadExact(kModelPath, g_model, sizeof(g_model)) ||
      !ReadExact(kCorpusPath, g_corpus, sizeof(g_corpus)) ||
      !HashMatches(g_model, sizeof(g_model), kOptimizedHash) ||
      !ValidateHeader()) {
    tm_printf(reinterpret_cast<UB*>(const_cast<char*>(
        "[RA1-VALIDATION] FAIL stage=artifact-load-or-identity\n")));
    failures = 1U;
    goto cleanup;
  }
  runtime.api_version = MTFS_SENTINEL_INFERENCE_API_VERSION;
  runtime.struct_size = sizeof(runtime);
  runtime.runtime_type = MTFS_SENTINEL_RUNTIME_NPU;
  runtime.provider_id = MTFS_SENTINEL_PROVIDER_RA_TFLM_ETHOSU;
  runtime.accelerator_id = MTFS_SENTINEL_ACCELERATOR_ETHOS_U55;
  runtime.model_format = MTFS_SENTINEL_MODEL_FORMAT_RA_TFLM_ETHOSU;
  runtime.model_version = 2U;
  runtime.runtime_abi = MTFS_RA8P1_ETHOSU_RUNTIME_ABI;
  runtime.input_dimension = MTFS_SENTINEL_FEATURE_DIMENSION;
  runtime.output_dimension = MTFS_SENTINEL_FEATURE_DIMENSION;
  runtime.input_data_type = MTFS_SENTINEL_DATA_TYPE_INT8;
  runtime.output_data_type = MTFS_SENTINEL_DATA_TYPE_INT8;
  runtime.input_scale_numerator = Load32(g_corpus + 32U);
  runtime.input_scale_shift = Load32(g_corpus + 36U);
  runtime.input_zero_point = static_cast<int8_t>(g_corpus[40U]);
  runtime.output_zero_point = static_cast<int8_t>(g_corpus[41U]);
  runtime.output_scale_numerator = Load32(g_corpus + 44U);
  runtime.output_scale_shift = Load32(g_corpus + 48U);
  runtime.required_alignment = 32U;
  runtime.persistent_memory = sizeof(g_persistent);
  runtime.binary_size = sizeof(g_model);
  runtime.binary = g_model;
  std::memcpy(runtime.canonical_model_hash, kCanonicalHash, 32U);
  std::memcpy(runtime.runtime_binary_hash, kOptimizedHash, 32U);
  std::memcpy(runtime.conversion_manifest_hash, kConversionHash, 32U);
  runtime.descriptor_version = 2U;
  runtime.region_count = 8U;
  runtime.runtime_version_major = 25U;
  runtime.runtime_version_minor = 2U;
  runtime.runtime_variant = MTFS_RA8P1_ETHOSU_RUNTIME_VARIANT;
  runtime.runtime_extra = MTFS_RA8P1_ETHOSU_RUNTIME_EXTRA;
  if (mtfs_ra8p1_tflm_ethosu_provider_config(&target,
          mtfs_ra8p1_sentinel_npu_lock, mtfs_ra8p1_sentinel_npu_unlock,
          nullptr, &config) != MTFS_OK ||
      config.ops->inspect(config.target, g_model, sizeof(g_model), &runtime,
                          &actual) != MTFS_OK ||
      std::memcmp(actual.runtime_binary_hash, kOptimizedHash, 32U) != 0 ||
      actual.copy_size != sizeof(g_persistent) || actual.region_count != 8U) {
    tm_printf(reinterpret_cast<UB*>(const_cast<char*>(
        "[RA1-VALIDATION] FAIL stage=embedded-artifact-policy substage=%u detail=%d\n")),
        target.diagnostics.diagnostic_stage,
        target.diagnostics.diagnostic_detail);
    failures = 1U;
    goto cleanup;
  }
  if (config.ops->lock(config.target, kTimeoutMs) != MTFS_OK) {
    failures = 1U;
    goto cleanup;
  }
  locked = true;
  if (config.ops->install(config.target, g_model, sizeof(g_model),
                          g_persistent, sizeof(g_persistent), &runtime) != MTFS_OK) {
    tm_printf(reinterpret_cast<UB*>(const_cast<char*>(
        "[RA1-VALIDATION] FAIL stage=install substage=%u detail=%d\n")),
        target.diagnostics.diagnostic_stage,
        target.diagnostics.diagnostic_detail);
    failures = 1U;
    goto cleanup;
  }
  installed = true;
  for (uint32_t index = 0U; index < kRecordCount && failures == 0U; ++index) {
    const uint8_t* record = g_corpus + kHeaderSize + index * kRecordSize;
    const int8_t* input_q4 = reinterpret_cast<const int8_t*>(record + 8U);
    const int8_t* vendor_input = reinterpret_cast<const int8_t*>(record + 32U);
    const int8_t* expected_raw = reinterpret_cast<const int8_t*>(record + 56U);
    const int8_t* expected_q4 = reinterpret_cast<const int8_t*>(record + 80U);
    const uint64_t expected_score = Load64(record + 104U);
    const uint8_t expected_decision = record[112U];
    int8_t first_raw[MTFS_SENTINEL_FEATURE_DIMENSION]{};
    uint32_t vector_raw = 0U, vector_q4 = 0U;
    uint64_t vector_score = 0U;
    for (uint32_t repeat = 0U; repeat < repeats; ++repeat) {
      int8_t output_raw[MTFS_SENTINEL_FEATURE_DIMENSION]{};
      int8_t output_q4[MTFS_SENTINEL_FEATURE_DIMENSION]{};
      mtfs_sentinel_inference_result_t result{};
      uint32_t repeat_q4 = 0U;
      if (config.ops->infer(config.target, vendor_input, output_raw,
                            kTimeoutMs) != MTFS_OK) {
        ++failures;
        break;
      }
      for (uint32_t element = 0U; element < MTFS_SENTINEL_FEATURE_DIMENSION;
           ++element) {
        const uint32_t raw_error = AbsoluteDelta(output_raw[element],
                                                  expected_raw[element]);
        if (raw_error > vector_raw) vector_raw = raw_error;
        if (mtfs_sentinel_requantize_int8_to_q4(output_raw[element],
                runtime.output_scale_numerator, runtime.output_scale_shift,
                runtime.output_zero_point, &output_q4[element]) != MTFS_OK) {
          ++failures;
          break;
        }
        const uint32_t q4_error = AbsoluteDelta(output_q4[element],
                                                 expected_q4[element]);
        if (q4_error > repeat_q4) repeat_q4 = q4_error;
      }
      if (failures != 0U || mtfs_sentinel_score_q8(input_q4, output_q4,
              kExpectedThresholdQ8, &result) != MTFS_OK) {
        ++failures;
        break;
      }
      if (repeat_q4 > vector_q4) vector_q4 = repeat_q4;
      const uint64_t score_error = result.score_q8 > expected_score ?
          result.score_q8 - expected_score : expected_score - result.score_q8;
      if (score_error > vector_score) vector_score = score_error;
      if (result.anomaly != expected_decision) ++decision_disagreements;
      mtfs_sentinel_score_interval_t interval{};
      if (mtfs_sentinel_score_interval_q8(input_q4, expected_q4, repeat_q4,
              kExpectedThresholdQ8, &interval) != MTFS_OK ||
          result.score_q8 < interval.score_min_q8 ||
          result.score_q8 > interval.score_max_q8) {
        ++interval_violations;
      }
      if (repeat == 0U) {
        std::memcpy(first_raw, output_raw, sizeof(first_raw));
      } else if (std::memcmp(first_raw, output_raw, sizeof(first_raw)) != 0) {
        ++repeatability_failures;
      }
    }
    if (failures != 0U) break;
    ++raw_histogram[vector_raw < 9U ? vector_raw : 9U];
    ++q4_histogram[vector_q4 < 9U ? vector_q4 : 9U];
    ++score_histogram[vector_score < 9U ? static_cast<uint32_t>(vector_score) : 9U];
    if (vector_raw > max_raw) max_raw = vector_raw;
    if (vector_q4 > max_q4) max_q4 = vector_q4;
    if (vector_score > max_score) max_score = vector_score;
    const uint64_t margin = expected_score > kExpectedThresholdQ8 ?
        expected_score - kExpectedThresholdQ8 :
        kExpectedThresholdQ8 - expected_score;
    if (margin < minimum_margin) minimum_margin = margin;
    if (margin <= 1U) ++near_threshold;
  }

cleanup:
  if (installed && config.ops->close(config.target) != MTFS_OK) ++failures;
  if (locked) config.ops->unlock(config.target);
  if (config.ops != nullptr && config.ops->zeroize != nullptr) {
    config.ops->zeroize(config.target, g_persistent, sizeof(g_persistent));
  } else {
    mtfs_secure_zero(g_persistent, sizeof(g_persistent));
  }
  const uint32_t heap_after = mtfs_ra8p1_heap_call_count();
  if (heap_after != heap_before) ++failures;
  PrintHistogram("raw-vector-max-histogram", raw_histogram);
  PrintHistogram("q4-vector-max-histogram", q4_histogram);
  PrintHistogram("score-error-histogram", score_histogram);
  tm_printf(reinterpret_cast<UB*>(const_cast<char*>(
      "[RA1-VALIDATION] %s samples=%u repeats=%u invocations=%u raw-max=%u q4-max=%u score-error-max=%u score-interval-violations=%u decision-disagreements=%u repeatability-failures=%u threshold-q8=%u threshold-margin-min=%u near-threshold=%u arena-used=%u installs=%u closes=%u invokes=%u invoke-failures=%u heap-calls-delta=%u\n")),
      reinterpret_cast<UB*>(const_cast<char*>(failures == 0U ? "PASS" : "FAIL")),
      kRecordCount, repeats, kRecordCount * repeats, max_raw, max_q4,
      static_cast<uint32_t>(max_score), interval_violations,
      decision_disagreements, repeatability_failures,
      static_cast<uint32_t>(kExpectedThresholdQ8),
      static_cast<uint32_t>(minimum_margin), near_threshold,
      target.diagnostics.arena_used, target.diagnostics.installs,
      target.diagnostics.closes, target.diagnostics.invokes,
      target.diagnostics.invoke_failures, heap_after - heap_before);
  mtfs_secure_zero(g_corpus, sizeof(g_corpus));
  mtfs_secure_zero(g_model, sizeof(g_model));
  return failures == 0U ? 0 : 1;
}

#else
extern "C" int mtfs_ra8p1_validation_run(uint32_t repeats) {
  (void)repeats;
  return 1;
}
#endif
