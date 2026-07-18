// =============================================================================
// hoshi_ws_audio.cpp — Route B "done right" (NO network on the ESPHome main loop)
// =============================================================================
//
// See hoshi_ws_audio.h for the 4-context architecture + concurrency model.
// Contract authority: docs/PROTOCOL.md (wire spec) in this repo.
// Proven reference:    tools/bridge/napi_bridge.py (a Python no-flash test client
//                       that speaks the exact same /ws/audio wire protocol).
// =============================================================================

#include "hoshi_ws_audio.h"
#include "esphome/core/log.h"

// LED + mww are optional handles wired from YAML; include their full defs only here.
#include "esphome/components/light/light_state.h"
#include "esphome/components/light/light_call.h"
// Volume overlay (a) renders a REAL per-LED level bar by writing pixels directly on
// the AddressableLight buffer (the esp32_rmt_led_strip output). Needs the full
// AddressableLight/ESPColorView def + Color. drive_led_ (loop) only.
#include "esphome/components/light/addressable_light.h"
#include "esphome/core/color.h"
#include "esphome/components/network/util.h"  // network::is_connected() for the boot LED
#ifdef USE_MICRO_WAKE_WORD
#include "esphome/components/micro_wake_word/micro_wake_word.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>

// WS client = esp-idf esp_websocket_client (managed IDF component, ^1.7.0).
#if defined(USE_ESP_IDF) && __has_include("esp_websocket_client.h")
#include "esp_websocket_client.h"
#define HOSHI_HAVE_WS_CLIENT 1
#endif

namespace esphome {
namespace hoshi_ws_audio {

static const char *const TAG = "hoshi_ws_audio";

// Bridge-proven: <=16 KB per binary frame (Hoshi EOFs oversize frames).
static const size_t UPLINK_CHUNK = 16384;
// tx-task local accumulation chunk when pulling from the uplink ring.
static const size_t TX_PULL_CHUNK = 4096;
// After llm_done + play_rb_ drained + speaker idle, wait this long for the
// resampler/i2s sink to flush its own tail before ending the turn.
static const uint32_t SPEAKER_TAIL_MS = 250;
// Grace window after llm_done before we consider the play producer quiesced (fix #2):
// tolerates a straggler llm_audio that races llm_done over the wire.
static const uint32_t PLAY_PRODUCER_GRACE_MS = 300;
// Night mode (LED package 2): visibility floor for INTERACTION feedback (wake spark,
// listening, VU, stop-ack, boot/OTA) — whoever speaks to Hoshi at night still needs
// to see her react. Pure info glows (timer arc, mute pixel) bypass the floor and may
// dim to 0. Below this raw dim value an info glow is treated as "off" entirely.
static const float NIGHT_FLOOR = 0.12f;
static const float NIGHT_INFO_OFF = 0.02f;
#ifdef HOSHI_HAVE_WS_CLIENT
static const TickType_t WS_SEND_TIMEOUT = pdMS_TO_TICKS(2000);  // blocks ONLY the tx task
static const TickType_t TX_NOTIFY_WAIT = pdMS_TO_TICKS(50);     // tx loop poll cadence
#endif

// -----------------------------------------------------------------------------
// Embedded TLS trust anchor — TOFU leaf-pinning placeholder.
//
// The reference deployment embeds its Hoshi server's self-signed HTTPS leaf
// cert here as the esp-tls trust anchor (Trust-On-First-Use pinning: the
// leaf IS its own root, no CA involved). That cert is unique to one specific
// server IP and private key, so it is NOT included in this public repo —
// paste in YOUR OWN server's leaf cert before building, e.g.:
//
//   echo | openssl s_client -connect <your-hoshi-server-ip>:<port> 2>/dev/null \
//     | openssl x509 -outform PEM
//
// then verify the SHA-256 fingerprint you got matches what your server
// actually serves before flashing (`openssl x509 -noout -fingerprint -sha256 -in leaf.pem`).
// Alternatively set `cacert_pem` in the YAML to override this default at
// runtime instead of recompiling. See docs/PROTOCOL.md §1 (TLS) for the
// full leaf-pinning rationale.
// -----------------------------------------------------------------------------
const char *const HoshiWsAudio::SERVER_LEAF_PEM =
    "-----BEGIN CERTIFICATE-----\n"
    "REPLACE-WITH-YOUR-OWN-SERVER-LEAF-CERT-BASE64-LINES\n"
    "-----END CERTIFICATE-----\n";

const char *HoshiWsAudio::effective_cacert_pem_() const {
  return this->cacert_pem_.empty() ? SERVER_LEAF_PEM : this->cacert_pem_.c_str();
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------
void HoshiWsAudio::setup() {
  ESP_LOGCONFIG(TAG, "Setting up hoshi_ws_audio (Route B, off-loop networking)");

  // Night mode (LED package 2): restore the last server-pushed state from NVS so a
  // night-time reboot boots DIM — the boot comet runs BEFORE the lazy ws connect
  // could re-push the state. The server re-pushes on every connect (source of truth).
  this->night_pref_ = global_preferences->make_preference<NightModePref>(fnv1_hash("hoshi_night_mode_v1"));
  NightModePref np{};
  if (this->night_pref_.load(&np)) {
    this->night_active_.store(np.active);
    float d = np.dim;
    if (d < 0.0f) d = 0.0f;
    if (d > 1.0f) d = 1.0f;
    this->night_dim_.store(d);
    ESP_LOGI(TAG, "night_mode restored from NVS: active=%s dim=%.2f", YESNO(np.active), d);
  }

  // Allocate the SPSC rings (PSRAM-first; ~64 KB each).
  this->uplink_rb_ = ring_buffer::RingBuffer::create(this->uplink_rb_bytes_);
  this->play_rb_ = ring_buffer::RingBuffer::create(this->play_rb_bytes_);
  if (this->uplink_rb_ == nullptr || this->play_rb_ == nullptr) {
    ESP_LOGE(TAG, "ring buffer alloc failed (uplink=%p play=%p)",
             (void *) this->uplink_rb_.get(), (void *) this->play_rb_.get());
    this->mark_failed();
    return;
  }

  // HARD INVARIANT (red-team fix #1): set the speaker's input format BEFORE the
  // first start()/play(). The speaker receives our return stream resampled to the
  // device uplink rate is NOT what we do anymore: we feed the DECODED 24k PCM16
  // straight in, and the YAML resampler does 24k->48k. So the speaker input is
  // (16-bit, 1 ch, return_sample_rate_ = 24000). Re-asserted in enter_speaking_().
  if (this->speaker_ != nullptr) {
    this->speaker_->set_audio_stream_info(
        audio::AudioStreamInfo(16, 1, this->return_sample_rate_));
  } else {
    ESP_LOGE(TAG, "no speaker bound — playback disabled");
  }

  // Bind the PASSIVE microphone source. It never starts/stops i2s_mics; it only
  // forwards audio while micro_wake_word (the active consumer) has it running. We
  // get already channel-selected, 32->16-bit converted, gain-applied PCM16 mono.
  if (this->mic_source_ != nullptr) {
    this->mic_source_->add_data_callback(
        [this](const std::vector<uint8_t> &d) { this->on_pcm16_(d); });
    // passive => no start() call; mic is powered by micro_wake_word.
  } else {
    ESP_LOGE(TAG, "no microphone source bound — uplink disabled");
  }

  // Dedicated tx task: the ONLY caller of esp_websocket_client send. Pinned to
  // core 1 (the IDF ws task uses tskNO_AFFINITY by default; we keep our send task
  // off a fixed core-0 collision). ~6 KB stack.
#ifdef USE_ESP32
  xTaskCreatePinnedToCore(&HoshiWsAudio::tx_task_trampoline_, "hoshi_ws_tx",
                          6144, this, 5 /*prio*/, &this->tx_task_, 1 /*core*/);
  if (this->tx_task_ == nullptr)
    ESP_LOGE(TAG, "failed to create hoshi_ws_tx task");
#endif

  this->state_entered_ms_ = millis();
  // NOTE: connection is opened LAZILY by the tx task on the first turn (off-loop),
  // NOT here — this is what removes the api/on_boot wake regression.
}

void HoshiWsAudio::loop() {
  // The loop NEVER touches the socket. It only: drains play_rb_, runs the turn
  // state machine + watchdog, and paints the LED.
  this->drain_play_to_speaker_();
  this->service_turn_state_();
  this->drive_led_();
  // Nachtmodus: persist a fresh server push to NVS from HERE (loop = the single
  // writer of night_pref_; the ws task only flips the dirty flag). exchange()
  // drains the flag, so each push is saved exactly once.
  if (this->night_pref_dirty_.exchange(false)) {
    NightModePref np{this->night_active_.load(), this->night_dim_.load()};
    this->night_pref_.save(&np);
  }
}

void HoshiWsAudio::dump_config() {
  ESP_LOGCONFIG(TAG, "hoshi_ws_audio:");
  ESP_LOGCONFIG(TAG, "  endpoint: wss://%s:%u%s", this->host_.c_str(), this->port_, this->path_.c_str());
  ESP_LOGCONFIG(TAG, "  auth_mode: %s", this->auth_mode_ == AuthMode::BEARER ? "bearer" : "query");
  ESP_LOGCONFIG(TAG, "  auth_token: %s", this->auth_token_.empty() ? "<UNSET>" : "<set>");
  ESP_LOGCONFIG(TAG, "  tls cert_pem: %s  skip_cn_check=%s",
                this->cacert_pem_.empty() ? "<embedded server leaf>" : "<yaml override>",
                YESNO(this->skip_cn_check_));
  ESP_LOGCONFIG(TAG, "  uplink: WAV PCM16 %u Hz mono (<=%uB chunks)",
                this->uplink_sample_rate_, (unsigned) UPLINK_CHUNK);
  ESP_LOGCONFIG(TAG, "  return: PCM16 %u Hz mono -> speaker (YAML resampler -> 48k)",
                this->return_sample_rate_);
  ESP_LOGCONFIG(TAG, "  rings: uplink=%uB play=%uB", this->uplink_rb_bytes_, this->play_rb_bytes_);
  ESP_LOGCONFIG(TAG, "  half_duplex: %s", YESNO(this->half_duplex_));
  ESP_LOGCONFIG(TAG, "  speaker_volume: %.2f (aic3204 DAC vol + unmute, re-asserted post-2.5s settle)",
                this->speaker_volume_.load());
  ESP_LOGCONFIG(TAG, "  watchdog: stt=%ums think=%ums speak=%ums",
                this->stt_timeout_ms_, this->think_timeout_ms_, this->speak_timeout_ms_);
  ESP_LOGCONFIG(TAG, "  energy VAD: rms_thr=%u silence=%ums min_speech=%ums max_turn=%ums",
                this->vad_rms_threshold_, this->silence_ms_, this->min_speech_ms_, this->max_turn_ms_);
  ESP_LOGCONFIG(TAG, "  endpointing: room-relative (ambient×%.2f, floor_max=%u; ambient now=%.0f)",
                this->vad_ambient_margin_, this->vad_silence_floor_max_, this->vad_ambient_rms_);
  ESP_LOGCONFIG(TAG, "  led: %s  micro_wake_word: %s",
                this->led_ ? "bound" : "none", this->mww_ ? "bound" : "none");
  ESP_LOGCONFIG(TAG, "  identity: room=%s satelliteId=%s (sent in start frame when set)",
                this->room_.empty() ? "<unset>" : this->room_.c_str(),
                this->satellite_id_.empty() ? "<unset>" : this->satellite_id_.c_str());
  ESP_LOGCONFIG(TAG, "  night_mode: active=%s dim=%.2f (NVS-restored; server re-pushes on connect)",
                YESNO(this->night_active_.load()), this->night_dim_.load());
#ifndef HOSHI_HAVE_WS_CLIENT
  ESP_LOGW(TAG, "  esp_websocket_client header NOT resolved — built without WS");
#endif
}

// =============================================================================
// (2) loop() context — turn state machine OWNER, play drain, LED. No I/O.
// =============================================================================

// Public API: all NON-BLOCKING. They only flip atomics; the tx task does the work.
void HoshiWsAudio::start_turn(const std::string &wake_word) {
  TurnState st = this->turn_state_.load();
  if (st != TurnState::IDLE) {
    ESP_LOGW(TAG, "wake while turn active (state=%d) -> ignoring (half-duplex)", (int) st);
    return;
  }
  if (this->hardware_muted_.load()) {
    ESP_LOGW(TAG, "wake while hardware-muted -> ignoring");
    return;
  }
  uint32_t id = this->turn_id_.fetch_add(1) + 1;
  // Reset per-turn cross-task flags BEFORE arming LISTENING.
  this->eos_requested_.store(false);
  this->abort_requested_.store(false);
  this->llm_done_seen_.store(false);
  this->play_grace_deadline_ms_.store(0);
  this->vad_reset_();
  if (this->uplink_rb_ != nullptr)
    this->uplink_rb_->reset();
  this->turn_state_.store(TurnState::LISTENING);
  this->state_entered_ms_ = millis();
  // LED-Paket 1 (c3): white->cyan wake bloom. start_turn only runs on the main loop
  // (YAML wake lambda), same context as drive_led_ -> plain member.
  this->wake_spark_until_ms_ = millis() + 240;
  this->start_requested_.store(true);  // tx task: connect (if needed) + send start
#ifdef USE_ESP32
  if (this->tx_task_ != nullptr)
    xTaskNotifyGive(this->tx_task_);
#endif
  ESP_LOGI(TAG, "WAKE wake_word=%s -> turn %u (LISTENING)", wake_word.c_str(), (unsigned) id);
}

void HoshiWsAudio::stop_turn() {
  if (this->turn_state_.load() != TurnState::LISTENING) {
    ESP_LOGD(TAG, "stop_turn: not LISTENING -> ignore");
    return;
  }
  ESP_LOGI(TAG, "stop_turn (manual) -> requesting eos");
  this->eos_requested_.store(true);
#ifdef USE_ESP32
  if (this->tx_task_ != nullptr)
    xTaskNotifyGive(this->tx_task_);
#endif
}

void HoshiWsAudio::abort_turn() {
  ESP_LOGI(TAG, "abort_turn (barge-in) turn=%u", (unsigned) this->turn_id_.load());
  // Stop playback immediately so the user is heard. With start-once (fix #1) we do NOT
  // stop the speaker chain (that would release the i2s bus + re-trigger "Parent bus is
  // busy"); instead we drop our queued + carried PCM so the always-running speaker
  // immediately reverts to self-filled silence. to_idle_ below flips out of SPEAKING,
  // which also stops drain_play_to_speaker_ from feeding any further audio.
  if (this->play_rb_ != nullptr)
    this->play_rb_->reset();
  this->play_pending_.clear();
  this->play_pending_pos_ = 0;
  this->abort_requested_.store(true);
#ifdef USE_ESP32
  if (this->tx_task_ != nullptr)
    xTaskNotifyGive(this->tx_task_);
#endif
  // LED-Paket 1 (c2): short red ack sweep — "heard you, cancelling". All abort_turn
  // callers run on the main loop (button/wake lambdas, mute cb) -> plain member.
  this->stop_ack_until_ms_ = millis() + 450;
  // The turn returns to IDLE here (loop owns it). The tx task sends {abort} async.
  this->to_idle_();
}

void HoshiWsAudio::set_hardware_muted(bool muted) {
  this->hardware_muted_.store(muted);
  ESP_LOGI(TAG, "hardware mute = %s", YESNO(muted));
  if (muted && this->turn_state_.load() == TurnState::LISTENING) {
    ESP_LOGI(TAG, "hardware mute during LISTENING -> aborting turn");
    this->abort_turn();
  }
}

// Live volume from the rotary dial (GPIO16/18). hoshi_ws_audio stays the sole owner
// of the speaker volume. Clamp to the verified [0.30, 0.85] window (0.85 = the
// distortion-safe Class-D ceiling), store it, and — IF the warmup unmute already ran
// (speaker_volume_set_) — push it to the aic3204 DAC immediately (single I2C write,
// non-blocking). Before the first SPEAKING we only store it; enter_speaking_ applies
// speaker_volume_ on the first unmute. The warmup logic is left untouched.
void HoshiWsAudio::set_volume_runtime(float v) {
  if (v < 0.30f)
    v = 0.30f;
  if (v > 0.85f)
    v = 0.85f;
  this->speaker_volume_ = v;
  if (this->speaker_volume_set_ && this->speaker_ != nullptr)
    this->speaker_->set_volume(v);
  // LED feedback: show the level overlay for ~1.2 s (read in drive_led_, same main-loop
  // context -> plain uint32_t, no atomic needed).
  this->volume_display_until_ms_ = millis() + 1200;
  ESP_LOGI(TAG, "volume set to %.2f (live=%d)", v, (int) this->speaker_volume_set_);
}

// Drain play_rb_ -> speaker, honouring play()'s accepted-bytes return (backpressure).
// Bytes read from the ring but not accepted by the speaker are held in a consumer-
// local carry (play_pending_) — NEVER re-written to the ring, so we never reorder
// against the concurrent ws producer. Stream ordering is thus: pending first, ring next.
void HoshiWsAudio::drain_play_to_speaker_() {
  if (this->speaker_ == nullptr || this->play_rb_ == nullptr)
    return;
  // Only stream while we are SPEAKING. The speaker chain stays RUNNING between turns
  // (start-once, fix #1) and self-fills silence; we just stop feeding it real PCM once
  // the turn is over. enter_speaking_ is triggered by the ws dispatch when audio arrives.
  if (this->turn_state_.load() != TurnState::SPEAKING)
    return;

  // 1) Flush any carried-over bytes from a previous partial write first.
  while (this->play_pending_pos_ < this->play_pending_.size()) {
    size_t remaining = this->play_pending_.size() - this->play_pending_pos_;
    size_t wrote = this->speaker_->play(this->play_pending_.data() + this->play_pending_pos_,
                                        remaining, /*ticks=*/0);
    this->play_pending_pos_ += wrote;
    if (wrote == 0)
      return;  // speaker still full this iteration
  }
  this->play_pending_.clear();
  this->play_pending_pos_ = 0;

  // 2) Drain fresh data from the ring.
  uint8_t buf[1024];
  size_t avail = this->play_rb_->available();
  while (avail > 0) {
    size_t want = avail < sizeof(buf) ? avail : sizeof(buf);
    size_t got = this->play_rb_->read(buf, want, 0);
    if (got == 0)
      break;
    // LED-Paket 1: feed the VU envelope with what we are about to play (the ring holds
    // header-stripped PCM16). Same loop context as drive_led_ -> plain member.
    this->vu_feed_(buf, got);
    size_t wrote = this->speaker_->play(buf, got, /*ticks=*/0);
    if (wrote < got) {
      // Carry the unaccepted tail locally (NOT back to the ring) until next loop().
      size_t leftover = got - wrote;
      this->play_pending_.assign(buf + wrote, buf + wrote + leftover);
      this->play_pending_pos_ = 0;
      break;
    }
    avail = this->play_rb_->available();
  }
}

// VU envelope feed (LED-Paket 1): track the peak |sample| of each PCM16 chunk drained
// to the speaker. drive_led_ decays play_env_ per frame, so the ring's glow follows the
// speech energy (loud syllables bright, pauses dark). Odd trailing byte is ignored —
// the VU is an approximation, not a meter. loop()-only.
void HoshiWsAudio::vu_feed_(const uint8_t *bytes, size_t len) {
  const int16_t *s = reinterpret_cast<const int16_t *>(bytes);
  size_t n = len / 2;
  int32_t peak = 0;
  for (size_t i = 0; i < n; i++) {
    int32_t v = s[i];
    if (v < 0)
      v = -v;
    if (v > peak)
      peak = v;
  }
  float level = (float) peak / 32768.0f;
  if (level > this->play_env_)
    this->play_env_ = level;
}

void HoshiWsAudio::service_turn_state_() {
  TurnState st = this->turn_state_.load();
  uint32_t now = millis();

  switch (st) {
    case TurnState::LISTENING: {
      // Absolute max-turn watchdog (safety net independent of the VAD).
      if (now - this->state_entered_ms_ >= this->max_turn_ms_ && !this->eos_requested_.load()) {
        ESP_LOGI(TAG, "VAD: max turn %ums (absolute) -> forcing eos", this->max_turn_ms_);
        this->eos_requested_.store(true);
#ifdef USE_ESP32
        if (this->tx_task_ != nullptr)
          xTaskNotifyGive(this->tx_task_);
#endif
      }
      // The tx task flips the state to AWAITING_STT once it has sent the WAV+stop.
      break;
    }
    case TurnState::AWAITING_STT:
      if (now - this->state_entered_ms_ >= this->stt_timeout_ms_) {
        ESP_LOGW(TAG, "watchdog: no transcript in %ums -> error", this->stt_timeout_ms_);
        this->to_error_("stt-timeout");
      }
      break;
    case TurnState::THINKING:
      if (now - this->state_entered_ms_ >= this->think_timeout_ms_) {
        ESP_LOGW(TAG, "watchdog: LLM stalled %ums -> error", this->think_timeout_ms_);
        this->to_error_("think-timeout");
      }
      break;
    case TurnState::SPEAKING: {
      if (now - this->state_entered_ms_ >= this->speak_timeout_ms_) {
        ESP_LOGW(TAG, "watchdog: speaking stalled %ums -> error", this->speak_timeout_ms_);
        this->to_error_("speak-timeout");
        break;
      }
      // Deferred turn-end: only once the server is done, the producer has quiesced,
      // the play queue is empty AND the speaker has drained + a tail grace passed.
      if (!this->llm_done_seen_.load())
        break;
      // Producer-quiesce guard (fix #2): wait for the grace window after llm_done so
      // a straggler llm_audio can still land before we tear down.
      uint32_t grace = this->play_grace_deadline_ms_.load();
      if (grace == 0) {
        this->play_grace_deadline_ms_.store(now + PLAY_PRODUCER_GRACE_MS);
        break;
      }
      if (now < grace || this->producer_active_.load())
        break;
      bool queue_empty = ((this->play_rb_ == nullptr) || (this->play_rb_->available() == 0)) &&
                         (this->play_pending_pos_ >= this->play_pending_.size());
      // START-ONCE (fix #1): the speaker chain stays RUNNING across turns, so we do NOT
      // gate on is_running()/finish() here (that would release the i2s bus and re-trigger
      // the "Parent bus is busy" race next turn). We only wait for our OWN play queue to
      // drain + the speaker's internal buffer to empty, then let the speaker keep running
      // (self-filling silence) and return to idle after a short tail grace.
      bool spk_drained = (this->speaker_ == nullptr) || !this->speaker_->has_buffered_data();
      if (queue_empty && spk_drained) {
        // Short tail wait beyond drain (lets the DMA + DAC flush the last samples).
        if (now >= grace + SPEAKER_TAIL_MS) {
          this->to_idle_();
        }
      }
      break;
    }
    case TurnState::ERROR:
      // Hold ERROR briefly so the LED is visible, then return to IDLE.
      if (now - this->state_entered_ms_ >= 1200)
        this->to_idle_();
      break;
    case TurnState::IDLE:
    default:
      break;
  }
}

// Shared one-shot engage for every addressable branch (a/b/c). On the FIRST addressable
// frame: push make_call(state=true, brightness=1.0, transition 0) — the light must be ON
// for the buffer to reach the strip, and full brightness keeps our lit pixels independent
// of the user dim slider — then set_effect_active(true) so update_state()/the transformer
// stop overwriting our direct per-LED writes. Idempotent via led_addr_active_. loop-only.
void HoshiWsAudio::led_addr_engage_(light::AddressableLight *addr) {
  if (this->led_addr_active_)
    return;
  auto call = this->led_->make_call();
  call.set_transition_length(0);
  call.set_state(true);
  call.set_brightness(1.0f);
  call.perform();
  addr->set_effect_active(true);
  this->led_addr_active_ = true;
}

// Shared fetch(+engage) guard for every animated branch (LED-Paket 1). Returns nullptr
// when no addressable buffer is available — callers then FALL THROUGH to the uniform
// (d) path instead of rendering, so behaviour degrades to pre-Paket-1. loop/main task.
light::AddressableLight *HoshiWsAudio::led_addr_(bool engage) {
  if (this->led_ == nullptr)
    return nullptr;
  auto *addr = static_cast<light::AddressableLight *>(this->led_->get_output());
  if (addr == nullptr || addr->size() == 0)
    return nullptr;
  if (engage)
    this->led_addr_engage_(addr);
  return addr;
}

// OTA progress ring (LED package 1, maintainer decision: "only enable via options" applies
// to idle decoration — the OTA ring only runs DURING a flash, so it's exempt). Called
// from the YAML ota.on_progress trigger: that fires on the MAIN task inside the
// blocking OTA receive loop, where loop()/drive_led_()/LightState::loop() do NOT run.
// So: paint the fill arc directly and hand-flush by calling this->led_->loop() —
// same task (no race with our own loop, it is blocked), and LightState::loop() just
// writes the scheduled frame to the strip. After an OTA abort the normal loop resumes,
// finds no active animation window, disengages and repaints the state colour (the
// led_force_repaint_ below guarantees exactly one repaint).
void HoshiWsAudio::set_ota_progress(float pct) {
  auto *addr = this->led_addr_(true);
  if (addr == nullptr)
    return;
  if (pct < 0.0f)
    pct = 0.0f;
  if (pct > 100.0f)
    pct = 100.0f;
  int32_t n = addr->size();
  int32_t filled = (int32_t) lroundf((pct / 100.0f) * (float) n);
  if (filled < 1)
    filled = 1;
  if (filled > n)
    filled = n;
  // Night dim with the interaction floor: an OTA at night is user-initiated,
  // the progress must stay readable.
  float nd = this->night_active_.load() ? this->night_dim_.load() : 1.0f;
  float ndf = nd < NIGHT_FLOOR ? NIGHT_FLOOR : nd;
  if (ndf > 1.0f) ndf = 1.0f;
  const Color fill((uint8_t) lroundf(20.0f * ndf), (uint8_t) lroundf(160.0f * ndf),
                   (uint8_t) lroundf(60.0f * ndf));    // calm green arc = progress
  const Color head((uint8_t) lroundf(120.0f * ndf), (uint8_t) lroundf(255.0f * ndf),
                   (uint8_t) lroundf(160.0f * ndf));   // brighter head pixel = "here"
  for (int32_t i = 0; i < n; i++)
    addr->get(i).set(i < filled ? (i == filled - 1 ? head : fill) : Color::BLACK);
  addr->schedule_show();
  this->led_->loop();               // hand-flush: main loop is blocked during OTA
  this->led_force_repaint_ = true;  // post-OTA(-abort): (d) repaints the state colour
}

// LED OWNER: loop()-only (called every loop() from loop()). The uniform (d) path goes
// through make_call(); the animated paths (a/b/c) take the AddressableLight buffer
// directly (guarded by effect_active, see below) — never racing the LightState's own
// loop because both run in the SAME main loop. Priority each frame:
//   (a) volume overlay active  -> per-frame REAL LEVEL BAR (addressable) + return
//   (b) network not up yet     -> per-frame boot COMET spinner (addressable) + return
//   (c) connect-confirm window -> per-frame green FILL-SWEEP (addressable) + return
//   (d) else                   -> the existing uniform turn-state colour (EDGE-gated)
// (a/b/c) share ONE engagement latch (led_addr_active_): the first addressable frame
// pushes a one-shot make_call(ON + brightness 1.0 + transition 0) and set_effect_active(
// true) so update_state()/the transformer stop overwriting our direct per-LED writes;
// the brightness is held at full so the lit pixels are independent of the user dim slider.
// When we leave (a/b/c) and fall through to (d) we disengage once (set_effect_active(
// false)); the edge gate (led_last_) would otherwise stay stuck on a stale value, so
// led_force_repaint_ forces exactly one uniform repaint that overwrites every pixel.
void HoshiWsAudio::drive_led_() {
  if (this->led_ == nullptr)
    return;

  uint32_t now = millis();
  // Gate the boot animation on the NETWORK (WiFi+IP), NOT the Hoshi ws: the ws is
  // opened lazily by the tx task on the FIRST turn (first wake word), so connected_
  // stays false at idle — tying the LED to it would breathe forever until you talk to
  // it. network::is_connected() flips a few seconds after boot = what "ready" means
  // here. (A persistent WiFi failure => perpetual breathing = a useful "no WiFi" hint.)
  bool net_up = network::is_connected();

  // Network edge detect (false->true): open a ~1 s green "online" confirm window.
  // Runs regardless of which branch renders so a WiFi reconnect also re-confirms.
  if (net_up && !this->was_connected_)
    this->connect_confirm_until_ms_ = now + 1000;
  this->was_connected_ = net_up;

  // ---- Night mode (LED package 2): global dim factor, server-pushed ----
  // nd  = raw factor for pure INFO glows (timer arc, mute pixel) — may reach 0 = off.
  // ndf = floored factor for INTERACTION feedback (everything the user just caused:
  //       wake/listening/VU/ack/volume/boot) — stays visible even at dim=0.
  float nd = this->night_active_.load() ? this->night_dim_.load() : 1.0f;
  if (nd < 0.0f) nd = 0.0f;
  if (nd > 1.0f) nd = 1.0f;
  float ndf = nd < NIGHT_FLOOR ? NIGHT_FLOOR : nd;

  // ---- (a) Volume overlay (highest priority) — per-frame REAL LEVEL BAR ----
  // We light N of the 12 LEDs proportional to the level (the ANZAHL is the signal).
  // To do this we write the strip's pixel buffer DIRECTLY (ESPColorView) instead of a
  // uniform make_call(). LightState coexistence: the esp32_rmt_led_strip output is an
  // AddressableLight; while an addressable EFFECT is active its update_state() and the
  // AddressableLightTransformer both bail out instead of overwriting the buffer (see
  // light/addressable_light.cpp). We borrow that exact flag (set_effect_active(true)) so
  // our direct writes survive. On entering the window we ALSO push one make_call(state=
  // true, brightness=1.0, transition 0): the light must be ON (effects require it) and a
  // fixed full brightness keeps the bar's lit-LED level independent of the user dim slider
  // (update_state still runs set_local_brightness from current_values before the effect
  // bail-out). Engage/disengage is shared with (b)/(c) via led_addr_active_ + the disengage
  // block below; on leaving we force (d) to repaint, overwriting every pixel — nothing hangs.
  if (now < this->volume_display_until_ms_) {
    auto *addr = static_cast<light::AddressableLight *>(this->led_->get_output());
    if (addr != nullptr && addr->size() > 0) {
      this->led_addr_engage_(addr);  // shared one-shot engage (ON + full bright + buffer)
      // Normalise the clamped [0.30, 0.85] window to 0..1, then to a LED count.
      float vol = this->speaker_volume_.load();
      if (vol < 0.30f) vol = 0.30f;
      if (vol > 0.85f) vol = 0.85f;
      float t = (vol - 0.30f) / 0.55f;  // 0..1
      int32_t n_leds = addr->size();
      int32_t lit = (int32_t) lroundf(t * (float) n_leds);
      if (lit < 1) lit = 1;             // always show at least one LED
      if (lit > n_leds) lit = n_leds;   // never exceed the strip
      // Level-dependent colour for the lit run: blue -> green -> amber (same feel as the
      // old uniform overlay) at a fixed, readable brightness (the bar LENGTH is the
      // signal; the hue is a secondary cue). ESPColorView::set() applies gamma + the
      // strip's colour correction for us.
      float r, g, b;
      if (t < 0.5f) {                   // blue -> green
        float u = t / 0.5f;
        r = 0.0f; g = u; b = 1.0f - u;
      } else {                          // green -> amber
        float u = (t - 0.5f) / 0.5f;
        r = u; g = 1.0f - 0.4f * u; b = 0.0f;
      }
      uint8_t cr = (uint8_t) lroundf(r * ndf * 255.0f);
      uint8_t cg = (uint8_t) lroundf(g * ndf * 255.0f);
      uint8_t cb = (uint8_t) lroundf(b * ndf * 255.0f);
      Color on = Color(cr, cg, cb);
      for (int32_t i = 0; i < n_leds; i++)
        addr->get(i).set(i < lit ? on : Color::BLACK);
      addr->schedule_show();
    } else {
      // No addressable buffer available (shouldn't happen for esp32_rmt_led_strip):
      // fall back to a safe uniform render so the dial still gives feedback.
      float vol = this->speaker_volume_.load();
      if (vol < 0.30f) vol = 0.30f;
      if (vol > 0.85f) vol = 0.85f;
      float t = (vol - 0.30f) / 0.55f;
      auto call = this->led_->make_call();
      call.set_transition_length(0);
      call.set_state(true);
      call.set_brightness((0.20f + 0.70f * t) * ndf);
      call.set_rgb(0.0f, t, 1.0f - t);
      call.perform();
    }
    this->led_force_repaint_ = true;  // force (d) to repaint once the overlay ends
    return;
  }

  // ---- (b) Network not up yet: boot COMET spinner (booting / joining WiFi) — per-frame ----
  // A bright cyan head LED rotates around the 12-ring with a fading tail behind it (the
  // 2-3 pixels trailing the head get progressively dimmer), everything else off. Reads as
  // "I'm starting / reaching for WiFi". Direct per-pixel via the shared addressable engage.
  if (!net_up) {
    auto *addr = static_cast<light::AddressableLight *>(this->led_->get_output());
    if (addr != nullptr && addr->size() > 0) {
      this->led_addr_engage_(addr);
      int32_t n = addr->size();
      int32_t head = (int32_t) ((now / 90) % (uint32_t) n);  // ~90 ms/LED => full lap ~1.1 s
      // Head + a 3-pixel fading tail (head brightest, each trailing pixel dimmer).
      const int32_t TAIL = 3;
      static const float tail_scale[1 + TAIL] = {1.0f, 0.45f, 0.18f, 0.06f};
      for (int32_t i = 0; i < n; i++)
        addr->get(i).set(Color::BLACK);
      // Cyan/blue head (lean cyan: green + full blue). Walk head back through the tail.
      for (int32_t k = 0; k <= TAIL; k++) {
        int32_t idx = ((head - k) % n + n) % n;  // wrap negative
        float s = tail_scale[k] * ndf;  // night dim (floored: boot feedback stays visible)
        uint8_t g = (uint8_t) lroundf(0.55f * s * 255.0f);
        uint8_t b = (uint8_t) lroundf(1.0f * s * 255.0f);
        addr->get(idx).set(Color(0, g, b));
      }
      addr->schedule_show();
    } else {
      // No addressable buffer: fall back to the old uniform breathing-blue so boot still
      // gives feedback (triangle wave over ~1500 ms -> brightness 0.1 .. 0.6).
      const uint32_t period = 1500;
      uint32_t phase = now % period;
      float tri = (phase < period / 2) ? ((float) phase / (float) (period / 2))
                                       : (1.0f - ((float) (phase - period / 2) / (float) (period / 2)));
      auto call = this->led_->make_call();
      call.set_transition_length(0);
      call.set_state(true);
      call.set_brightness((0.1f + 0.5f * tri) * ndf);
      call.set_rgb(0.0f, 0.2f, 1.0f);  // blue
      call.perform();
    }
    this->led_force_repaint_ = true;  // force (d) to repaint once we connect
    return;
  }

  // ---- (c) Connect-confirm window: green FILL-SWEEP "online" — per-frame ----
  // Over the ~1 s window the LEDs fill in one after another around the ring (progress =
  // how far through the window) and then (c) ends -> (d) idle. A satisfying "ready" moment.
  if (now < this->connect_confirm_until_ms_) {
    auto *addr = static_cast<light::AddressableLight *>(this->led_->get_output());
    if (addr != nullptr && addr->size() > 0) {
      this->led_addr_engage_(addr);
      int32_t n = addr->size();
      // The window was opened as now + 1000 (see edge detect above). Reconstruct progress
      // 0..1 from the remaining time; clamp defensively against the deadline edge.
      uint32_t remain = (this->connect_confirm_until_ms_ > now)
                            ? (this->connect_confirm_until_ms_ - now) : 0;
      if (remain > 1000) remain = 1000;
      float p = 1.0f - ((float) remain / 1000.0f);  // 0 at window start -> 1 at window end
      int32_t filled = (int32_t) lroundf(p * (float) n);
      if (filled < 1) filled = 1;       // show the first pixel immediately
      if (filled > n) filled = n;
      Color green = Color(0, (uint8_t) lroundf(255.0f * ndf), (uint8_t) lroundf(51.0f * ndf));
      for (int32_t i = 0; i < n; i++)
        addr->get(i).set(i < filled ? green : Color::BLACK);
      addr->schedule_show();
    } else {
      // No addressable buffer: fall back to the old uniform solid-green confirm.
      auto call = this->led_->make_call();
      call.set_transition_length(0);
      call.set_state(true);
      call.set_brightness(0.7f * ndf);
      call.set_rgb(0.0f, 1.0f, 0.2f);  // green
      call.perform();
    }
    this->led_force_repaint_ = true;  // force (d) to repaint once confirm ends
    return;
  }

  // ================= LED package 1 (2026-07-08, maintainer sign-off) =================
  // New animated branches (c2)-(c7). Same contract as (a)-(c): render per frame,
  // set led_force_repaint_, return; on nullptr addr FALL THROUGH so behaviour
  // degrades to the uniform (d) colours. One state load for all branches below.
  TurnState st = this->turn_state_.load();

  // ---- (c2) Stop-Quittung: red sweep after abort_turn() — "heard you, cancelled" ----
  if (now < this->stop_ack_until_ms_) {
    auto *addr = this->led_addr_(true);
    if (addr != nullptr) {
      int32_t n = addr->size();
      uint32_t remain = this->stop_ack_until_ms_ - now;  // (0, 450]
      if (remain > 450) remain = 450;
      float p = 1.0f - ((float) remain / 450.0f);  // 0 -> 1 over the window
      int32_t filled = (int32_t) lroundf(p * (float) n);
      if (filled < 1) filled = 1;
      if (filled > n) filled = n;
      const Color red((uint8_t) lroundf(200.0f * ndf), (uint8_t) lroundf(24.0f * ndf),
                      (uint8_t) lroundf(16.0f * ndf));
      for (int32_t i = 0; i < n; i++)
        addr->get(i).set(i < filled ? red : Color::BLACK);
      addr->schedule_show();
      this->led_force_repaint_ = true;
      return;
    }
  }

  // ---- (c3) Wake-Spark: white spark blooms to listening-cyan within ~240 ms ----
  if (st == TurnState::LISTENING && now < this->wake_spark_until_ms_) {
    auto *addr = this->led_addr_(true);
    if (addr != nullptr) {
      int32_t n = addr->size();
      uint32_t remain = this->wake_spark_until_ms_ - now;  // (0, 240]
      if (remain > 240) remain = 240;
      float p = 1.0f - ((float) remain / 240.0f);  // 0 -> 1
      // Symmetric bloom from pixel 0: arms grow to half the ring each side.
      int32_t arms = 1 + (int32_t) lroundf(p * (float) (n / 2));
      // Colour lerp white(220,220,220) -> cyan(0,204,255) with progress, night-dimmed.
      uint8_t r = (uint8_t) lroundf(220.0f * (1.0f - p) * ndf);
      uint8_t g = (uint8_t) lroundf((220.0f + (204.0f - 220.0f) * p) * ndf);
      uint8_t b = (uint8_t) lroundf((220.0f + (255.0f - 220.0f) * p) * ndf);
      const Color c(r, g, b);
      for (int32_t i = 0; i < n; i++)
        addr->get(i).set(Color::BLACK);
      for (int32_t k = 0; k < arms && k < n; k++) {
        addr->get(k).set(c);                     // clockwise arm
        addr->get(((n - k) % n)).set(c);         // counter-clockwise arm (k=0 -> pixel 0)
      }
      addr->schedule_show();
      this->led_force_repaint_ = true;
      return;
    }
  }

  // ---- (c4) Speaker-accent shimmer: short accent glow when the server names a speaker ----
  // (ws task sets accent+window atomically; an optional downlink extension that maps a
  // recognized speaker to an LED accent color.)
  {
    uint32_t flash_until = this->speaker_flash_until_ms_.load();
    uint32_t accent = this->speaker_accent_rgb_.load();
    if (now < flash_until && (accent & 0x01000000u) != 0) {
      auto *addr = this->led_addr_(true);
      if (addr != nullptr) {
        int32_t n = addr->size();
        uint32_t remain = flash_until - now;  // (0, 700]
        if (remain > 700) remain = 700;
        float fade = (float) remain / 700.0f;          // 1 -> 0
        float s = (0.12f + 0.68f * fade) * ndf;        // never fully dark inside the window
        uint8_t r = (uint8_t) lroundf(((accent >> 16) & 0xFF) * s);
        uint8_t g = (uint8_t) lroundf(((accent >> 8) & 0xFF) * s);
        uint8_t b = (uint8_t) lroundf((accent & 0xFF) * s);
        const Color c(r, g, b);
        for (int32_t i = 0; i < n; i++)
          addr->get(i).set(c);
        addr->schedule_show();
        this->led_force_repaint_ = true;
        return;
      }
    }
  }

  // ---- (c5) SPEAKING: VU — the ring breathes with Hoshi's voice ----
  // play_env_ is fed by vu_feed_ (drain, same loop) with each chunk's peak; decay it
  // here per frame (~140 ms half-life at a 16 ms loop) and drive a uniform green glow.
  // sqrtf = perceptual: quiet speech still visibly moves the ring.
  if (st == TurnState::SPEAKING) {
    this->play_env_ *= 0.92f;
    auto *addr = this->led_addr_(true);
    if (addr != nullptr) {
      int32_t n = addr->size();
      float level = sqrtf(this->play_env_);
      if (level > 1.0f) level = 1.0f;
      float s = (0.15f + 0.75f * level) * ndf;
      const Color c((uint8_t) 0, (uint8_t) lroundf(255.0f * s), (uint8_t) lroundf(51.0f * s));
      for (int32_t i = 0; i < n; i++)
        addr->get(i).set(c);
      addr->schedule_show();
      this->led_force_repaint_ = true;
      return;
    }
  }

  // ---- (c6) THINKING/AWAITING_STT: Denk-Swirl — a slow dim amber comet ----
  // Keeps the existing colour language (yellow = thinking) but makes the ~seconds of
  // brain latency read as "she's working", not "it hung". ~110 ms/LED, tail of 4.
  if (st == TurnState::AWAITING_STT || st == TurnState::THINKING) {
    auto *addr = this->led_addr_(true);
    if (addr != nullptr) {
      int32_t n = addr->size();
      int32_t head = (int32_t) ((now / 110) % (uint32_t) n);
      const int32_t TAIL = 4;
      static const float tail_scale[1 + TAIL] = {1.0f, 0.50f, 0.22f, 0.08f, 0.03f};
      for (int32_t i = 0; i < n; i++)
        addr->get(i).set(Color::BLACK);
      for (int32_t k = 0; k <= TAIL; k++) {
        int32_t idx = ((head - k) % n + n) % n;
        float s = 0.45f * tail_scale[k] * ndf;  // overall dim: it is a wait state, not an alert
        addr->get(idx).set(Color((uint8_t) lroundf(255.0f * s), (uint8_t) lroundf(190.0f * s),
                                 (uint8_t) lroundf(20.0f * s)));
      }
      addr->schedule_show();
      this->led_force_repaint_ = true;
      return;
    }
  }

  // ---- (c7) IDLE-Info: timer countdown arc + mute pixel (both very dim) ----
  // Timer state comes from the ws timer_state frame (atomics); remaining is
  // extrapolated locally from the rx timestamp so the arc moves between pushes.
  // Deliberately NO idle decoration beyond these two (maintainer call: nothing that disturbs sleep).
  if (st == TurnState::IDLE) {
    bool muted = this->hardware_muted_.load();
    int32_t total = this->timer_total_s_.load();
    int32_t rem = 0;
    bool timer_active = false;
    if (total > 0) {
      uint32_t rx = this->timer_rx_ms_.load();
      int32_t elapsed_s = (int32_t) ((now - rx) / 1000u);
      rem = this->timer_remaining_rx_s_.load() - elapsed_s;
      timer_active = rem > 0;
    }
    // Nachtmodus: info glows use the RAW factor (no floor) and are treated as OFF
    // below NIGHT_INFO_OFF — a sleeping room stays truly dark at dim~0.
    bool info_visible = nd > NIGHT_INFO_OFF;
    if ((muted || timer_active) && info_visible) {
      auto *addr = this->led_addr_(true);
      if (addr != nullptr) {
        int32_t n = addr->size();
        for (int32_t i = 0; i < n; i++)
          addr->get(i).set(Color::BLACK);
        if (timer_active) {
          int32_t lit = (int32_t) ceilf(((float) rem / (float) total) * (float) n);
          if (lit < 1) lit = 1;
          if (lit > n) lit = n;
          // Last minute: gentle triangle pulse 0.15..0.45; otherwise steady dim 0.30.
          float s = 0.30f;
          if (rem <= 60) {
            uint32_t phase = now % 1200u;
            float tri = (phase < 600u) ? ((float) phase / 600.0f)
                                       : (1.0f - ((float) (phase - 600u) / 600.0f));
            s = 0.15f + 0.30f * tri;
          }
          s *= nd;  // night: raw factor, arc may get arbitrarily faint
          const Color amber((uint8_t) lroundf(255.0f * s), (uint8_t) lroundf(190.0f * s),
                            (uint8_t) lroundf(20.0f * s));
          for (int32_t i = 0; i < lit; i++)
            addr->get(i).set(amber);
        }
        if (muted)
          addr->get(0).set(Color((uint8_t) lroundf(90.0f * nd), 0, 0));  // dim red "mic off" pixel
        addr->schedule_show();
        this->led_force_repaint_ = true;
        return;
      }
    }
  }
  // =============== Ende LED-Paket 1 Branches ===============

  // We are NOT in any addressable window (a/b/c, c2-c7) any more. If the addressable
  // buffer was engaged by ANY of them, release it back to the LightState (clear
  // effect_active) so the uniform (d) repaint below actually reaches the hardware via
  // update_state(). The branch we just left already set led_force_repaint_, so (d)
  // repaints exactly once and overwrites every animation pixel — nothing left hanging.
  if (this->led_addr_active_) {
    auto *addr = static_cast<light::AddressableLight *>(this->led_->get_output());
    if (addr != nullptr)
      addr->set_effect_active(false);
    this->led_addr_active_ = false;
  }

  // ---- (d) Normal turn-state colour — EDGE-gated for efficiency ----
  // (st was loaded once above, before the (c2)-(c7) branches.) The night factor is
  // part of the edge: a night_mode flip mid-state must also repaint exactly once.
  if (st == this->led_last_ && !this->led_force_repaint_ && ndf == this->led_night_last_)
    return;
  this->led_last_ = st;
  this->led_night_last_ = ndf;
  this->led_force_repaint_ = false;
  auto call = this->led_->make_call();
  call.set_transition_length(80);
  switch (st) {
    case TurnState::LISTENING:    // cyan
      call.set_state(true); call.set_brightness(0.6f * ndf); call.set_rgb(0.0f, 0.8f, 1.0f); break;
    case TurnState::AWAITING_STT:
    case TurnState::THINKING:     // yellow
      call.set_state(true); call.set_brightness(0.6f * ndf); call.set_rgb(1.0f, 0.8f, 0.0f); break;
    case TurnState::SPEAKING:     // green
      call.set_state(true); call.set_brightness(0.6f * ndf); call.set_rgb(0.0f, 1.0f, 0.2f); break;
    case TurnState::ERROR:        // red
      call.set_state(true); call.set_brightness(0.7f * ndf); call.set_rgb(1.0f, 0.0f, 0.0f); break;
    case TurnState::IDLE:
    default:
      call.set_state(false); break;
  }
  call.perform();
}

void HoshiWsAudio::enter_speaking_() {
  // Called from the ws dispatch (tts_audio_start / first llm_audio). Funnel the
  // heavy work (mww.stop, speaker.start, set_audio_stream_info) here; these are
  // safe to invoke from the ws task as they don't block on our send path.
  if (this->turn_state_.load() == TurnState::SPEAKING)
    return;
#ifdef USE_MICRO_WAKE_WORD
  // Red-team fix #7: stop the wake detector during SPEAKING (no AEC -> the device
  // would self-trigger on its own TTS). Restarted in to_idle_().
  if (this->mww_ != nullptr && this->mww_running_) {
    this->mww_->stop();
    this->mww_running_ = false;
  }
#endif
  if (this->speaker_ != nullptr) {
    // Re-assert the HARD INVARIANT before (the one-time) start(): 16-bit/1ch/return_rate.
    this->speaker_->set_audio_stream_info(
        audio::AudioStreamInfo(16, 1, this->return_sample_rate_));
    // START-ONCE (fix #1): only start the chain the first time. After that it stays
    // RUNNING and self-fills silence between turns, so the i2s bus channel is never
    // released + re-acquired per turn (no "Parent bus is busy" race / 1 s retry).
    if (!this->speaker_running_) {
      this->speaker_->start();
      this->speaker_running_ = true;
    }
    // Re-assert DAC volume + unmute (fix #2). The aic3204 powers up SILENT: its setup()
    // defers write_volume_() by 2.5 s with volume_=0 -> DAC digital vol -127, clobbering
    // the i2s speaker's boot-time set_volume(1.0). With no media_player to push a volume,
    // nothing ever re-asserts it. We do it here, on the first SPEAKING (always > 2.5 s
    // after boot), so the deferred write can't undo us. set_volume(>0) forwards through
    // resampler -> i2s speaker -> aic3204->set_mute_off() + set_volume() (i2s_audio_speaker
    // .cpp set_volume()). Done once is enough; the value is sticky in the DAC registers.
    if (!this->speaker_volume_set_) {
      this->speaker_->set_volume(this->speaker_volume_);
      this->speaker_volume_set_ = true;
      ESP_LOGI(TAG, "speaker volume asserted -> %.2f (aic3204 unmute + DAC vol)",
               this->speaker_volume_.load());
    }
  }
  this->turn_state_.store(TurnState::SPEAKING);
  this->state_entered_ms_ = millis();
  ESP_LOGD(TAG, "enter SPEAKING (mic gated, mww stopped, speaker running)");
}

void HoshiWsAudio::to_idle_() {
  this->turn_state_.store(TurnState::IDLE);
  this->state_entered_ms_ = millis();
  this->eos_requested_.store(false);
  this->abort_requested_.store(false);
  this->llm_done_seen_.store(false);
  this->play_grace_deadline_ms_.store(0);
  // NOTE: do NOT stop the speaker here (start-once, fix #1) — it stays RUNNING and
  // self-fills silence. We only clear our play queue so no stale PCM leaks into the
  // next turn; drain_play_to_speaker_ stops feeding because turn_state_ != SPEAKING.
  this->play_pending_.clear();
  this->play_pending_pos_ = 0;
  if (this->play_rb_ != nullptr)
    this->play_rb_->reset();
  if (this->uplink_rb_ != nullptr)
    this->uplink_rb_->reset();
#ifdef USE_MICRO_WAKE_WORD
  // Restart the wake detector (fix #7).
  if (this->mww_ != nullptr && !this->mww_running_) {
    this->mww_->start();
    this->mww_running_ = true;
  }
#endif
  ESP_LOGD(TAG, "turn -> IDLE");
}

void HoshiWsAudio::to_error_(const char *why) {
  ESP_LOGW(TAG, "turn -> ERROR (%s)", why);
  // Start-once (fix #1): keep the speaker RUNNING (self-fills silence); just drop our
  // queued/carried PCM. Leaving SPEAKING state stops drain_play_to_speaker_ feeding it.
  if (this->play_rb_ != nullptr)
    this->play_rb_->reset();
  this->play_pending_.clear();
  this->play_pending_pos_ = 0;
  // Tell the tx task to drop any in-flight turn work.
  this->abort_requested_.store(true);
  this->turn_state_.store(TurnState::ERROR);
  this->state_entered_ms_ = millis();
}

// =============================================================================
// (1) mic callback context — passive MicrophoneSource gives PCM16 mono ready.
// =============================================================================
void HoshiWsAudio::on_pcm16_(const std::vector<uint8_t> &data) {
  // --- Ambient floor tracking (endpointing vs. background TV, field finding 2026-07-17) ---
  // Outside LISTENING the mic KEEPS streaming (the passive MicrophoneSource rides on
  // micro_wake_word, the always-on consumer; only SPEAKING stops mww, fix #7). Use
  // those idle blocks to track the ROOM's noise floor (TV, dishwasher, …) as a slow
  // EMA (~1.5 s settle at ~30 ms blocks). vad_feed_ then judges "silence" RELATIVE to
  // this floor, so background speech no longer holds the recording window open.
  // Subsampled ×4: idle CPU stays negligible. Mic-cb context only (plain member).
  {
    TurnState amb_st = this->turn_state_.load();
    if (amb_st != TurnState::LISTENING && !this->hardware_muted_.load() && data.size() >= 8) {
      const int16_t *amb_s = reinterpret_cast<const int16_t *>(data.data());
      double amb_rms = HoshiWsAudio::block_rms_(amb_s, data.size() / 2, 4);
      this->vad_ambient_rms_ += 0.03f * ((float) amb_rms - this->vad_ambient_rms_);
    }
  }

  // Gate: only capture while LISTENING and not muted. (The MicrophoneSource already
  // zeroes data when the mic component is muted; we add the turn-state + hw gate.)
  if (this->turn_state_.load() != TurnState::LISTENING)
    return;
  if (this->hardware_muted_.load())
    return;
  if (this->eos_requested_.load())
    return;  // already endpointed; stop accumulating
  if (data.empty() || this->uplink_rb_ == nullptr)
    return;

  // The source delivers PCM16 mono (1 channel) — bytes are int16 little-endian.
  size_t n_samples = data.size() / 2;
  if (n_samples == 0)
    return;
  const int16_t *samples = reinterpret_cast<const int16_t *>(data.data());

  // Single-producer write into the uplink ring. write_without_replacement avoids
  // clobbering not-yet-sent audio mid-utterance (red-team fix #3): if the tx task
  // falls behind, we drop the NEWEST block (and log) rather than corrupt the stream.
  size_t wrote = this->uplink_rb_->write_without_replacement(data.data(), data.size(), 0);
  if (wrote < data.size()) {
    ESP_LOGW(TAG, "uplink ring full: dropped %u/%u bytes (raise uplink_rb_bytes)",
             (unsigned) (data.size() - wrote), (unsigned) data.size());
  }

  // RMS VAD (fix #6): compute RMS for this block, log high-water for calibration.
  if (this->vad_feed_(samples, n_samples)) {
    ESP_LOGI(TAG, "VAD: %ums trailing silence -> end-of-speech -> eos", this->silence_ms_);
    this->eos_requested_.store(true);
#ifdef USE_ESP32
    if (this->tx_task_ != nullptr)
      xTaskNotifyGive(this->tx_task_);
#endif
  }
}

// =============================================================================
// (3) tx task context — the ONLY caller of esp_websocket_client send.
// =============================================================================
void HoshiWsAudio::tx_task_trampoline_(void *arg) {
  static_cast<HoshiWsAudio *>(arg)->tx_task_run_();
}

void HoshiWsAudio::tx_task_run_() {
#ifdef USE_ESP32
  // Single fixed accumulation buffer for the per-turn WAV (44-byte header + PCM16).
  // Allocated ONCE from PSRAM (external first, fall back internal), NEVER realloc'd.
  // Sized for the absolute max turn so the inner drain can hard-cap without OOM:
  //   WAV_CAP = 44 + max_turn_ms_/1000 * uplink_sample_rate_ * 2  (PCM16 mono).
  // Defaults: 44 + 10*16000*2 = 320044 bytes (~313 KB).
  const size_t WAV_CAP =
      44 + (size_t) ((uint64_t) this->max_turn_ms_ / 1000ULL * this->uplink_sample_rate_ * 2ULL);
  esphome::RAMAllocator<uint8_t> alloc(esphome::RAMAllocator<uint8_t>::ALLOC_EXTERNAL |
                                       esphome::RAMAllocator<uint8_t>::ALLOC_INTERNAL);  // PSRAM first
  uint8_t *wav_buf = alloc.allocate(WAV_CAP);
  if (wav_buf == nullptr) {
    ESP_LOGE(TAG, "tx: failed to allocate %u B WAV buffer (PSRAM+internal exhausted)",
             (unsigned) WAV_CAP);
    // No buffer => we cannot run uplink. Fail the component cleanly rather than crash.
    this->turn_state_.store(TurnState::ERROR);
    this->state_entered_ms_ = millis();
    vTaskDelete(nullptr);
    return;
  }
  ESP_LOGI(TAG, "tx: WAV buffer allocated once (%u B, PSRAM-first) — bounded, no realloc",
           (unsigned) WAV_CAP);

  size_t wav_len = 44;               // PCM accumulates from offset 44; header is filled at eos.
  bool turn_active = false;          // tx-local: have we sent {start} for this turn?
  uint32_t my_turn_id = 0;

  for (;;) {
    if (this->tx_should_exit_.load())
      break;
    // Wait for a notify (start/eos/abort) or poll periodically while a turn runs.
    ulTaskNotifyTake(pdTRUE, turn_active ? pdMS_TO_TICKS(10) : TX_NOTIFY_WAIT);

    // --- abort handling (highest priority) ---
    if (this->abort_requested_.exchange(false)) {
      if (turn_active) {
        this->send_abort_frame_(my_turn_id);
        turn_active = false;
        wav_len = 44;  // reset accumulator (buffer kept, never freed/realloc'd)
      }
      continue;
    }

    // --- start a new turn ---
    if (this->start_requested_.exchange(false)) {
      my_turn_id = this->turn_id_.load();
      // Lazy connect OFF-LOOP (this is the whole point of the redesign).
      if (!this->connected_.load()) {
        if (!this->connect_()) {
          ESP_LOGW(TAG, "tx: connect failed -> aborting turn %u", (unsigned) my_turn_id);
          // Signal the loop to show the error LED + return to idle.
          this->turn_state_.store(TurnState::ERROR);
          this->state_entered_ms_ = millis();
          continue;
        }
        // Wait (off-loop) up to ~3s for the async CONNECTED event.
        uint32_t waited = 0;
        while (!this->connected_.load() && waited < 3000 &&
               !this->abort_requested_.load()) {
          vTaskDelay(pdMS_TO_TICKS(20));
          waited += 20;
        }
        if (!this->connected_.load()) {
          ESP_LOGW(TAG, "tx: not CONNECTED after %ums -> error (TLS/handshake?)", waited);
          this->turn_state_.store(TurnState::ERROR);
          this->state_entered_ms_ = millis();
          continue;
        }
      }
      this->send_start_frame_(my_turn_id);
      ESP_LOGI(TAG, "tx -> start(mimeType=audio/wav) turn=%u", (unsigned) my_turn_id);
      turn_active = true;
      wav_len = 44;  // reset accumulator to just-past-header for the new turn
      this->uplink_rms_peak_ = 0;
    }

    if (!turn_active)
      continue;

    // --- drain the uplink ring continuously during LISTENING (fix #3) ---
    if (this->turn_state_.load() == TurnState::LISTENING)
      this->tx_drain_uplink_(wav_buf, &wav_len, WAV_CAP);

    // --- end-of-speech: flush remaining ring, build + send WAV, then {stop} ---
    if (this->eos_requested_.load() && this->turn_state_.load() == TurnState::LISTENING) {
      this->tx_drain_uplink_(wav_buf, &wav_len, WAV_CAP);  // pull any last bytes
      this->tx_send_wav_(wav_buf, wav_len, my_turn_id);
      this->send_stop_frame_();
      // Hand the turn to AWAITING_STT (loop owns the rest of the lifecycle).
      this->turn_state_.store(TurnState::AWAITING_STT);
      this->state_entered_ms_ = millis();
      ESP_LOGI(TAG, "tx -> WAV(%u B) + stop, turn=%u; awaiting transcript",
               (unsigned) wav_len, (unsigned) my_turn_id);
      turn_active = false;
      wav_len = 44;  // reset accumulator (buffer retained for the next turn)
    }
  }
  this->disconnect_();
  alloc.deallocate(wav_buf, WAV_CAP);
  vTaskDelete(nullptr);
#endif
}

// Pull as much as is available from the uplink ring into the FIXED PSRAM buffer at
// buf+*len, clamped to cap. NEVER reallocates. When the buffer is full (no room
// left for PCM) we force eos so the turn ends even if the VAD never fires — this
// is the absolute, OOM-proof cap (replaces the old ineffective inner-loop break).
void HoshiWsAudio::tx_drain_uplink_(uint8_t *buf, size_t *len, size_t cap) {
  if (this->uplink_rb_ == nullptr || buf == nullptr || len == nullptr)
    return;
  uint8_t tmp[TX_PULL_CHUNK];
  for (;;) {
    size_t room = (cap > *len) ? (cap - *len) : 0;
    if (room == 0) {
      // Hard cap reached: bound absolutely + end the turn (idempotent).
      if (!this->eos_requested_.exchange(true)) {
        ESP_LOGW(TAG, "uplink cap reached (%u B) -> forcing eos", (unsigned) cap);
#ifdef USE_ESP32
        if (this->tx_task_ != nullptr)
          xTaskNotifyGive(this->tx_task_);
#endif
      }
      return;
    }
    size_t avail = this->uplink_rb_->available();
    if (avail == 0)
      break;
    size_t want = avail < sizeof(tmp) ? avail : sizeof(tmp);
    if (want > room)
      want = room;  // clamp the read so we never exceed the buffer
    size_t got = this->uplink_rb_->read(tmp, want, 0);
    if (got == 0)
      break;
    std::memcpy(buf + *len, tmp, got);
    *len += got;
  }
}

// Write the canonical 44-byte WAV header into buf[0..44] (PCM already sits at
// buf[44..wav_len]) and send buf[0..wav_len] directly in <=16KB binary frames.
// ONE buffer, zero extra large allocation.
void HoshiWsAudio::tx_send_wav_(uint8_t *buf, size_t wav_len, uint32_t turn_id) {
  if (buf == nullptr || wav_len < 44)
    return;
  size_t pcm_len = wav_len - 44;
  if (pcm_len == 0)
    ESP_LOGW(TAG, "no mic audio for turn %u -> empty WAV", (unsigned) turn_id);

  // Build the 44-byte header via the existing builder, then copy it into buf[0..44].
  // (wav_header_ appends to a std::vector; this is a tiny, fixed 44-byte alloc — not
  // the large-buffer OOM path.)
  std::vector<uint8_t> hdr;
  hdr.reserve(44);
  HoshiWsAudio::wav_header_(hdr, this->uplink_sample_rate_, (uint32_t) pcm_len);
  std::memcpy(buf, hdr.data(), hdr.size() < 44 ? hdr.size() : 44);

  size_t n_chunks = 0;
  for (size_t off = 0; off < wav_len; off += UPLINK_CHUNK) {
    size_t len = (wav_len - off < UPLINK_CHUNK) ? (wav_len - off) : UPLINK_CHUNK;
    if (!this->send_binary_(buf + off, len)) {
      ESP_LOGW(TAG, "tx: send_bin failed at chunk %u", (unsigned) n_chunks);
      break;
    }
    n_chunks++;
  }
  uint32_t audio_ms = (uint32_t) ((pcm_len / 2) * 1000ULL / this->uplink_sample_rate_);
  ESP_LOGI(TAG, "tx -> WAV %u B in %u chunk(s) (%ums audio, rms_peak=%u) turn=%u",
           (unsigned) wav_len, (unsigned) n_chunks, audio_ms,
           this->uplink_rms_peak_, (unsigned) turn_id);
}

bool HoshiWsAudio::connect_() {
#ifdef HOSHI_HAVE_WS_CLIENT
  if (this->ws_ != nullptr && this->connected_.load())
    return true;
  if (this->ws_ != nullptr) {
    // Stale handle from a prior failed attempt: tear it down first.
    esp_websocket_client_stop((esp_websocket_client_handle_t) this->ws_);
    esp_websocket_client_destroy((esp_websocket_client_handle_t) this->ws_);
    this->ws_ = nullptr;
  }

  esp_websocket_client_config_t cfg = {};
  cfg.host = this->host_.c_str();
  cfg.port = this->port_;
  cfg.transport = WEBSOCKET_TRANSPORT_OVER_SSL;   // wss only (plain ws VETOED)
  cfg.cert_pem = this->effective_cacert_pem_();    // leaf-pin (contract §A)
  // IP-SAN sharp edge (fix #4): a leaf whose SAN is an IP address, which some esp-tls
  // builds won't validate against the host string. Allow skipping the CN/SAN check
  // (we still pin the exact leaf bytes via cert_pem, so this is not a downgrade).
  cfg.skip_cert_common_name_check = this->skip_cn_check_;
  cfg.buffer_size = 4096;
  // ws task runs ws_event_ -> on_ws_text_ -> handle_llm_audio_, whose decode scratch
  // (out[2048]+hdr[256]) + the esp_websocket_client call chain overflow the DEFAULT
  // ~4 KB task stack on the first llm_audio frame (CONFIRMED on-device: FreeRTOS
  // vApplicationStackOverflowHook in handle_llm_audio_). Give it generous headroom.
  cfg.task_stack = 12288;
  cfg.disable_auto_reconnect = false;
  cfg.network_timeout_ms = 10000;
  cfg.reconnect_timeout_ms = 5000;
  // Pin the IDF ws task to core 0 (our tx task is on core 1, the ESPHome loop is
  // on core 1 / APP). This keeps recv/TLS off our send + main contexts.
  cfg.task_core_id_set = true;
  cfg.task_core_id = 0;

  std::string path = this->path_;
  std::string hdrs;
  if (this->auth_mode_ == AuthMode::BEARER) {
    if (!this->auth_token_.empty()) {
      hdrs = "Authorization: Bearer " + this->auth_token_ + "\r\n";
      cfg.headers = hdrs.c_str();
    }
  } else {  // QUERY (?token=)
    if (!this->auth_token_.empty())
      path += (path.find('?') == std::string::npos ? "?token=" : "&token=") + this->auth_token_;
  }
  cfg.path = path.c_str();

  this->ws_ = esp_websocket_client_init(&cfg);
  if (this->ws_ == nullptr) {
    ESP_LOGE(TAG, "esp_websocket_client_init failed");
    return false;
  }
  esp_websocket_register_events((esp_websocket_client_handle_t) this->ws_, WEBSOCKET_EVENT_ANY,
                                &HoshiWsAudio::ws_event_, this);
  esp_err_t err = esp_websocket_client_start((esp_websocket_client_handle_t) this->ws_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_websocket_client_start failed: %d", (int) err);
    return false;
  }
  ESP_LOGI(TAG, "wss connect started -> %s:%u%s", this->host_.c_str(), this->port_, this->path_.c_str());
  return true;
#else
  ESP_LOGE(TAG, "connect_(): esp_websocket_client header not resolved");
  return false;
#endif
}

void HoshiWsAudio::disconnect_() {
#ifdef HOSHI_HAVE_WS_CLIENT
  if (this->ws_ != nullptr) {
    esp_websocket_client_stop((esp_websocket_client_handle_t) this->ws_);
    esp_websocket_client_destroy((esp_websocket_client_handle_t) this->ws_);
    this->ws_ = nullptr;
  }
#endif
  this->connected_.store(false);
}

bool HoshiWsAudio::send_text_(const std::string &json) {
#ifdef HOSHI_HAVE_WS_CLIENT
  if (this->ws_ == nullptr || !this->connected_.load())
    return false;
  int sent = esp_websocket_client_send_text((esp_websocket_client_handle_t) this->ws_,
                                            json.c_str(), (int) json.size(), WS_SEND_TIMEOUT);
  if (sent < 0) {
    ESP_LOGW(TAG, "send_text failed (%d): %s", sent, json.c_str());
    return false;
  }
  return true;
#else
  (void) json;
  return false;
#endif
}

bool HoshiWsAudio::send_binary_(const uint8_t *data, size_t len) {
#ifdef HOSHI_HAVE_WS_CLIENT
  if (this->ws_ == nullptr || !this->connected_.load())
    return false;
  int sent = esp_websocket_client_send_bin((esp_websocket_client_handle_t) this->ws_,
                                           reinterpret_cast<const char *>(data), (int) len, WS_SEND_TIMEOUT);
  if (sent < 0) {
    ESP_LOGW(TAG, "send_bin failed (%d, len=%u)", sent, (unsigned) len);
    return false;
  }
  return true;
#else
  (void) data; (void) len;
  return false;
#endif
}

void HoshiWsAudio::send_start_frame_(uint32_t turn_id) {
  std::string j = "{\"type\":\"start\",\"mimeType\":\"audio/wav\",\"turnId\":\"";
  char idbuf[16];
  snprintf(idbuf, sizeof(idbuf), "%u", (unsigned) turn_id);
  j += idbuf;
  j += "\"";
  if (!this->room_.empty()) { j += ",\"room\":\""; j += this->room_; j += "\""; }
  if (!this->satellite_id_.empty()) { j += ",\"satelliteId\":\""; j += this->satellite_id_; j += "\""; }
  j += "}";
  this->send_text_(j);
}

void HoshiWsAudio::send_stop_frame_() {
  this->send_text_("{\"type\":\"stop\"}");
}

void HoshiWsAudio::send_abort_frame_(uint32_t turn_id) {
  std::string j = "{\"type\":\"abort\",\"turnId\":\"";
  char idbuf[16];
  snprintf(idbuf, sizeof(idbuf), "%u", (unsigned) turn_id);
  j += idbuf;
  j += "\"}";
  this->send_text_(j);
}

void HoshiWsAudio::wav_header_(std::vector<uint8_t> &out, uint32_t sample_rate, uint32_t data_size) {
  const uint16_t channels = 1;
  const uint16_t bits = 16;
  const uint16_t block_align = channels * bits / 8;
  const uint32_t byte_rate = sample_rate * block_align;
  const uint32_t riff_size = 36 + data_size;
  auto u32 = [&out](uint32_t v) {
    out.push_back((uint8_t) (v & 0xFF));
    out.push_back((uint8_t) ((v >> 8) & 0xFF));
    out.push_back((uint8_t) ((v >> 16) & 0xFF));
    out.push_back((uint8_t) ((v >> 24) & 0xFF));
  };
  auto u16 = [&out](uint16_t v) {
    out.push_back((uint8_t) (v & 0xFF));
    out.push_back((uint8_t) ((v >> 8) & 0xFF));
  };
  auto tag = [&out](const char *s) { out.insert(out.end(), s, s + 4); };
  tag("RIFF"); u32(riff_size); tag("WAVE");
  tag("fmt "); u32(16); u16(1 /*PCM*/); u16(channels);
  u32(sample_rate); u32(byte_rate); u16(block_align); u16(bits);
  tag("data"); u32(data_size);
}

// =============================================================================
// VAD (mic callback context only).
// =============================================================================
void HoshiWsAudio::vad_reset_() {
  this->vad_speech_started_ = false;
  this->vad_voiced_ms_ = 0;
  this->vad_silence_run_ms_ = 0;
}

// Block RMS over every stride-th sample. stride 1 = exact (turn path); stride 4 =
// cheap ambient tracking at idle. Any context (pure function).
double HoshiWsAudio::block_rms_(const int16_t *samples, size_t n, size_t stride) {
  if (samples == nullptr || n == 0)
    return 0.0;
  if (stride == 0)
    stride = 1;
  double acc = 0.0;
  size_t cnt = 0;
  for (size_t i = 0; i < n; i += stride) {
    double s = (double) samples[i];
    acc += s * s;
    cnt++;
  }
  return cnt > 0 ? std::sqrt(acc / (double) cnt) : 0.0;
}

// Energy VAD with ROOM-RELATIVE endpointing (reworked 2026-07-17, field finding:
// "keeps listening too long with the TV on"). Two derived boundaries per block:
//   q ("room")  = max(0.75×thr, ambient×margin), capped at vad_silence_floor_max_
//   v ("voice") = max(thr, q×4/3)   — preserves the original 0.75-hysteresis ratio
// rms ≥ v = user speech (start / reset silence run) · rms < q = room level = silence
// counts · dead zone q..v = hold. With a quiet room (ambient→0) both collapse to the
// pre-patch absolute behaviour (q=0.75×thr, v=thr); a blaring TV is capped by
// floor_max so v can never climb into unreachable territory.
bool HoshiWsAudio::vad_feed_(const int16_t *samples, size_t n_samples) {
  if (n_samples == 0)
    return false;
  double rms = HoshiWsAudio::block_rms_(samples, n_samples, 1);
  if ((uint32_t) rms > this->uplink_rms_peak_)
    this->uplink_rms_peak_ = (uint32_t) rms;

  const double thr = (double) this->vad_rms_threshold_;
  double q = thr * 0.75;
  const double rel = (double) this->vad_ambient_rms_ * (double) this->vad_ambient_margin_;
  if (rel > q)
    q = rel;
  if (q > (double) this->vad_silence_floor_max_)
    q = (double) this->vad_silence_floor_max_;
  const double v = std::max(thr, q * (4.0 / 3.0));
  ESP_LOGV(TAG, "VAD block rms=%.0f amb=%.0f q=%.0f v=%.0f peak=%u",
           rms, this->vad_ambient_rms_, q, v, this->uplink_rms_peak_);

  uint32_t dur_ms = (uint32_t) ((n_samples * 1000ULL) / this->uplink_sample_rate_);
  if (dur_ms == 0)
    dur_ms = 1;

  if (!this->vad_speech_started_) {
    if (rms >= v) {
      this->vad_voiced_ms_ += dur_ms;
      if (this->vad_voiced_ms_ >= this->min_speech_ms_) {
        this->vad_speech_started_ = true;
        this->vad_silence_run_ms_ = 0;
        ESP_LOGD(TAG, "VAD: speech start (rms=%.0f amb=%.0f v=%.0f)",
                 rms, this->vad_ambient_rms_, v);
      }
    } else {
      this->vad_voiced_ms_ = 0;
    }
    return false;
  }

  if (rms >= v) {
    this->vad_silence_run_ms_ = 0;
  } else if (rms < q) {
    this->vad_silence_run_ms_ += dur_ms;
    if (this->vad_silence_run_ms_ >= this->silence_ms_) {
      ESP_LOGD(TAG, "VAD: end-of-speech (rms=%.0f amb=%.0f q=%.0f)",
               rms, this->vad_ambient_rms_, q);
      return true;  // end-of-speech
    }
  }
  return false;
}

// =============================================================================
// (4) ws_event_ context — IDF esp_websocket_client task: recv + dispatch. NO send.
// =============================================================================
void HoshiWsAudio::ws_event_(void *handler_args, const char *base, int32_t event_id, void *event_data) {
  (void) base;
  auto *self = static_cast<HoshiWsAudio *>(handler_args);
  if (self == nullptr)
    return;
#ifdef HOSHI_HAVE_WS_CLIENT
  auto *ev = static_cast<esp_websocket_event_data_t *>(event_data);
  switch ((esp_websocket_event_id_t) event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      self->connected_.store(true);
      ESP_LOGI(TAG, "ws CONNECTED");
      // Notify the tx task so a turn waiting on connect proceeds promptly.
#ifdef USE_ESP32
      if (self->tx_task_ != nullptr)
        xTaskNotifyGive(self->tx_task_);
#endif
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
      self->connected_.store(false);
      ESP_LOGW(TAG, "ws DISCONNECTED");
      break;
    case WEBSOCKET_EVENT_CLOSED:
      self->connected_.store(false);
      ESP_LOGW(TAG, "ws CLOSED");
      break;
    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGW(TAG, "ws ERROR (tls_err=%d ws_status=%d)",
               ev ? ev->error_handle.esp_tls_last_esp_err : 0,
               ev ? ev->error_handle.esp_ws_handshake_status_code : 0);
      break;
    case WEBSOCKET_EVENT_DATA: {
      if (ev == nullptr || ev->data_ptr == nullptr || ev->data_len <= 0)
        break;
      if (ev->op_code == 0x2) {  // binary
        self->on_ws_binary_(reinterpret_cast<const uint8_t *>(ev->data_ptr), (size_t) ev->data_len);
        break;
      }
      // text (0x1) or continuation (0x0): reassemble fragmented payloads into the
      // PSRAM-backed rx_buf_ (NOT a default/internal std::string). A single large
      // llm_audio frame can be ~200 KB of base64 — keeping that out of internal RAM
      // is half of the downlink OOM fix; the other half is the stream-decode below.
      if (ev->payload_offset == 0)
        self->rx_len_ = 0;
      // Reserve exactly what the full payload needs when known (payload_len), else
      // grow incrementally as fragments arrive.
      size_t need = (ev->payload_len > 0)
                        ? (size_t) ev->payload_len
                        : (self->rx_len_ + (size_t) ev->data_len);
      if (!self->rx_reserve_(need)) {
        ESP_LOGW(TAG, "rx reassembly alloc failed (need=%u B) -> dropping frame",
                 (unsigned) need);
        self->rx_len_ = 0;
        break;
      }
      std::memcpy(self->rx_buf_ + self->rx_len_, ev->data_ptr, (size_t) ev->data_len);
      self->rx_len_ += (size_t) ev->data_len;
      bool complete = (ev->payload_len == 0) ||
                      ((size_t) (ev->payload_offset + ev->data_len) >= (size_t) ev->payload_len);
      if (complete) {
        // Dispatch DIRECTLY over the PSRAM-backed buffer — never copy the whole frame
        // into an internal-RAM std::string. on_ws_text_ parses the small control
        // fields against this span and, for llm_audio, stream-decodes the (large)
        // base64 in place from the same PSRAM bytes. This is the decisive half of the
        // downlink OOM fix: no full-frame-size buffer ever lives in internal RAM.
        self->on_ws_text_(self->rx_buf_, self->rx_len_);
        self->rx_len_ = 0;
      }
      break;
    }
    default:
      break;
  }
#else
  (void) event_id; (void) event_data;
#endif
}

void HoshiWsAudio::on_ws_text_(const char *buf, size_t len) {
  if (buf == nullptr || len == 0)
    return;

  // --- llm_audio fast-path: parse + stream-decode the base64 WITHOUT building a
  // full-frame std::string. The frame may be ~200 KB of base64; copying it into an
  // internal-RAM std::string is the exact OOM we are eliminating. We therefore scan
  // the raw PSRAM span for the type + data spans directly. ---
  {
    // Wrap the raw span in a non-owning std::string only for the tiny find() calls
    // below would copy; instead use the span helpers that take (const char*, size_t).
    size_t toff = 0, tlen = 0;
    if (HoshiWsAudio::json_find_string_span_raw_(buf, len, "type", toff, tlen) &&
        tlen == 9 && std::memcmp(buf + toff, "llm_audio", 9) == 0) {
      // Stale-turn guard against the raw span (tolerant — see below).
      size_t idoff = 0, idlen = 0;
      if (HoshiWsAudio::json_find_string_span_raw_(buf, len, "turnId", idoff, idlen) && idlen > 0) {
        long got = 0; bool numeric = true;
        for (size_t i = 0; i < idlen; i++) {
          char c = buf[idoff + i];
          if (c < '0' || c > '9') { numeric = false; break; }
          got = got * 10 + (c - '0');
        }
        long cur = (long) this->turn_id_.load();
        if (numeric && got != 0 && got < cur) {
          ESP_LOGD(TAG, "drop stale-turn llm_audio turnId span (cur=%ld)", cur);
          return;
        }
      }
      size_t doff = 0, dlen = 0;
      if (HoshiWsAudio::json_find_string_span_raw_(buf, len, "data", doff, dlen) && dlen > 0) {
        if (this->turn_state_.load() != TurnState::SPEAKING)
          this->enter_speaking_();
        this->handle_llm_audio_(buf + doff, dlen);
      } else {
        ESP_LOGW(TAG, "llm_audio without data field");
      }
      return;
    }
  }

  // --- all other (small) control frames: a transient copy is harmless (no base64).
  std::string json(buf, len);

  std::string mtype;
  if (!HoshiWsAudio::json_get_string_(json, "type", mtype)) {
    ESP_LOGV(TAG, "downlink without type: %s", json.c_str());
    return;
  }

  // turnId guard (red-team fix #5): TOLERANT. Only drop a frame if it carries a
  // turnId that clearly belongs to an OLDER turn (numeric and < current). Missing,
  // non-numeric, or matching/newer ids are accepted (the server may echo a string
  // id or omit it; we must not drop the whole turn on a format mismatch).
  std::string tid;
  if (HoshiWsAudio::json_get_string_(json, "turnId", tid) && !tid.empty()) {
    long got = 0; bool numeric = true;
    for (char c : tid) { if (c < '0' || c > '9') { numeric = false; break; } got = got * 10 + (c - '0'); }
    long cur = (long) this->turn_id_.load();
    if (numeric && got != 0 && got < cur) {
      ESP_LOGD(TAG, "drop stale-turn frame type=%s turnId=%s (cur=%ld)", mtype.c_str(), tid.c_str(), cur);
      return;
    }
  }

  if (mtype == "transcript") {
    std::string text;
    HoshiWsAudio::json_get_string_(json, "text", text);
    ESP_LOGI(TAG, "ws <- transcript: %s", text.c_str());
    // Advance to THINKING (loop owns watchdog; we just set the state + timestamp).
    if (this->turn_state_.load() == TurnState::AWAITING_STT) {
      this->turn_state_.store(TurnState::THINKING);
      this->state_entered_ms_ = millis();
    }
  } else if (mtype == "no_input") {
    ESP_LOGI(TAG, "ws <- no_input -> ending turn");
    this->turn_state_.store(TurnState::ERROR);  // brief red, loop returns to idle
    this->state_entered_ms_ = millis();
  } else if (mtype == "transcribing_started" || mtype == "llm_thinking" ||
             mtype == "llm_start" || mtype == "llm_delta" || mtype == "session_meta") {
    ESP_LOGD(TAG, "ws <- %s", mtype.c_str());
  } else if (mtype == "tts_audio_start") {
    ESP_LOGI(TAG, "ws <- tts_audio_start");
    this->enter_speaking_();
  } else if (mtype == "tts_audio_end") {
    ESP_LOGD(TAG, "ws <- tts_audio_end");
  } else if (mtype == "llm_done") {
    ESP_LOGI(TAG, "ws <- llm_done -> turn %u done", (unsigned) this->turn_id_.load());
    if (this->turn_state_.load() == TurnState::SPEAKING) {
      // Defer turn-end to loop() (after play_rb_ + speaker drain + tail). Fix #2:
      // loop honours a grace window so a straggler llm_audio still lands.
      this->llm_done_seen_.store(true);
    } else {
      // No audio (ttsHandled=true / text-only): end now (brief idle via loop).
      this->llm_done_seen_.store(true);
      // Nudge: if not speaking, push state so loop tears down promptly.
      if (this->turn_state_.load() == TurnState::THINKING ||
          this->turn_state_.load() == TurnState::AWAITING_STT) {
        this->turn_state_.store(TurnState::ERROR);  // brief, loop -> idle (no audio)
        this->state_entered_ms_ = millis();
      }
    }
  } else if (mtype == "llm_error") {
    std::string stage, message;
    HoshiWsAudio::json_get_string_(json, "stage", stage);
    HoshiWsAudio::json_get_string_(json, "message", message);
    ESP_LOGW(TAG, "ws <- llm_error stage=%s: %s", stage.c_str(), message.c_str());
    this->turn_state_.store(TurnState::ERROR);
    this->state_entered_ms_ = millis();
  } else if (mtype == "turn_aborted") {
    ESP_LOGI(TAG, "ws <- turn_aborted (server ack)");
  } else if (mtype == "sidecar_alarm") {
    ESP_LOGW(TAG, "ws <- sidecar_alarm: %s", json.c_str());
  } else if (mtype == "timer_state") {
    // LED package 1 (c7): timer countdown arc. Device side of the proposed downlink
    // push {type:"timer_state", remainingS:<int>, totalS:<int>} — the server side
    // is not implemented yet in the reference backend; until it lands this branch is
    // simply never taken. totalS<=0 (or remainingS<=0) clears the arc. ws-task
    // context -> atomics; drive_led_ extrapolates remaining between pushes.
    long total = 0, rem = 0;
    HoshiWsAudio::json_get_number_(json, "totalS", total);
    HoshiWsAudio::json_get_number_(json, "remainingS", rem);
    this->timer_total_s_.store((int32_t) total);
    this->timer_remaining_rx_s_.store((int32_t) rem);
    this->timer_rx_ms_.store(millis());
    ESP_LOGI(TAG, "ws <- timer_state remaining=%lds total=%lds", rem, total);
  } else if (mtype == "speaker") {
    // LED package 1 (c4): recognition shimmer — device side of an optional "speaker"
    // downlink extension. If your backend can identify who's talking, it can push
    // {type:"speaker", speakerId:"..."} for a brief per-speaker accent-color glow.
    // Not implemented server-side in the reference deployment; the accent map lives
    // HERE (device), so the wire only ever carries a plain speakerId string — extend
    // the table below with your own household's speaker IDs and colors.
    std::string sid;
    if (!HoshiWsAudio::json_get_string_(json, "speakerId", sid) || sid.empty())
      HoshiWsAudio::json_get_string_(json, "recognizedSpeaker", sid);
    uint32_t rgb;
    if (sid == "example-speaker-a") {
      rgb = 0x01FFAA28u;  // warm gold
    } else if (sid == "example-speaker-b") {
      rgb = 0x01FF6E96u;  // sakura pink
    } else {
      rgb = 0x018C8C96u;  // unknown/guest: neutral grey (never guessed)
    }
    this->speaker_accent_rgb_.store(rgb);
    this->speaker_flash_until_ms_.store(millis() + 700);
    ESP_LOGI(TAG, "ws <- speaker '%s' -> accent shimmer", sid.c_str());
  } else if (mtype == "night_mode") {
    // Night mode (LED package 2, maintainer sign-off 2026-07-15): server-pushed global dim.
    // {"type":"night_mode","active":<bool>,"dim":<float 0..1>} — Serverseite live
    // seit 12.07 (Push bei ws-Connect + Settings-PUT + Scheduler-Grenze). ws-Task
    // context -> atomics; loop() persists to NVS via the dirty flag (single writer).
    bool active = false;
    float dimv = 1.0f;
    HoshiWsAudio::json_get_bool_(json, "active", active);
    if (!HoshiWsAudio::json_get_float_(json, "dim", dimv))
      dimv = 0.25f;  // frame without dim: sane dark default
    if (dimv < 0.0f) dimv = 0.0f;
    if (dimv > 1.0f) dimv = 1.0f;
    this->night_active_.store(active);
    this->night_dim_.store(dimv);
    this->night_pref_dirty_.store(true);
    ESP_LOGI(TAG, "ws <- night_mode active=%s dim=%.2f", YESNO(active), dimv);
  } else {
    ESP_LOGD(TAG, "ws <- unhandled type=%s", mtype.c_str());
  }
}

void HoshiWsAudio::on_ws_binary_(const uint8_t *data, size_t len) {
  (void) data;
  ESP_LOGD(TAG, "ignoring unexpected binary downlink (%u bytes)", (unsigned) len);
}

// Grow the PSRAM-backed reassembly buffer to at least `need` bytes (external RAM
// first, internal fallback). Grown geometrically and reused; never shrunk. ws-task
// context only (single owner). Returns false if the allocation fails.
bool HoshiWsAudio::rx_reserve_(size_t need) {
#ifdef USE_ESP32
  if (need <= this->rx_cap_)
    return true;
  // Grow with headroom so a stream of similarly-sized frames doesn't realloc each
  // time. Start at 8 KB (covers all small control frames without any alloc churn).
  size_t new_cap = this->rx_cap_ ? this->rx_cap_ : 8192;
  while (new_cap < need)
    new_cap *= 2;
  esphome::RAMAllocator<char> alloc(esphome::RAMAllocator<char>::ALLOC_EXTERNAL |
                                    esphome::RAMAllocator<char>::ALLOC_INTERNAL);  // PSRAM first
  char *nb = alloc.allocate(new_cap);
  if (nb == nullptr) {
    ESP_LOGE(TAG, "rx_reserve_: failed to allocate %u B (PSRAM+internal exhausted)",
             (unsigned) new_cap);
    return false;
  }
  if (this->rx_buf_ != nullptr) {
    if (this->rx_len_ > 0)
      std::memcpy(nb, this->rx_buf_, this->rx_len_);
    alloc.deallocate(this->rx_buf_, this->rx_cap_);
  }
  this->rx_buf_ = nb;
  this->rx_cap_ = new_cap;
  ESP_LOGD(TAG, "rx reassembly buffer -> %u B (PSRAM-first)", (unsigned) new_cap);
  return true;
#else
  (void) need;
  return false;
#endif
}

// STREAM-decode base64 WAV (PCM16/24k/mono) straight into play_rb_ in small chunks.
// CRITICAL (downlink OOM fix): this NEVER materialises the full decoded WAV. The
// base64 input [b64, b64+b64_len) is read in place from the PSRAM reassembly buffer;
// decoding runs through a tiny fixed STACK scratch buffer; only the PCM payload is
// written into the 64 KB play ring (which + the speaker drain naturally bound memory
// regardless of reply length). A long reply just streams through.
//
// WAV header handling: we stage the first decoded bytes until we have located the
// 'data' chunk (robust to non-44-byte headers, exactly as before), then stream every
// subsequent decoded byte as PCM. NO resampling here (speaker = return_sample_rate_
// 24k; the YAML resampler does 24k->48k).
void HoshiWsAudio::handle_llm_audio_(const char *b64, size_t b64_len) {
  if (this->play_rb_ == nullptr || b64 == nullptr || b64_len == 0)
    return;

  // (4) OBSERVABILITY: log the incoming frame size up front so on-device tests
  // confirm frames arrive + their magnitude. Decoded length is ~3/4 of base64.
  ESP_LOGI(TAG, "llm_audio: b64=%u -> pcm≈%u B (stream-decode)",
           (unsigned) b64_len, (unsigned) (b64_len * 3 / 4));

  // Mark the play producer active across the decode+write (fix #2 quiesce guard).
  this->producer_active_.store(true);

  // Build the base64 reverse table once (same alphabet as base64_decode_).
  static bool built = false;
  static int8_t rev[256];
  if (!built) {
    for (int i = 0; i < 256; i++) rev[i] = -1;
    const char *alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; i++) rev[(unsigned char) alpha[i]] = (int8_t) i;
    rev[(unsigned char) '='] = -2;
    built = true;
  }

  // Small fixed scratch for decoded bytes (STACK, ~2 KB). Never holds the whole WAV.
  uint8_t out[2048];
  size_t out_n = 0;

  // Header staging: accumulate just enough leading decoded bytes to locate 'data'.
  // A canonical header is 44 B; allow up to 256 B for non-standard chunk ordering.
  uint8_t hdr[256];
  size_t hdr_n = 0;
  bool header_done = false;   // true once 'data' located and we are streaming PCM
  size_t data_remaining = 0;  // bytes of PCM still to forward from the 'data' chunk

  int val = 0, bits = 0;
  bool aborted = false;
  size_t pcm_written = 0;

  // Lambda: push a run of decoded bytes [src, src+n) honouring header staging then
  // streaming PCM into play_rb_ with bounded backpressure. Returns false on a fatal
  // condition (bad RIFF / ring drop) so the caller stops.
  auto consume = [&](const uint8_t *src, size_t n) -> bool {
    size_t i = 0;
    // Phase A: still locating the WAV 'data' chunk.
    while (!header_done && i < n) {
      hdr[hdr_n++] = src[i++];
      // Validate RIFF/WAVE as soon as we have the first 12 bytes.
      if (hdr_n == 12) {
        if (std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
          ESP_LOGW(TAG, "llm_audio not RIFF/WAVE -> skip");
          return false;
        }
      }
      // Walk chunks once we have >=12 bytes; look for 'data'.
      if (hdr_n >= 12) {
        size_t p = 12;
        while (p + 8 <= hdr_n) {
          uint32_t csz = (uint32_t) hdr[p + 4] | ((uint32_t) hdr[p + 5] << 8) |
                         ((uint32_t) hdr[p + 6] << 16) | ((uint32_t) hdr[p + 7] << 24);
          if (std::memcmp(hdr + p, "data", 4) == 0) {
            // Found it. Any header-staged bytes BEYOND p+8 are already PCM; forward
            // them, then switch to streaming mode for the remainder of the input.
            size_t pcm_off = p + 8;
            data_remaining = csz;  // declared data size (may be trimmed by stream end)
            header_done = true;
            size_t staged_pcm = (hdr_n > pcm_off) ? (hdr_n - pcm_off) : 0;
            if (staged_pcm > data_remaining)
              staged_pcm = data_remaining;
            // Write staged PCM.
            size_t w = 0;
            while (w < staged_pcm) {
              size_t got = this->play_rb_->write_without_replacement(
                  hdr + pcm_off + w, staged_pcm - w, pdMS_TO_TICKS(200));
              if (got == 0) {
                ESP_LOGW(TAG, "play ring full: dropped %u B of llm_audio (raise play_rb_bytes)",
                         (unsigned) (staged_pcm - w));
                return false;
              }
              w += got;
            }
            pcm_written += staged_pcm;
            data_remaining -= staged_pcm;
            break;
          }
          // Skip this non-data chunk (8-byte id+size + padded body) IF fully staged.
          size_t advance = 8 + csz + (csz & 1);
          if (p + advance > hdr_n)
            break;  // need more bytes to finish skipping this chunk
          p += advance;
        }
      }
      if (!header_done && hdr_n >= sizeof(hdr)) {
        ESP_LOGW(TAG, "llm_audio header > %u B without 'data' -> skip", (unsigned) sizeof(hdr));
        return false;
      }
    }
    // Phase B: streaming PCM. Forward the rest of this decoded run, capped by the
    // declared 'data' size (if any remains).
    if (header_done && i < n) {
      size_t avail = n - i;
      if (data_remaining > 0 && avail > data_remaining)
        avail = data_remaining;
      size_t w = 0;
      while (w < avail) {
        size_t got = this->play_rb_->write_without_replacement(
            src + i + w, avail - w, pdMS_TO_TICKS(200));
        if (got == 0) {
          ESP_LOGW(TAG, "play ring full: dropped %u B of llm_audio (raise play_rb_bytes)",
                   (unsigned) (avail - w));
          return false;
        }
        w += got;
      }
      pcm_written += avail;
      if (data_remaining > 0)
        data_remaining -= avail;
    }
    return true;
  };

  // Stream the base64 input in place. Decode 6 bits at a time into `out`; flush the
  // scratch through consume() whenever it fills. This is O(scratch) extra memory.
  for (size_t k = 0; k < b64_len && !aborted; k++) {
    int8_t d = rev[(unsigned char) b64[k]];
    if (d == -1)
      continue;   // skip whitespace / newlines
    if (d == -2)
      break;      // '=' padding -> end of data
    val = (val << 6) | d;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[out_n++] = (uint8_t) ((val >> bits) & 0xFF);
      if (out_n == sizeof(out)) {
        if (!consume(out, out_n)) { aborted = true; break; }
        out_n = 0;
      }
    }
  }
  // Flush any decoded tail.
  if (!aborted && out_n > 0)
    consume(out, out_n);

  if (!header_done && !aborted)
    ESP_LOGW(TAG, "llm_audio: no 'data' chunk decoded -> nothing played");

  this->producer_active_.store(false);
}

// =============================================================================
// Tiny JSON helpers + base64 (no ArduinoJson under esp-idf).
// =============================================================================
bool HoshiWsAudio::json_get_string_(const std::string &json, const char *key, std::string &out) {
  std::string needle = "\"";
  needle += key;
  needle += "\"";
  size_t k = json.find(needle);
  if (k == std::string::npos)
    return false;
  size_t c = json.find(':', k + needle.size());
  if (c == std::string::npos)
    return false;
  size_t q1 = json.find('"', c + 1);
  if (q1 == std::string::npos)
    return false;
  size_t q2 = q1 + 1;
  std::string val;
  while (q2 < json.size()) {
    char ch = json[q2];
    if (ch == '\\' && q2 + 1 < json.size()) {
      char nx = json[q2 + 1];
      if (nx == 'n') val.push_back('\n');
      else if (nx == 't') val.push_back('\t');
      else if (nx == '"') val.push_back('"');
      else if (nx == '\\') val.push_back('\\');
      else if (nx == '/') val.push_back('/');
      else val.push_back(nx);
      q2 += 2;
      continue;
    }
    if (ch == '"')
      break;
    val.push_back(ch);
    q2++;
  }
  out.swap(val);
  return true;
}

// Locate a string value's RAW span in a (const char*, len) buffer without copying.
// Mirrors json_get_string_'s key/colon/quote scan but returns offsets into `buf`.
// Walks to the CLOSING quote honouring backslash escapes (so the span ends at the
// real terminator). The returned span is the raw bytes; for base64 it contains no
// escapes, so it is byte-identical to the value to decode.
bool HoshiWsAudio::json_find_string_span_raw_(const char *buf, size_t len, const char *key,
                                              size_t &val_off, size_t &val_len) {
  if (buf == nullptr || len == 0)
    return false;
  // Build the needle "\"key\"".
  std::string needle = "\"";
  needle += key;
  needle += "\"";
  // Find the needle within [buf, buf+len). std::string::find over a temporary view
  // would copy; do a simple in-place search instead (buffer may be ~200 KB).
  const size_t nlen = needle.size();
  if (nlen == 0 || nlen > len)
    return false;
  size_t k = std::string::npos;
  for (size_t i = 0; i + nlen <= len; i++) {
    if (std::memcmp(buf + i, needle.data(), nlen) == 0) { k = i; break; }
  }
  if (k == std::string::npos)
    return false;
  // Find ':' after the key.
  size_t c = k + nlen;
  while (c < len && buf[c] != ':') {
    // Bail if we run into another key before a colon (defensive).
    if (buf[c] == '}') return false;
    c++;
  }
  if (c >= len)
    return false;
  // Find the opening quote of the value.
  size_t q1 = c + 1;
  while (q1 < len && buf[q1] != '"') {
    if (buf[q1] == ',' || buf[q1] == '}') return false;  // value is not a string
    q1++;
  }
  if (q1 >= len)
    return false;
  // Scan to the closing quote, honouring backslash escapes.
  size_t q2 = q1 + 1;
  while (q2 < len) {
    char ch = buf[q2];
    if (ch == '\\' && q2 + 1 < len) { q2 += 2; continue; }
    if (ch == '"')
      break;
    q2++;
  }
  if (q2 >= len)
    return false;  // unterminated
  val_off = q1 + 1;
  val_len = q2 - (q1 + 1);
  return true;
}

// Float value ("dim":0.25) — integer part via the digit scan idiom, optional
// fractional part. No exponent support (the wire never sends one).
bool HoshiWsAudio::json_get_float_(const std::string &json, const char *key, float &out) {
  std::string needle = "\"";
  needle += key;
  needle += "\"";
  size_t k = json.find(needle);
  if (k == std::string::npos)
    return false;
  size_t c = json.find(':', k + needle.size());
  if (c == std::string::npos)
    return false;
  size_t p = c + 1;
  while (p < json.size() && (json[p] == ' ' || json[p] == '\t'))
    p++;
  bool neg = false;
  if (p < json.size() && (json[p] == '-' || json[p] == '+')) {
    neg = (json[p] == '-');
    p++;
  }
  bool any = false;
  float v = 0.0f;
  while (p < json.size() && json[p] >= '0' && json[p] <= '9') {
    v = v * 10.0f + (float) (json[p] - '0');
    p++;
    any = true;
  }
  if (p < json.size() && json[p] == '.') {
    p++;
    float scale = 0.1f;
    while (p < json.size() && json[p] >= '0' && json[p] <= '9') {
      v += (float) (json[p] - '0') * scale;
      scale *= 0.1f;
      p++;
      any = true;
    }
  }
  if (!any)
    return false;
  out = neg ? -v : v;
  return true;
}

// Bare JSON bool ("active":true) — unquoted literal, so neither the string nor the
// number scanner matches it.
bool HoshiWsAudio::json_get_bool_(const std::string &json, const char *key, bool &out) {
  std::string needle = "\"";
  needle += key;
  needle += "\"";
  size_t k = json.find(needle);
  if (k == std::string::npos)
    return false;
  size_t c = json.find(':', k + needle.size());
  if (c == std::string::npos)
    return false;
  size_t p = c + 1;
  while (p < json.size() && (json[p] == ' ' || json[p] == '\t'))
    p++;
  if (json.compare(p, 4, "true") == 0) {
    out = true;
    return true;
  }
  if (json.compare(p, 5, "false") == 0) {
    out = false;
    return true;
  }
  return false;
}

bool HoshiWsAudio::json_get_number_(const std::string &json, const char *key, long &out) {
  std::string needle = "\"";
  needle += key;
  needle += "\"";
  size_t k = json.find(needle);
  if (k == std::string::npos)
    return false;
  size_t c = json.find(':', k + needle.size());
  if (c == std::string::npos)
    return false;
  size_t p = c + 1;
  while (p < json.size() && (json[p] == ' ' || json[p] == '\t'))
    p++;
  bool neg = false;
  if (p < json.size() && (json[p] == '-' || json[p] == '+')) {
    neg = (json[p] == '-');
    p++;
  }
  bool any = false;
  long v = 0;
  while (p < json.size() && json[p] >= '0' && json[p] <= '9') {
    v = v * 10 + (json[p] - '0');
    p++;
    any = true;
  }
  if (!any)
    return false;
  out = neg ? -v : v;
  return true;
}

std::vector<uint8_t> HoshiWsAudio::base64_decode_(const std::string &in) {
  static bool built = false;
  static int8_t rev[256];
  if (!built) {
    for (int i = 0; i < 256; i++) rev[i] = -1;
    const char *alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; i++) rev[(unsigned char) alpha[i]] = (int8_t) i;
    rev[(unsigned char) '='] = -2;
    built = true;
  }
  std::vector<uint8_t> out;
  out.reserve(in.size() * 3 / 4 + 3);
  int val = 0;
  int bits = 0;
  for (unsigned char ch : in) {
    int8_t d = rev[ch];
    if (d == -1)
      continue;
    if (d == -2)
      break;
    val = (val << 6) | d;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back((uint8_t) ((val >> bits) & 0xFF));
    }
  }
  return out;
}

}  // namespace hoshi_ws_audio
}  // namespace esphome
