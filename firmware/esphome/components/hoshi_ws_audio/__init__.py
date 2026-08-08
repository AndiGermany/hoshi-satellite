# =============================================================================
# hoshi_ws_audio — ESPHome external component (config schema / codegen)
# =============================================================================
#
# REDESIGN 2026-06-21 (Route B "done right"): NO networking ever runs on the
#   ESPHome main loop. Four strictly-separated contexts (see hoshi_ws_audio.h):
#     1. mic callback  — passive microphone::MicrophoneSource over i2s_mics:
#        gives us already-channel-selected, 32->16-bit-converted, gain-applied
#        PCM16 mono. Callback only: write uplink RingBuffer + RMS VAD + atomic eos.
#     2. ESPHome loop() — drain play RingBuffer to speaker, turn state machine,
#        watchdog deadlines, LED.
#     3. hoshi_ws_tx FreeRTOS task — the ONLY caller of esp_websocket_client send.
#     4. IDF esp_websocket_client task (ws_event_) — TLS+recv+dispatch.
#
# WS client = esp-idf `esp_websocket_client` (managed IDF component, ^1.7.0),
#   pulled via esp32.add_idf_component. Its esp-tls cert_pem is the hoshi-server leaf-pin.
# =============================================================================

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MICROPHONE, CONF_SPEAKER

from esphome.components import microphone, speaker, light
from esphome.components import esp32  # for esp32.add_idf_component (esp-idf framework only)

CODEOWNERS = ["@hoshi-satellite"]
# AUTO_LOAD ring_buffer: we use esphome::ring_buffer::RingBuffer for the uplink +
#   play single-producer/single-consumer queues (red-team fix #2/#3).
AUTO_LOAD = ["ring_buffer"]
DEPENDENCIES = ["microphone", "speaker", "network", "esp32"]

CONF_HOST = "host"
CONF_PORT = "port"
CONF_PATH = "path"
CONF_AUTH_TOKEN = "auth_token"
CONF_AUTH_MODE = "auth_mode"
CONF_CACERT_PEM = "cacert_pem"
CONF_SKIP_CN_CHECK = "skip_cert_common_name_check"
CONF_UPLINK_SAMPLE_RATE = "uplink_sample_rate"
CONF_RETURN_SAMPLE_RATE = "return_sample_rate"
CONF_HALF_DUPLEX = "half_duplex"
# Output volume forwarded to the speaker chain (resampler -> i2s speaker -> aic3204 DAC).
# MUST be > 0: the aic3204 powers up SILENT (deferred write_volume_ with volume_=0 ->
# DAC digital vol -127) and we have no media_player to re-assert it (fix #2).
CONF_SPEAKER_VOLUME = "speaker_volume"
CONF_ROOM = "room"
CONF_SATELLITE_ID = "satellite_id"
# On-device energy/RMS VAD tunables.
CONF_VAD_RMS_THRESHOLD = "vad_rms_threshold"
CONF_SILENCE_MS = "silence_ms"
CONF_MIN_SPEECH_MS = "min_speech_ms"
CONF_MAX_TURN_MS = "max_turn_ms"
# Adaptive endpointing vs. background speech (TV, field finding 2026-07-17): "silence"
# is RELATIVE to the tracked room ambient (EMA at idle) instead of an absolute RMS.
# margin = factor above ambient that still counts as room noise; floor_max = safety
# ceiling (above it behaviour degrades to the pre-patch absolute thresholds).
CONF_VAD_AMBIENT_MARGIN = "vad_ambient_margin"
CONF_VAD_SILENCE_FLOOR_MAX = "vad_silence_floor_max"
# Buffer sizing + watchdog deadlines.
CONF_UPLINK_RB_BYTES = "uplink_rb_bytes"
CONF_PLAY_RB_BYTES = "play_rb_bytes"
CONF_STT_TIMEOUT_MS = "stt_timeout_ms"
CONF_THINK_TIMEOUT_MS = "think_timeout_ms"
CONF_SPEAK_TIMEOUT_MS = "speak_timeout_ms"
# Optional LED + micro_wake_word handles (driven from loop()/state machine).
CONF_LIGHT = "light"
CONF_MICRO_WAKE_WORD = "micro_wake_word"

AUTH_MODES = ["bearer", "query"]

hoshi_ws_audio_ns = cg.esphome_ns.namespace("hoshi_ws_audio")
HoshiWsAudio = hoshi_ws_audio_ns.class_("HoshiWsAudio", cg.Component)

# micro_wake_word component (so we can stop()/start() it from C++ during SPEAKING).
micro_wake_word_ns = cg.esphome_ns.namespace("micro_wake_word")
MicroWakeWord = micro_wake_word_ns.class_("MicroWakeWord", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HoshiWsAudio),
        # PASSIVE microphone source over i2s_mics (red-team fix #1 + #6): the source
        #   handles channel-selection, 32->16-bit conversion and gain, so the callback
        #   receives ready-to-use PCM16 mono. channels/gain_factor/bits stay YAML knobs.
        cv.Required(CONF_MICROPHONE): microphone.microphone_source_schema(
            min_bits_per_sample=16,
            max_bits_per_sample=16,
            min_channels=1,
            max_channels=1,
        ),
        cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
        cv.Required(CONF_HOST): cv.string,
        # 0.8-Realität (2026-08-08): Hoshi 0.5/:8081 ist retired, 0.8-Prod läuft auf
        # :8082 (siehe hoshi-voice-pe.yaml Kopf-Kommentar 0.8-CUTOVER 2026-07-08).
        # Falsche Defaults hier wären ein STUMMES Close 1008, wenn eine fremde YAML
        # den `port:`/`auth_mode:`-Key mal weglässt. Andis eigene YAML setzt beide
        # Keys explizit (hoshi-voice-pe.yaml: port: ${hoshi_port} = "8082",
        # auth_mode: query) — dieser Default-Wechsel ändert für ihn NICHTS.
        cv.Optional(CONF_PORT, default=8082): cv.port,
        cv.Optional(CONF_PATH, default="/ws/audio"): cv.string,
        cv.Optional(CONF_AUTH_TOKEN, default=""): cv.string,
        # 0.8 WS wall liest den Token NUR aus dem ?token=-Query-Param (siehe YAML-
        # Kommentar bei auth_mode); "bearer" bleibt nur für 0.5-era-Kompatibilität im
        # Schema. Default jetzt "query" — Andis YAML setzt auth_mode explizit, siehe oben.
        cv.Optional(CONF_AUTH_MODE, default="query"): cv.one_of(*AUTH_MODES, lower=True),
        cv.Optional(CONF_CACERT_PEM, default=""): cv.string,
        # IP-SAN sharp edge (red-team fix #4): if the leaf has an IP SAN that esp-tls
        #   won't validate against the host, allow skipping the CN/SAN check (still
        #   pins the leaf bytes via cert_pem). Default False = strict.
        cv.Optional(CONF_SKIP_CN_CHECK, default=False): cv.boolean,
        cv.Optional(CONF_UPLINK_SAMPLE_RATE, default=16000): cv.positive_int,
        cv.Optional(CONF_RETURN_SAMPLE_RATE, default=24000): cv.positive_int,
        cv.Optional(CONF_HALF_DUPLEX, default=True): cv.boolean,
        # Output volume [0.0, 1.0]. Default 0.85 (= upstream media_player volume_max);
        # MUST be > 0 or the aic3204 stays silent (fix #2).
        cv.Optional(CONF_SPEAKER_VOLUME, default=0.85): cv.percentage,
        cv.Optional(CONF_ROOM, default=""): cv.string,
        cv.Optional(CONF_SATELLITE_ID, default=""): cv.string,
        cv.Optional(CONF_VAD_RMS_THRESHOLD, default=700): cv.positive_int,
        cv.Optional(CONF_SILENCE_MS, default=900): cv.positive_int,
        cv.Optional(CONF_MIN_SPEECH_MS, default=50): cv.positive_int,
        cv.Optional(CONF_MAX_TURN_MS, default=10000): cv.positive_int,
        cv.Optional(CONF_VAD_AMBIENT_MARGIN, default=1.30): cv.positive_float,
        cv.Optional(CONF_VAD_SILENCE_FLOOR_MAX, default=2000): cv.positive_int,
        # Uplink ring >= 64KB (red-team fix #3): ~2s of 16k/16-bit mono.
        cv.Optional(CONF_UPLINK_RB_BYTES, default=65536): cv.positive_int,
        # Play ring (PSRAM, EXTERNAL_FIRST): 256 KB so one ~260 KB single-frame llm_audio
        # reply (~5.4s of 24k/16-bit mono) fits comfortably — removes timing pressure on the
        # loop()-side drain (defensive fix #3; cheap in PSRAM, no drops were observed).
        cv.Optional(CONF_PLAY_RB_BYTES, default=262144): cv.positive_int,
        # Watchdog deadlines per state (red-team: force to_idle_ + error LED).
        cv.Optional(CONF_STT_TIMEOUT_MS, default=15000): cv.positive_int,
        cv.Optional(CONF_THINK_TIMEOUT_MS, default=30000): cv.positive_int,
        cv.Optional(CONF_SPEAK_TIMEOUT_MS, default=60000): cv.positive_int,
        # Optional LED + mww handles (state-driven feedback + half-duplex self-trigger guard).
        cv.Optional(CONF_LIGHT): cv.use_id(light.LightState),
        cv.Optional(CONF_MICRO_WAKE_WORD): cv.use_id(MicroWakeWord),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Passive microphone source (red-team fix #1): never start/stops the mic — it only
    #   receives audio while micro_wake_word (the active consumer) has it running.
    mic_source = await microphone.microphone_source_to_code(
        config[CONF_MICROPHONE], passive=True
    )
    cg.add(var.set_microphone_source(mic_source))

    spk = await cg.get_variable(config[CONF_SPEAKER])
    cg.add(var.set_speaker(spk))

    cg.add(var.set_host(config[CONF_HOST]))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_path(config[CONF_PATH]))

    cg.add(var.set_auth_token(config[CONF_AUTH_TOKEN]))
    cg.add(var.set_auth_mode(config[CONF_AUTH_MODE]))
    cg.add(var.set_cacert_pem(config[CONF_CACERT_PEM]))
    cg.add(var.set_skip_cert_common_name_check(config[CONF_SKIP_CN_CHECK]))

    cg.add(var.set_uplink_sample_rate(config[CONF_UPLINK_SAMPLE_RATE]))
    cg.add(var.set_return_sample_rate(config[CONF_RETURN_SAMPLE_RATE]))
    cg.add(var.set_half_duplex(config[CONF_HALF_DUPLEX]))
    cg.add(var.set_speaker_volume(config[CONF_SPEAKER_VOLUME]))

    cg.add(var.set_room(config[CONF_ROOM]))
    cg.add(var.set_satellite_id(config[CONF_SATELLITE_ID]))

    cg.add(var.set_vad_rms_threshold(config[CONF_VAD_RMS_THRESHOLD]))
    cg.add(var.set_silence_ms(config[CONF_SILENCE_MS]))
    cg.add(var.set_min_speech_ms(config[CONF_MIN_SPEECH_MS]))
    cg.add(var.set_max_turn_ms(config[CONF_MAX_TURN_MS]))
    cg.add(var.set_vad_ambient_margin(config[CONF_VAD_AMBIENT_MARGIN]))
    cg.add(var.set_vad_silence_floor_max(config[CONF_VAD_SILENCE_FLOOR_MAX]))

    cg.add(var.set_uplink_rb_bytes(config[CONF_UPLINK_RB_BYTES]))
    cg.add(var.set_play_rb_bytes(config[CONF_PLAY_RB_BYTES]))
    cg.add(var.set_stt_timeout_ms(config[CONF_STT_TIMEOUT_MS]))
    cg.add(var.set_think_timeout_ms(config[CONF_THINK_TIMEOUT_MS]))
    cg.add(var.set_speak_timeout_ms(config[CONF_SPEAK_TIMEOUT_MS]))

    if CONF_LIGHT in config:
        led = await cg.get_variable(config[CONF_LIGHT])
        cg.add(var.set_light(led))
    if CONF_MICRO_WAKE_WORD in config:
        mww = await cg.get_variable(config[CONF_MICRO_WAKE_WORD])
        cg.add(var.set_micro_wake_word(mww))

    # --- WS client = esp-idf esp_websocket_client (managed IDF component) -----------------
    # PINNED EXACT 1.7.0 (2026-08-08, matches hoshi-voice-pe.yaml esp32.components pin —
    # this codegen call is the authoritative declaration, the YAML block documents/locks
    # it too). Verified against a real successful build's dependencies.lock — see the YAML
    # comment for the resolved component_hash + upstream commit_sha.
    esp32.add_idf_component(
        name="espressif/esp_websocket_client",
        ref="1.7.0",
    )
