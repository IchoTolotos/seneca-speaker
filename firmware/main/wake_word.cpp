// Seneca streaming microWakeWord model. Frontend settings/quantization match its training
// pipeline and ESPHome micro_wake_word (Apache-2.0); no ESPHome runtime is required.
#include "wake_word.h"
#include "audio_diag.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "controller.h"
#include "chime.h"
#include "xvf3800_i2s.h"
extern "C" bool rtc_peer_active(void);
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"
extern "C" {
#include "frontend.h"
#include "frontend_util.h"
extern const uint8_t seneca_start[] asm("_binary_seneca_tflite_start");
}

static const char *TAG = "seneca";
struct AudioBlock { int16_t pcm[160]; uint32_t seq; };
static QueueHandle_t s_audio;
static std::atomic<bool> s_ready;
static std::atomic<uint32_t> s_detections, s_frames;
static std::atomic<int> s_peak, s_score;
static uint32_t s_sequence;

void gls_wake_feed(const int16_t *samples, size_t count)
{
    if (!s_audio || count != 160) return;
    AudioBlock b;
    memcpy(b.pcm, samples, sizeof(b.pcm));
    b.seq = ++s_sequence;
    xQueueSend(s_audio, &b, 0); // never block the microphone; gaps reset inference state
}
bool gls_wake_ready(void) { return s_ready.load(); }
uint32_t gls_wake_detections(void) { return s_detections.load(); }
uint32_t gls_wake_frames(void) { return s_frames.load(); }
int gls_wake_peak(void) { return s_peak.load(); }
int gls_wake_score(void) { return s_score.load(); }

static bool register_ops(tflite::MicroMutableOpResolver<20> &r)
{
    return r.AddCallOnce() == kTfLiteOk && r.AddVarHandle() == kTfLiteOk &&
        r.AddReshape() == kTfLiteOk && r.AddReadVariable() == kTfLiteOk &&
        r.AddStridedSlice() == kTfLiteOk && r.AddConcatenation() == kTfLiteOk &&
        r.AddAssignVariable() == kTfLiteOk && r.AddConv2D() == kTfLiteOk &&
        r.AddMul() == kTfLiteOk && r.AddAdd() == kTfLiteOk && r.AddMean() == kTfLiteOk &&
        r.AddFullyConnected() == kTfLiteOk && r.AddLogistic() == kTfLiteOk &&
        r.AddQuantize() == kTfLiteOk && r.AddDepthwiseConv2D() == kTfLiteOk &&
        r.AddAveragePool2D() == kTfLiteOk && r.AddMaxPool2D() == kTfLiteOk &&
        r.AddPad() == kTfLiteOk && r.AddPack() == kTfLiteOk && r.AddSplitV() == kTfLiteOk;
}

static void task(void *)
{
    // Persistent arenas live in PSRAM; leave internal RAM available for Wi-Fi/WebRTC.
    constexpr size_t ARENA = 256 * 1024, VAR_ARENA = 4096;
    uint8_t *arena = (uint8_t *)heap_caps_aligned_alloc(16, ARENA, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *variables = (uint8_t *)heap_caps_aligned_alloc(16, VAR_ARENA, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const tflite::Model *model = tflite::GetModel(seneca_start);
    static tflite::MicroMutableOpResolver<20> resolver;
    if (!arena || !variables || model->version() != TFLITE_SCHEMA_VERSION || !register_ops(resolver)) {
        ESP_LOGE(TAG, "cannot initialize Seneca model");
        heap_caps_free(arena); heap_caps_free(variables); vTaskDelete(nullptr); return;
    }
    auto *allocator = tflite::MicroAllocator::Create(variables, VAR_ARENA);
    auto *resources = tflite::MicroResourceVariables::Create(allocator, 20);
    tflite::MicroInterpreter interpreter(model, resolver, arena, ARENA, resources);
    if (interpreter.AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "Seneca tensor allocation failed"); vTaskDelete(nullptr); return;
    }
    TfLiteTensor *input = interpreter.input(0), *output = interpreter.output(0);
    if (input->type != kTfLiteInt8 || input->dims->size != 3 || input->dims->data[0] != 1 ||
        input->dims->data[2] != 40 || input->dims->data[1] < 1 || input->dims->data[1] > 16 ||
        output->type != kTfLiteUInt8 || output->bytes != 1) {
        ESP_LOGE(TAG, "unexpected Seneca input/output tensors"); vTaskDelete(nullptr); return;
    }
    FrontendConfig cfg = {};
    cfg.window.size_ms = 30; cfg.window.step_size_ms = 10;
    cfg.filterbank.num_channels = 40;
    cfg.filterbank.lower_band_limit = 125; cfg.filterbank.upper_band_limit = 7500;
    cfg.noise_reduction.smoothing_bits = 10;
    cfg.noise_reduction.even_smoothing = .025; cfg.noise_reduction.odd_smoothing = .06;
    cfg.noise_reduction.min_signal_remaining = .05;
    cfg.pcan_gain_control.enable_pcan = 1; cfg.pcan_gain_control.strength = .95;
    cfg.pcan_gain_control.offset = 80; cfg.pcan_gain_control.gain_bits = 21;
    cfg.log_scale.enable_log = 1; cfg.log_scale.scale_shift = 6;
    FrontendState frontend = {};
    if (!FrontendPopulateState(&cfg, &frontend, 16000)) {
        ESP_LOGE(TAG, "audio frontend allocation failed"); vTaskDelete(nullptr); return;
    }
    int stride = input->dims->data[1], slice = 0, history_index = 0, warmup = 0;
    uint8_t history[5] = {};
    uint32_t previous = 0;
    int64_t last_detection = 0;
    bool was_muted = false;
    ESP_LOGI(TAG, "Seneca ready: 16 kHz, stride %d, cutoff 0.72, tensor arena used %u bytes",
             stride, (unsigned)interpreter.arena_used_bytes());
    s_ready = true;
    AudioBlock block;
    for (;;) {
        if (xQueueReceive(s_audio, &block, portMAX_DELAY) != pdTRUE) continue;
        bool test = xvf_i2s_wake_test();
        // muted, or in a call (not needed then: "Seneca, stop" works on the transcript) -> no inference
        bool muted = (xvf_i2s_mic_muted() || rtc_peer_active()) && !test; // the wake test listens anyway
        if ((previous && block.seq != previous + 1) || muted != was_muted) {
            FrontendReset(&frontend); interpreter.Reset();
            slice = 0; warmup = 0; history_index = 0; memset(history, 0, sizeof(history));
        }
        previous = block.seq; was_muted = muted;
        s_frames++;
        int peak = 0;
        for (int16_t sample : block.pcm) peak = std::max(peak, std::abs((int)sample));
        s_peak = peak;
        if (muted) { s_score = 0; continue; }
        size_t offset = 0;
        while (offset < 160) {
            size_t consumed = 0;
            FrontendOutput features = FrontendProcessSamples(&frontend, block.pcm + offset, 160 - offset, &consumed);
            offset += consumed;
            if (features.size == 40) {
                for (int i = 0; i < 40; i++) {
                    int value = (features.values[i] * 256 + 333) / 666 - 128;
                    input->data.int8[slice * 40 + i] = (int8_t)std::max(-128, std::min(127, value));
                }
                if (++slice == stride) {
                    slice = 0;
                    if (interpreter.Invoke() != kTfLiteOk) {
                        s_ready = false; ESP_LOGE(TAG, "Seneca inference failed"); vTaskDelete(nullptr); return;
                    }
                    history[history_index] = output->data.uint8[0];
                    history_index = (history_index + 1) % 5;
                    unsigned sum = 0; for (uint8_t p : history) sum += p;
                    s_score = sum * 100 / (5 * 255);
                    int64_t now = esp_timer_get_time();
                    if (warmup < 101) ++warmup;
                    bool detect = warmup > 100 && sum > 0.72 * 255 * 5 && now - last_detection > 2000000;
                    if (warmup > 100) gls_diag_wake_score(s_score.load(), detect);
                    if (detect) {
                        last_detection = now; s_detections++;
                        ESP_LOGI(TAG, "Seneca detected (confidence %d%%)%s", s_score.load(), test ? " [test: beep only]" : "");
                        if (test) gls_chime_play(GLS_CHIME_WAKE_TEST);
                        else gls_controller_wake();
                        memset(history, 0, sizeof(history));
                    }
                }
            }
            if (!consumed) break;
        }
    }
}

esp_err_t gls_wake_start(void)
{
    if (s_audio) return ESP_OK;
    s_audio = xQueueCreate(16, sizeof(AudioBlock));
    if (!s_audio) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(task, "seneca", 8192, nullptr, 4, nullptr, 1) != pdPASS  /* internal stack: PSRAM made inference too slow */) {
        vQueueDelete(s_audio); s_audio = nullptr; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
