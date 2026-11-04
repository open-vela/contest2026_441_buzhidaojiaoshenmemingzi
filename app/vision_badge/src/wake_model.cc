/* SPDX-License-Identifier: Apache-2.0 */
/* R1 adapter for the locked OpenVela TFLM and its real microfrontend. */
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <new>
#include <syslog.h>
#include <arch/chip/bk7258_psram.h>
#include <vision_badge/wake_model.h>
#include <vision_badge/wake_policy.h>
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "wake_selftest_input.inc"

extern "C" {
extern const unsigned char r1_wake_model_data[];
extern const size_t r1_wake_model_bytes;
}

static bool unpack_bits(const unsigned char *input, size_t bytes,
                        unsigned char *output, size_t capacity)
{
  size_t source=0,dest=0;
  while (source<bytes && dest<capacity)
    {
      unsigned code=input[source++];
      size_t count=code<128?code+1:257-code;
      size_t needed=code<128?count:1;
      if (code==128 || count>capacity-dest || needed>bytes-source) return false;
      if (code<128) std::memcpy(output+dest,input+source,count);
      else std::memset(output+dest,input[source],count);
      source+=needed;dest+=count;
    }
  return source==bytes && dest==capacity;
}

static uint32_t model_u32(const unsigned char *data)
{
  return uint32_t(data[0]) | uint32_t(data[1])<<8 |
         uint32_t(data[2])<<16 | uint32_t(data[3])<<24;
}

struct r1_wake_model_s
{
  void *allocation = nullptr;
  void *model_allocation = nullptr;
  void *arena_allocation = nullptr;
  tflite::MicroMutableOpResolver<6> resolver;
  tflite::MicroInterpreter *interpreter = nullptr;
  alignas(tflite::MicroInterpreter) unsigned char interpreter_storage[sizeof(tflite::MicroInterpreter)];
  unsigned char *arena = nullptr;
  FrontendState frontend = {};
  bool frontend_ready = false;
  TfLiteTensor *input = nullptr, *output = nullptr;
  float features[149 * 40] = {};
  int16_t overlap[480] = {};
  unsigned int pending = 0, rows = 0, hops = 0;
  uint64_t last_ms = 0;
  bool timestamp_valid = false;
  r1_wake_policy_s policy = {};
  r1_wake_model_diag_s diagnostic = {};
};

void r1_wake_model_close(r1_wake_model_s *m)
{
  if (!m) return;
  if (m->frontend_ready) FrontendFreeStateContents(&m->frontend);
  if (m->interpreter) m->interpreter->~MicroInterpreter();
  std::free(m->arena_allocation);
  std::free(m->model_allocation);
  void *allocation = m->allocation;
  m->~r1_wake_model_s();
  bk7258_psram_free(allocation);
}

int r1_wake_model_open(const unsigned char *data, size_t bytes, bool require_release, r1_wake_model_s **out)
{
  if (!out) return -EINVAL;
  *out = nullptr;
  if (!data || bytes < 8 || bytes > 128 * 1024) return -EINVAL;
  bool packed=std::memcmp(data,"R1PB",4)==0;
  if (packed && bytes<12) return -EBADMSG;
  size_t model_bytes=packed?model_u32(data+4):bytes;
  if (model_bytes<8 || model_bytes>128*1024) return -EBADMSG;
  void *model_allocation=std::malloc(model_bytes+15);
  if (!model_allocation) return -ENOMEM;
  auto *copied_model=reinterpret_cast<unsigned char *>((reinterpret_cast<uintptr_t>(model_allocation)+15)&~uintptr_t(15));
  if (packed)
    {
      bool valid=unpack_bits(data+12,bytes-12,copied_model,model_bytes);
      uint32_t checksum=UINT32_C(2166136261);
      if (valid) for (size_t i=0;i<model_bytes;i++)
        checksum=(checksum^copied_model[i])*UINT32_C(16777619);
      if (!valid || checksum!=model_u32(data+8))
        { std::free(model_allocation); return -EBADMSG; }
    }
  else std::memcpy(copied_model,data,model_bytes);
  flatbuffers::Verifier verifier(copied_model,model_bytes);
  if (!tflite::VerifyModelBuffer(verifier))
    { std::free(model_allocation); return -EBADMSG; }
  const auto *model = tflite::GetModel(copied_model);
  if (model->version() != TFLITE_SCHEMA_VERSION || !model->subgraphs() || model->subgraphs()->size() != 1)
    { std::free(model_allocation); return -ENOTSUP; }
  const auto *graph = model->subgraphs()->Get(0);
  if (!graph->inputs() || graph->inputs()->size() != 1 || !graph->outputs() || graph->outputs()->size() != 1)
    { std::free(model_allocation); return -ENOTSUP; }
  void *raw = bk7258_psram_malloc(sizeof(r1_wake_model_s) + 15);
  if (!raw) { std::free(model_allocation); return -ENOMEM; }
  auto *m = new (reinterpret_cast<void *>((reinterpret_cast<uintptr_t>(raw) + 15) & ~uintptr_t(15))) r1_wake_model_s;
  m->allocation = raw;
  /* Match team135 trigger_model_load (930d75ae, Apache-2.0): model
   * weights and the CPU-only arena use normal malloc with 16-byte alignment.
   * Do not retain a Flash-backed model or force the arena into the chip's
   * private PSRAM heap. Normal malloc may still select a PSRAM region;
   * log actual addresses and measure, rather than promising SRAM residency.
   */
  constexpr size_t arena_bytes = 64 * 1024;
  m->model_allocation = model_allocation;
  m->arena_allocation = std::malloc(arena_bytes + 15);
  if (!m->model_allocation || !m->arena_allocation)
    { r1_wake_model_close(m); return -ENOMEM; }
  m->arena = reinterpret_cast<unsigned char *>((reinterpret_cast<uintptr_t>(m->arena_allocation) + 15) & ~uintptr_t(15));
  auto &r = m->resolver;
  if (r.AddConv2D(tflite::Register_CONV_2D_INT8()) != kTfLiteOk ||
      r.AddDepthwiseConv2D(tflite::Register_DEPTHWISE_CONV_2D_INT8()) != kTfLiteOk ||
      r.AddAveragePool2D(tflite::Register_AVERAGE_POOL_2D_INT8()) != kTfLiteOk ||
      r.AddFullyConnected(tflite::Register_FULLY_CONNECTED_INT8()) != kTfLiteOk ||
      r.AddReshape() != kTfLiteOk || r.AddSoftmax(tflite::Register_SOFTMAX_INT8()) != kTfLiteOk)
    { r1_wake_model_close(m); return -ENOTSUP; }
  m->interpreter = new (m->interpreter_storage) tflite::MicroInterpreter(model, r, m->arena, arena_bytes);
  if (m->interpreter->AllocateTensors() != kTfLiteOk)
    { r1_wake_model_close(m); return -ENOMEM; }
  syslog(LOG_INFO,"R1 WAKE memory weights=%p arena=%p context=%p\n",
         static_cast<void *>(copied_model), static_cast<void *>(m->arena),
         static_cast<void *>(m));
  m->input = m->interpreter->input(0); m->output = m->interpreter->output(0);
  const int dimensions[] = {1,149,40,1};
  bool valid = m->input && m->output && m->input->type == kTfLiteInt8 && m->output->type == kTfLiteInt8 &&
    m->input->dims && m->input->dims->size == 4 && m->input->bytes == 149 * 40 &&
    m->output->dims && m->output->dims->size == 2 && m->output->dims->data[0] == 1 &&
    m->output->dims->data[1] == 3 && m->output->bytes == 3;
  if (valid) for (unsigned i = 0; i < 4; i++) valid = valid && m->input->dims->data[i] == dimensions[i];
  if (!valid || !std::isfinite(m->input->params.scale) || m->input->params.scale <= 0 ||
      !std::isfinite(m->output->params.scale) || m->output->params.scale <= 0 ||
      m->input->params.zero_point < -128 || m->input->params.zero_point > 127 ||
      m->output->params.zero_point < -128 || m->output->params.zero_point > 127)
    { r1_wake_model_close(m); return -EPROTO; }
  FrontendConfig config;
  FrontendFillConfigWithDefaults(&config);
  config.window.size_ms = 30; config.window.step_size_ms = 20;
  config.filterbank.num_channels = 40;
  config.filterbank.lower_band_limit = 125; config.filterbank.upper_band_limit = 7500;
  config.noise_reduction.even_smoothing = config.noise_reduction.odd_smoothing = 0;
  config.noise_reduction.min_signal_remaining = 1;
  config.pcan_gain_control.enable_pcan = 0;
  config.log_scale.enable_log = 1; config.log_scale.scale_shift = 6;
  if (!FrontendPopulateState(&config, &m->frontend, 16000))
    { FrontendFreeStateContents(&m->frontend); r1_wake_model_close(m); return -ENOMEM; }
  m->frontend_ready = true;
  /* Adopt team 135's silent-history initialization: a short phrase spoken
   * immediately after listening begins must have two evaluable windows.
   * Generate history with this exact frontend, while retaining our existing
   * 30/20 ms PCM framing (pending remains zero). Gaps still clear history. */
  size_t silence_read = 0;
  auto silence = FrontendProcessSamples(&m->frontend, m->overlap, 480, &silence_read);
  if (silence_read != 480 || !silence.values || silence.size != 40)
    { r1_wake_model_close(m); return -EPROTO; }
  for (unsigned row = 0; row < 149; row++)
    for (unsigned i = 0; i < 40; i++) m->features[row * 40 + i] = silence.values[i];
  FrontendReset(&m->frontend);
  m->rows = 149;
  r1_wake_policy_init(&m->policy, require_release);
  *out = m;
  return 0;
}

void r1_wake_model_gap(r1_wake_model_s *m)
{
  FrontendReset(&m->frontend);
  std::memset(m->features, 0, sizeof(m->features));
  std::memset(m->overlap, 0, sizeof(m->overlap));
  m->pending = m->rows = m->hops = 0;
  m->diagnostic.valid = false;
  r1_wake_policy_gap(&m->policy);
}

int r1_wake_model_feed(r1_wake_model_s *m, const int16_t pcm[320], uint64_t end_ms, float *score, bool *inferred)
{
  if (!m || !pcm || !score || !inferred) return -EINVAL;
  *score = 0; *inferred = false;
  if (m->timestamp_valid && (end_ms <= m->last_ms || end_ms - m->last_ms != 20))
    {
      r1_wake_model_gap(m);
      if (end_ms <= m->last_ms) return -ESTALE;
    }
  m->last_ms = end_ms; m->timestamp_valid = true;
  unsigned consumed = 0;
  while (consumed < 320)
    {
      unsigned take = 480 - m->pending;
      if (take > 320 - consumed) take = 320 - consumed;
      std::memcpy(m->overlap + m->pending, pcm + consumed, take * sizeof(int16_t));
      consumed += take; m->pending += take;
      if (m->pending < 480) continue;
      if (m->rows == 149) std::memmove(m->features, m->features + 40, (149 - 1) * 40 * sizeof(float));
      else m->rows++;
      FrontendReset(&m->frontend); /* v1 model uses independently reset windows. */
      size_t read = 0;
      auto output = FrontendProcessSamples(&m->frontend, m->overlap, 480, &read);
      if (read != 480 || !output.values || output.size != 40) return -EPROTO;
      for (unsigned i = 0; i < 40; i++) m->features[(m->rows - 1) * 40 + i] = output.values[i];
      std::memmove(m->overlap, m->overlap + 320, 160 * sizeof(int16_t));
      m->pending = 160;
    }
  if (++m->hops < R1_WAKE_SCORE_PERIOD_HOPS || m->rows < 149) return 0;
  m->hops = 0;
  m->diagnostic.input_low = m->diagnostic.input_high = 0;
  for (unsigned i = 0; i < 149 * 40; i++)
    {
      float value = std::round(m->features[i] / m->input->params.scale) + m->input->params.zero_point;
      if (value <= -128) m->diagnostic.input_low++;
      if (value >= 127) m->diagnostic.input_high++;
      m->input->data.int8[i] = static_cast<int8_t>(value < -128 ? -128 : value > 127 ? 127 : value);
    }
  if (m->interpreter->Invoke() != kTfLiteOk) return -EIO;
  float scores[3];
  for (unsigned i = 0; i < 3; i++) scores[i] = (m->output->data.int8[i] - m->output->params.zero_point) * m->output->params.scale;
  for (unsigned i = 0; i < 3; i++)
    {
      if (!std::isfinite(scores[i]) || scores[i] < 0 || scores[i] > 1)
        { m->diagnostic.valid = false; return -EPROTO; }
      m->diagnostic.scores_milli[i] = static_cast<unsigned int>(scores[i] * 1000 + .5f);
    }
  m->diagnostic.valid = true;
  *score = scores[2]; *inferred = true;
  return r1_wake_policy_score(&m->policy, scores, end_ms);
}
size_t r1_wake_model_arena_used(const r1_wake_model_s *m)
{
  return m ? m->interpreter->arena_used_bytes() : 0;
}

int r1_wake_model_diagnostics(const r1_wake_model_s *m, r1_wake_model_diag_s *out)
{
  if (!m || !out) return -EINVAL;
  *out = m->diagnostic;
  return out->valid ? 0 : -ENODATA;
}

int r1_wake_model_set_threshold(r1_wake_model_s *m, unsigned int percent)
{
  return m ? r1_wake_policy_set_threshold(&m->policy, percent) : -EINVAL;
}

int r1_wake_model_selftest(int output_q[3])
{
  if (!output_q) return -EINVAL;
  for (unsigned i = 0; i < 3; i++) output_q[i] = 0;
  r1_wake_model_s *m = nullptr;
  int ret = r1_wake_model_open(r1_wake_model_data, r1_wake_model_bytes, false, &m);
  if (ret < 0) return ret;
  /* PackBits: literal lengths 1..128, repeat lengths 2..128. Bounds are
   * checked even though this is an immutable, reviewed test vector. */
  if (!unpack_bits(r1_wake_test_input,sizeof(r1_wake_test_input),
                   reinterpret_cast<unsigned char *>(m->input->data.int8),5960)) ret=-EBADMSG;
  if (ret == 0 && m->interpreter->Invoke() != kTfLiteOk) ret = -EIO;
  if (ret == 0)
    for (unsigned i = 0; i < 3; i++)
      {
        output_q[i] = m->output->data.int8[i];
        int delta = output_q[i] - r1_wake_test_output[i];
        if (delta < -1 || delta > 1) ret = -EBADMSG;
      }
  r1_wake_model_close(m);
  return ret;
}

int r1_wake_model_frontend_selftest(unsigned int *changed, unsigned int *max_delta)
{
  /* Independent fixed-PCM frontend check, not a microphone recording.
   * Expected 40 bins were exported by the sanitized host frontend. */
  static const uint16_t expected[40] = {
    646,617,633,659,653,645,680,659,669,666,669,689,674,685,658,669,639,658,635,652,
    661,660,645,667,656,674,662,669,669,654,686,686,708,686,683,681,678,691,686,705};
  if (!changed || !max_delta) return -EINVAL;
  *changed = *max_delta = 0;
  r1_wake_model_s *m = nullptr;
  int ret = r1_wake_model_open(r1_wake_model_data, r1_wake_model_bytes, false, &m);
  if (ret < 0) return ret;
  uint32_t state = 0x12345678u;
  for (unsigned i = 0; i < 480; i++)
    {
      state = state * 1664525u + 1013904223u;
      m->overlap[i] = static_cast<int16_t>(static_cast<int32_t>(state >> 20) - 2048);
    }
  FrontendReset(&m->frontend);
  size_t read = 0;
  auto values = FrontendProcessSamples(&m->frontend, m->overlap, 480, &read);
  if (read != 480 || values.size != 40 || !values.values) ret = -EPROTO;
  else
    for (unsigned i = 0; i < 40; i++)
      {
        unsigned actual = values.values[i], wanted = expected[i];
        unsigned delta = actual > wanted ? actual - wanted : wanted - actual;
        if (delta) (*changed)++;
        if (delta > *max_delta) *max_delta = delta;
      }
  if (!ret && *max_delta > 1) ret = -EBADMSG;
  r1_wake_model_close(m);
  return ret;
}
