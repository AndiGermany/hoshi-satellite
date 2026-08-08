// =============================================================================
// hoshi_ws_audio.h — bidirectional wss client for the Hoshi satellite contract
// =============================================================================
//
// REDESIGN 2026-06-21 (Route B "done right"). The confirmed root cause of the
//   device hang was: the wake trigger ran on the ESPHome main loop and start_turn()
//   did a blocking esp_websocket_client_send_text (2s timeout) BEFORE CONNECTED,
//   starving USB-CDC, the api server (6053), the LED, and the watchdog.
//
//   FIX: NO networking EVER runs on the ESPHome main loop. Four strictly-separated
//   contexts share state only through esphome::ring_buffer::RingBuffer (single
//   producer / single consumer) and std::atomic flags:
//
//   (1) mic callback   [passive MicrophoneSource over i2s_mics; runs on the i2s
//                       mic task]: receives already channel-selected, 32->16-bit
//                       converted, gain-applied PCM16 mono. ONLY writes uplink_rb_,
//                       runs the RMS VAD, and sets the atomic eos_requested_.
//   (2) loop()         [ESPHome main loop]: turn state machine OWNER, drains
//                       play_rb_ -> speaker (play(...,ticks=0), backpressure),
//                       watchdog deadlines, LED. NEVER does blocking I/O.
//   (3) hoshi_ws_tx    [dedicated FreeRTOS task, off the IDF ws core]: the ONLY
//                       caller of esp_websocket_client send_text/send_bin. Lazy
//                       connect_() off-loop, waits for connected_, sends start,
//                       drains uplink_rb_ during LISTENING, on eos builds the WAV
//                       and sends it in <=16KB binary frames, then stop.
//   (4) ws_event_      [IDF esp_websocket_client task]: TLS+recv+dispatch.
//                       Reassembles fragmented text, base64-decodes llm_audio into
//                       play_rb_. NEVER sends here (recursive-lock deadlock).
//
// Contract authority: hoshi-satellite/CONTRACT.md + wiki/satellite-contract-0.7.md
// Proven reference:    hoshi-satellite/bridge/napi_bridge.py (same /ws/audio flow)
// =============================================================================

#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"  // NVS persistence for the night_mode state
#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/ring_buffer/ring_buffer.h"

#ifdef USE_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#endif

#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace esphome {

// Forward-declare light so the header needs no light include when LED is unused.
namespace light {
class LightState;
class AddressableLight;
}
namespace micro_wake_word {
class MicroWakeWord;
}

namespace hoshi_ws_audio {

enum class AuthMode { BEARER, QUERY };

// Internal turn lifecycle. Single owner: the ESPHome loop() (transitions are funnelled
// there or guarded atomically). Drives the LED + the half-duplex gate.
enum class TurnState : uint8_t {
  IDLE = 0,        // no active turn; passive mic data is dropped
  LISTENING,       // accumulating mic PCM into uplink_rb_, RMS VAD armed
  AWAITING_STT,    // eos fired: WAV + {stop} sent by tx task, waiting for transcript
  THINKING,        // transcript received, LLM running
  SPEAKING,        // playing returned audio; mic muted (half-duplex), mww stopped
  ERROR,           // transient error display state; loop() returns it to IDLE
};

class HoshiWsAudio : public Component {
 public:
  // --- ESPHome lifecycle ---
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // --- Config setters (called from __init__.py to_code) ---
  void set_microphone_source(microphone::MicrophoneSource *src) { this->mic_source_ = src; }
  void set_speaker(speaker::Speaker *spk) { this->speaker_ = spk; }
  void set_light(light::LightState *led) { this->led_ = led; }
  void set_micro_wake_word(micro_wake_word::MicroWakeWord *mww) { this->mww_ = mww; }
  void set_host(const std::string &host) { this->host_ = host; }
  void set_port(uint16_t port) { this->port_ = port; }
  void set_path(const std::string &path) { this->path_ = path; }
  void set_auth_token(const std::string &t) { this->auth_token_ = t; }
  void set_auth_mode(const std::string &m) { this->auth_mode_ = (m == "query") ? AuthMode::QUERY : AuthMode::BEARER; }
  void set_cacert_pem(const std::string &pem) { this->cacert_pem_ = pem; }
  void set_skip_cert_common_name_check(bool v) { this->skip_cn_check_ = v; }
  void set_uplink_sample_rate(uint32_t r) { this->uplink_sample_rate_ = r; }
  void set_return_sample_rate(uint32_t r) { this->return_sample_rate_ = r; }
  void set_half_duplex(bool v) { this->half_duplex_ = v; }
  void set_speaker_volume(float v) { this->speaker_volume_ = v; }
  void set_room(const std::string &r) { this->room_ = r; }
  void set_satellite_id(const std::string &s) { this->satellite_id_ = s; }
  void set_vad_rms_threshold(uint32_t v) { this->vad_rms_threshold_ = v; }
  void set_vad_ambient_margin(float v) { this->vad_ambient_margin_ = v; }
  void set_vad_silence_floor_max(uint32_t v) { this->vad_silence_floor_max_ = v; }
  void set_silence_ms(uint32_t v) { this->silence_ms_ = v; }
  void set_min_speech_ms(uint32_t v) { this->min_speech_ms_ = v; }
  void set_max_turn_ms(uint32_t v) { this->max_turn_ms_ = v; }
  void set_uplink_rb_bytes(uint32_t v) { this->uplink_rb_bytes_ = v; }
  void set_play_rb_bytes(uint32_t v) { this->play_rb_bytes_ = v; }
  void set_stt_timeout_ms(uint32_t v) { this->stt_timeout_ms_ = v; }
  void set_think_timeout_ms(uint32_t v) { this->think_timeout_ms_ = v; }
  void set_speak_timeout_ms(uint32_t v) { this->speak_timeout_ms_ = v; }

  // --- Public API (called from YAML lambdas) — all NON-BLOCKING ---
  // Open a turn: arm LISTENING + signal the tx task. NO socket I/O here.
  void start_turn(const std::string &wake_word);
  // Force end-of-uplink now (e.g. button): set eos flag; tx task flushes.
  void stop_turn();
  // Barge-in: stop playback, request abort; tx task sends {abort}.
  void abort_turn();
  // Physical mute switch (GPIO3) — hard-gates the uplink.
  void set_hardware_muted(bool muted);
  // Live volume change from the rotary dial (GPIO16/18). Clamps to [0.30, 0.85],
  // stores speaker_volume_, and applies it to the DAC immediately IF the warmup
  // unmute (enter_speaking_) has already run; otherwise it is applied on first
  // SPEAKING. NON-BLOCKING (member store + a single aic3204 I2C write) — safe to
  // call from the main loop (YAML lambda). The warmup logic still owns the first
  // unmute via speaker_volume_set_.
  void set_volume_runtime(float v);
  // OTA progress (LED-Paket 1, 2026-07-08): paint the ring as a fill arc DIRECTLY.
  // The YAML ota.on_progress trigger fires on the main task INSIDE the blocking OTA
  // receive loop — drive_led_()/LightState::loop() do not run while flashing, so this
  // method writes the pixel buffer itself and hand-flushes via this->led_->loop()
  // (same task, no race; LightState::loop() just writes the scheduled frame).
  void set_ota_progress(float pct);

 protected:
  // ---- mic callback context (1) ----
  // Receives already-converted PCM16 mono from the passive MicrophoneSource. Writes
  // uplink_rb_ (single producer), runs the RMS VAD, sets eos_requested_. No socket.
  void on_pcm16_(const std::vector<uint8_t> &data);

  // ---- loop() context (2) ----
  void drain_play_to_speaker_();   // play_rb_ -> speaker, backpressure-aware
  void service_turn_state_();      // watchdog deadlines + deferred turn-end
  void drive_led_();               // LED per state transition (loop-only; LightCall here)
  void led_addr_engage_(light::AddressableLight *addr);  // shared addressable one-shot engage (loop-only)
  // Fetch the addressable output (nullptr if unavailable) and optionally engage it —
  // shared guard for every animated branch (LED-Paket 1). loop()/main-task only.
  light::AddressableLight *led_addr_(bool engage);
  // VU envelope feed (LED-Paket 1): peak |sample| of a PCM16 chunk just drained to the
  // speaker -> play_env_. Called from drain_play_to_speaker_ (loop-only, plain member).
  void vu_feed_(const uint8_t *bytes, size_t len);
  void enter_speaking_();          // mww.stop + speaker.start + state SPEAKING (loop-only)
  void to_idle_();                 // clear turn + restart mww (loop-only)
  void to_error_(const char *why); // transient ERROR state (loop-only)

  // ---- tx task context (3) ----
  static void tx_task_trampoline_(void *arg);
  void tx_task_run_();             // the dedicated send loop
  bool connect_();                 // build config + start client (off-loop)
  void disconnect_();
  bool send_text_(const std::string &json);
  bool send_binary_(const uint8_t *data, size_t len);
  void send_start_frame_(uint32_t turn_id);
  void send_stop_frame_();
  void send_abort_frame_(uint32_t turn_id);
  // Pull uplink_rb_ -> fixed PSRAM buffer (buf+*len), clamped to cap. Forces eos
  // (no realloc, no OOM) when the buffer is full.
  void tx_drain_uplink_(uint8_t *buf, size_t *len, size_t cap);
  // Write the 44-byte WAV header into buf[0..44] and send buf[0..wav_len] in
  // <=16KB binary frames. No second large allocation.
  void tx_send_wav_(uint8_t *buf, size_t wav_len, uint32_t turn_id);

  // ---- ws_event_ context (4) ----
  static void ws_event_(void *handler_args, const char *base, int32_t event_id, void *event_data);
  // Dispatch a (possibly very large) reassembled text frame held in PSRAM. For all
  // small control frames a transient std::string copy is built (harmless, no base64);
  // for llm_audio the base64 is located + stream-decoded IN PLACE from `buf` so the
  // whole-frame-size buffer is never duplicated into internal RAM.
  void on_ws_text_(const char *buf, size_t len);
  void on_ws_binary_(const uint8_t *data, size_t len);  // future opt-in binary path
  // STREAM-decode the base64 WAV value [b64, b64+b64_len) DIRECTLY into play_rb_ in
  // small chunks — never materialises the whole decoded WAV (downlink OOM fix). The
  // base64 bytes are read in place from the reassembled JSON; no second full copy.
  void handle_llm_audio_(const char *b64, size_t b64_len);

  // ---- helpers (any context, no shared mutable state) ----
  const char *effective_cacert_pem_() const;
  static void wav_header_(std::vector<uint8_t> &out, uint32_t sample_rate, uint32_t data_size);
  static bool json_get_string_(const std::string &json, const char *key, std::string &out);
  // Locate a string value's RAW span in place over a (const char*, len) buffer — no
  // copy. On success sets [val_off, val_off+val_len) to the bytes between the quotes
  // within `buf`. Returns the raw (still-escaped) span; for base64 values (no '\'
  // chars, no quotes) this is byte-identical to the value, which is all the
  // llm_audio stream-decode path needs. Used to parse the huge llm_audio frame
  // straight out of the PSRAM reassembly buffer without an internal-RAM copy.
  static bool json_find_string_span_raw_(const char *buf, size_t len, const char *key,
                                         size_t &val_off, size_t &val_len);
  static bool json_get_number_(const std::string &json, const char *key, long &out);
  // night_mode frame needs both (json_get_number_ is integer-only): a float value
  // ("dim":0.25) and a bare JSON bool ("active":true — unquoted literal).
  static bool json_get_float_(const std::string &json, const char *key, float &out);
  static bool json_get_bool_(const std::string &json, const char *key, bool &out);
  static std::vector<uint8_t> base64_decode_(const std::string &in);

  // ---- VAD (mic callback context only) ----
  void vad_reset_();
  bool vad_feed_(const int16_t *samples, size_t n_samples);
  // Block RMS over every stride-th sample (stride>1 = cheap ambient tracking at idle).
  static double block_rms_(const int16_t *samples, size_t n, size_t stride);

  // =====================================================================
  // State
  // =====================================================================
  microphone::MicrophoneSource *mic_source_{nullptr};
  speaker::Speaker *speaker_{nullptr};
  light::LightState *led_{nullptr};
  micro_wake_word::MicroWakeWord *mww_{nullptr};

  std::string host_;
  uint16_t port_{8081};
  std::string path_{"/ws/audio"};
  std::string auth_token_;
  AuthMode auth_mode_{AuthMode::BEARER};
  std::string cacert_pem_;
  bool skip_cn_check_{false};
  uint32_t uplink_sample_rate_{16000};
  uint32_t return_sample_rate_{24000};
  bool half_duplex_{true};
  // Output volume forwarded to the speaker chain (resampler -> i2s speaker -> aic3204
  // DAC). MUST be > 0 or the aic3204 stays at its silent default (DAC digital vol -127):
  // the aic3204 setup() defers write_volume_() by 2.5 s with volume_=0, clobbering the
  // i2s speaker's boot-time set_volume(1.0); nothing re-asserts it (we have no
  // media_player, unlike upstream). We re-assert it on every enter_speaking_ — well
  // after the 2.5 s DAC settle — so analog output is actually produced. (See aic3204.cpp
  // write_volume_ + the deferred set_timeout(2500,...) in aic3204::setup.)
  std::atomic<float> speaker_volume_{0.85f};  // cross-task: written by loop()/YAML set_volume_runtime, read by ws enter_speaking_
  std::string room_;
  std::string satellite_id_;

  // Energy/RMS VAD config (HW-tuning knobs, red-team fix #6).
  uint32_t vad_rms_threshold_{700};
  uint32_t silence_ms_{900};
  uint32_t min_speech_ms_{50};
  uint32_t max_turn_ms_{10000};
  // Adaptive endpointing vs. background speech (TV, 2026-07-17): see vad_feed_.
  float vad_ambient_margin_{1.30f};
  uint32_t vad_silence_floor_max_{2000};

  // Buffer sizing + watchdog deadlines.
  uint32_t uplink_rb_bytes_{65536};
  // Play ring (PSRAM, EXTERNAL_FIRST): bumped to 256 KB so a single ~260 KB llm_audio
  // frame (one ~5.4 s 24k mono PCM reply) fits comfortably without timing pressure on
  // the loop()-side drain. Cheap in PSRAM (defensive fix #3).
  uint32_t play_rb_bytes_{262144};
  uint32_t stt_timeout_ms_{15000};
  uint32_t think_timeout_ms_{30000};
  uint32_t speak_timeout_ms_{60000};

  // VAD runtime (mic callback context only; no cross-task access).
  bool vad_speech_started_{false};
  uint32_t vad_voiced_ms_{0};
  uint32_t vad_silence_run_ms_{0};
  // Room-ambient RMS floor (TV, appliances …), slow EMA tracked OUTSIDE of LISTENING
  // — the passive MicrophoneSource keeps delivering blocks at idle because
  // micro_wake_word is the always-on mic consumer. Mic-cb context only, plain.
  float vad_ambient_rms_{0.0f};
  // Uplink RMS high-water + dual-block logging (flash-day calibration, fix #6/#3).
  uint32_t uplink_rms_peak_{0};

  // ---- Cross-task shared state (atomics + single-producer/consumer rings) ----
  std::atomic<bool> connected_{false};        // set by ws_event_ (4), read everywhere
  std::atomic<bool> hardware_muted_{false};   // set by loop()/YAML, read by mic cb
  std::atomic<TurnState> turn_state_{TurnState::IDLE};
  std::atomic<uint32_t> turn_id_{0};          // monotonic; current turn's id
  // tx-task signalling:
  std::atomic<bool> start_requested_{false};  // loop -> tx: open a turn
  std::atomic<bool> eos_requested_{false};    // mic cb / loop -> tx: flush WAV + {stop}
  std::atomic<bool> abort_requested_{false};  // loop -> tx: send {abort}, drop turn
  std::atomic<bool> tx_should_exit_{false};   // shutdown
  // play_rb_ producer-quiesce guard (red-team fix #2): the IDF ws task sets this true
  // while it is actively writing decoded llm_audio; loop()'s to_idle_ must not reset
  // until it is false (no straggler audio after llm_done).
  std::atomic<bool> producer_active_{false};
  std::atomic<bool> llm_done_seen_{false};    // ws -> loop: server finished the turn
  std::atomic<uint32_t> play_grace_deadline_ms_{0};  // small window after llm_done

  // Single-producer (mic cb) / single-consumer (tx task) uplink ring.
  std::unique_ptr<ring_buffer::RingBuffer> uplink_rb_;
  // Single-producer (ws task) / single-consumer (loop) play ring.
  std::unique_ptr<ring_buffer::RingBuffer> play_rb_;

  // loop()-only turn-state bookkeeping.
  uint32_t state_entered_ms_{0};   // millis() when current state was entered (watchdog)
  TurnState led_last_{TurnState::ERROR};  // last LED state pushed (force first paint)

  // ---- LED visualization (loop()/drive_led_ context only — loop-safe) ----
  // Volume-overlay deadline: written in set_volume_runtime (main-loop / YAML lambda),
  // read in drive_led_ (loop). Same (main-loop) context -> plain uint32_t is safe.
  uint32_t volume_display_until_ms_{0};
  // Boot/connect breathing + confirm edge detect. drive_led_-only -> plain is safe.
  bool was_connected_{false};                  // edge detector for connected_ false->true
  uint32_t connect_confirm_until_ms_{0};       // green "online" confirm window deadline
  // When an overlay/animation window (volume / breathing / confirm) ENDS, the edge-
  // gated state path would stay stuck on led_last_; this flag forces one repaint.
  bool led_force_repaint_{false};
  // Addressable-pixel engagement latch (drive_led_/loop-only -> plain bool is safe).
  // Shared by EVERY addressable branch: the volume level-bar (a), the boot comet (b)
  // and the "online" fill-sweep (c). While engaged we set the AddressableLight's
  // effect_active flag so its own update_state()/transformer stop overwriting our direct
  // per-LED writes (exactly the mechanism addressable effects use). Set true once on the
  // first addressable frame (one-shot make_call ON + full brightness + transition 0),
  // cleared once on the first uniform (d) frame so (d) repaints the turn-state colour and
  // no animation pixels are left hanging.
  bool led_addr_active_{false};
  // ---- LED-Paket 1 (2026-07-08, maintainer sign-off) ----
  // One-shot animation windows, all written on the main loop (YAML lambdas / loop
  // callbacks) and read in drive_led_ -> plain members are safe (same task).
  uint32_t wake_spark_until_ms_{0};  // (c3) white->cyan bloom right after wake
  uint32_t stop_ack_until_ms_{0};    // (c2) red ack sweep after abort_turn()
  // VU envelope 0..1: fed by vu_feed_ (drain, loop) with the peak of each drained
  // PCM16 chunk, exponentially decayed per drive_led_ frame. loop-only, plain.
  float play_env_{0.0f};
  // timer_state downlink (ws task WRITES, loop READS -> atomics). total<=0 = no
  // timer. loop() extrapolates remaining locally from rx timestamp between pushes.
  std::atomic<int32_t> timer_total_s_{0};
  std::atomic<int32_t> timer_remaining_rx_s_{0};
  std::atomic<uint32_t> timer_rx_ms_{0};
  // speaker downlink accent (ws task WRITES, loop READS). 0 = unset; else
  // 0x01RRGGBB (flag bit distinguishes "guest grey" from "never set").
  std::atomic<uint32_t> speaker_accent_rgb_{0};
  std::atomic<uint32_t> speaker_flash_until_ms_{0};
  // ---- Nachtmodus (LED-Paket 2, maintainer sign-off 15.07) ----
  // Server-pushed night_mode frame (ws task WRITES, loop/main READS -> atomics).
  // active=false => factor 1.0 (normal). dim = global 0..1 factor on all LED
  // effects; interaction feedback additionally gets a visibility floor in
  // drive_led_, pure info glows (timer arc / mute pixel) may reach 0.
  std::atomic<bool> night_active_{false};
  std::atomic<float> night_dim_{0.25f};
  // NVS persistence: the ws task only marks dirty; loop() does the actual save
  // (single writer to the preference object, no cross-task NVS access). Persisted
  // so a 3-a.m. reboot boots DIM — the boot comet runs before the lazy ws connect
  // could re-push the state.
  struct NightModePref {
    bool active;
    float dim;
  };
  std::atomic<bool> night_pref_dirty_{false};
  ESPPreferenceObject night_pref_;  // loop()/setup() only
  // Night-factor edge gate for the uniform (d) path: (d) is repainted only on state
  // change — a night_mode flip mid-state must also trigger one repaint. loop-only.
  float led_night_last_{-1.0f};
  // START-ONCE speaker lifecycle (fix #1): the i2s_audio_speaker bus channel is released
  // ASYNCHRONOUSLY (stop_i2s_driver_ -> parent_->unlock() only runs when the speaker task
  // reaches TASK_STOPPED, several loop() iterations after finish()). A per-turn
  // start()/finish() therefore races the next turn's start() -> try_lock() fails ->
  // "Parent bus is busy" (i2s_audio_speaker_standard.cpp:311) -> 1 s retry. Instead we
  // start the speaker chain ONCE (first turn) and keep it RUNNING; the i2s task self-fills
  // silence between turns (always-fill model, exactly upstream's `timeout: never`). Mic
  // half-duplex is still gated by turn_state_ + mww.stop()/start(), so duplex is unaffected.
  bool speaker_running_{false};    // did we start() the speaker chain (once, kept running)?
  std::atomic<bool> speaker_volume_set_{false}; // re-asserted DAC vol/unmute post-2.5s settle? set by ws enter_speaking_, read by loop()/YAML set_volume_runtime
  bool mww_running_{true};          // tracks our stop()/start() of the wake detector
  // Consumer-local carry for bytes read from play_rb_ but not yet accepted by the
  // speaker (backpressure). Kept HERE (not re-written to the ring) so we never
  // reorder against the concurrent ws producer. loop()-only.
  std::vector<uint8_t> play_pending_;
  size_t play_pending_pos_{0};

  // ws_event_ reassembly buffer (only touched on the IDF ws task). A large
  // fragmented text frame (e.g. a single llm_audio carrying ~200 KB of base64)
  // must NOT live in INTERNAL RAM — back it with PSRAM via RAMAllocator (external
  // first, internal fallback), mirroring the uplink tx buffer idiom. Grown once on
  // demand, reused across frames; never shrunk (bounded by the largest frame seen).
  char *rx_buf_{nullptr};       // PSRAM-backed reassembly storage (capacity rx_cap_)
  size_t rx_cap_{0};            // allocated capacity of rx_buf_
  size_t rx_len_{0};            // bytes currently reassembled
  // Ensure rx_buf_ has at least `need` bytes of capacity (PSRAM-first). Returns
  // false if the (PSRAM+internal) allocation fails. ws-task context only.
  bool rx_reserve_(size_t need);

  // esp_websocket_client handle (opaque void* so the header needs no IDF include).
  void *ws_{nullptr};

  // tx task handle.
#ifdef USE_ESP32
  TaskHandle_t tx_task_{nullptr};
#endif

  // ---- Embedded TLS trust anchor (contract §A Leaf-Pinning) ----------
  //   0.8-cutover leaf (hoshi-server:8082, since 2026-07-08). SHA-256 (verify at flash time):
  //   berechne ihn selbst: openssl x509 -in <leaf.pem> -noout -fingerprint -sha256
  static const char *const SERVER_LEAF_PEM;
};

}  // namespace hoshi_ws_audio
}  // namespace esphome
