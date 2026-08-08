#!/usr/bin/env bash
# =============================================================================
# generate-samples.sh — "Hey Hoshi" Wake-Word Sample-Generator (TURNKEY)
# Ticket: T162 · Lane: satellite-hand · Persona: Eda/Timo
# =============================================================================
#
#   Was dieses Skript tut (alles LOKAL, alles FREI, kein Cloud-Call):
#     1) POSITIVE synthetisieren  — "Hey Hoshi" via piper-sample-generator
#                                    (en_US-libritts_r-medium, multi-speaker).
#     2) HARD-NEGATIVES synthetisieren — Phrasen aus ./hard-negatives.txt.
#     3) RESAMPLE auf 16 kHz mono PCM16 — microWakeWord/ESPHome-Standard —
#                                    in strukturierte Ausgabeordner.
#
#   Ausgabe-Layout (unter $OUT_ROOT, default ./generated-samples/dataset-v1/):
#       positives/   <- synthetische "Hey Hoshi", 16 kHz mono PCM16
#       negatives/   <- synthetische Hard-Negatives, 16 kHz mono PCM16
#     (echte Aufnahmen kommen separat dazu — siehe TODO[REAL] + recordings/README.md)
#
#   Doktrin: Synthese = Menge (billig, lokal, frei). Echte Aufnahmen = Qualität.
#            OpenAI-TTS ist KEINE Mengen-Quelle (nur ~10 Stimmen → Overfit).
#            Augmentation (Background/RIR/Gain/Pitch) gehört in den TRAINER,
#            NICHT in diesen Resample-Schritt — siehe train/TRAIN-RUNBOOK.md.
#
#   ---------------------------------------------------------------------------
#   VORAUSSETZUNGEN (auf Andis Mac bereits erfüllt, 2026-06-13, siehe SETUP-LOG.md):
#     - venv unter ./.venv mit piper-sample-generator installiert
#     - Generator-Modell ./piper-sample-generator/models/en_US-libritts_r-medium.pt
#       (PFLICHT 204089915 Bytes — Größen-Check unten bricht sonst ab)
#     - ffmpeg im PATH (für sauberes, augmentationsfreies 16k-Resampling)
#   Falls eine Voraussetzung fehlt: das Skript meldet es klar und bricht ab,
#   bevor irgendetwas generiert wird. KEINE großen Downloads im Default-Lauf.
#
#   AUFRUF:
#     bash generate-samples.sh                 # Default: 200 Positive + Negatives
#     POS_SAMPLES=1000 bash generate-samples.sh
#     POS_SAMPLES=2000 NEG_PER_PHRASE=40 OUT_ROOT=./generated-samples/dataset-v2 \
#                                              bash generate-samples.sh
#     SMOKE=1 bash generate-samples.sh         # 10 Positive, KEINE Negatives (Beweis-Lauf)
#
#   Quellen (verifiziert 2026-06-13 / 2026-06-22):
#     - github.com/rhasspy/piper-sample-generator   (lokal: master HEAD, v3.2.0)
#     - github.com/OHF-Voice/micro-wake-word        (vormals kahrendt/microWakeWord)
#     - esphome.io/components/micro_wake_word/
# =============================================================================

set -euo pipefail

# -----------------------------------------------------------------------------
# Konfiguration (alles per Env überschreibbar)
# -----------------------------------------------------------------------------
WAKE_PHRASE="${WAKE_PHRASE:-Hey Hoshi}"   # 3 Silben bewusst (niedrigere FAR als "Hoshi")

# Verzeichnisse — relativ zum Skript, NICHTS landet außerhalb von wake/.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_PY="${VENV_PY:-${SCRIPT_DIR}/.venv/bin/python}"
PSG_DIR="${PSG_DIR:-${SCRIPT_DIR}/piper-sample-generator}"
GEN_MODEL="${GEN_MODEL:-${PSG_DIR}/models/en_US-libritts_r-medium.pt}"
GEN_MODEL_BYTES="${GEN_MODEL_BYTES:-204089915}"  # PFLICHT-Größe des GEN_MODEL (env-überschreibbar seit 17.07 für DE-Modell; fängt abgebrochene Downloads)
# MULTI-VOICE-Modus (seit 18.07, DE-Fix): GEN_MODELS = space-separierte Liste normaler
# Piper-Stimmen (.onnx) — der Generator cycled sie (--model append). Überschreibt
# GEN_MODEL. Hintergrund: de_DE-mls-medium.pt produziert Kauderwelsch (bekanntes
# MLS-Problem, vgl. HF piper-voices Discussion #13 zur nl-Schwester) — Whisper-
# verifiziert 18.07: 0/18 Samples enthielten die Phrase. Deutsche Qualitäts-Stimmen
# (thorsten & Co.) statt MLS; Byte-Check gilt nur für den .pt-Einzelmodus.
GEN_MODELS="${GEN_MODELS:-}"
HARD_NEG_FILE="${HARD_NEG_FILE:-${SCRIPT_DIR}/hard-negatives.txt}"

OUT_ROOT="${OUT_ROOT:-${SCRIPT_DIR}/generated-samples/dataset-v1}"
OUT_POS="${OUT_ROOT}/positives"
OUT_NEG="${OUT_ROOT}/negatives"
RAW_DIR="${OUT_ROOT}/.raw22k"             # Zwischen-WAVs @22050 Hz vor dem 16k-Resample

# Mengen — MODERATER Default, sequentiell (16-GB-Mac: RAM-Wand respektieren).
POS_SAMPLES="${POS_SAMPLES:-200}"         # synthetische Positive gesamt (1000–5000 für robustes Modell)
NEG_PER_PHRASE="${NEG_PER_PHRASE:-20}"    # synthetische Negative je Hard-Negative-Phrase
BATCH_SIZE="${BATCH_SIZE:-10}"            # MPS-RAM-schonend (SETUP-LOG: 5–25 ok, bei OOM senken)
TARGET_RATE="${TARGET_RATE:-16000}"       # microWakeWord-Standard

# Diversitäts-Stellschrauben (EN-Generator). Mehr Sprecher/Tempo = breiterer Unterbau.
MAX_SPEAKERS="${MAX_SPEAKERS:-700}"       # < 904
LENGTH_SCALES="${LENGTH_SCALES:-0.9 1.0 1.1}"   # Sprechtempo, pro Batch durchrotiert → deckt (c) Tempo ab
SLERP_WEIGHTS="${SLERP_WEIGHTS:-0.0 0.25 0.5 0.75 1.0}"  # Sprecher-Blending

# SMOKE=1 → winziger Beweis-Lauf (10 Positive, keine Negatives).
SMOKE="${SMOKE:-0}"
if [ "${SMOKE}" = "1" ]; then
  POS_SAMPLES=10
  NEG_PER_PHRASE=0
  BATCH_SIZE=5
  echo ">> SMOKE-Modus: ${POS_SAMPLES} Positive, keine Negatives."
fi

# -----------------------------------------------------------------------------
# Helfer
# -----------------------------------------------------------------------------
log()  { printf '\n>> %s\n' "$*"; }
die()  { printf '\nFEHLER: %s\n' "$*" >&2; exit 1; }

# Slug für Dateinamen aus einer Phrase (Kleinbuchstaben, Umlaute → ascii, _ statt Space).
slugify() {
  printf '%s' "$1" \
    | tr '[:upper:]' '[:lower:]' \
    | sed -e 's/ä/ae/g; s/ö/oe/g; s/ü/ue/g; s/ß/ss/g' \
    | tr -c 'a-z0-9' '_' \
    | sed -e 's/__*/_/g; s/^_//; s/_$//'
}

# Resample EIN Verzeichnis (22k WAV) → 16 kHz mono PCM16 nach $2, mit Prefix $3.
# REIN Resample (ffmpeg) — KEINE Augmentation. Augmentation macht der Trainer.
# (Hinweis: piper_sample_generator.augment resampled ZWAR auch, fügt aber Gain+IR
#  hinzu — das wollen wir hier NICHT, damit die Roh-Positives sauber bleiben.)
resample_dir() {
  local src="$1" dst="$2" prefix="$3" n=0
  mkdir -p "${dst}"
  shopt -s nullglob
  for wav in "${src}"/*.wav; do
    n=$((n + 1))
    local out
    out="$(printf '%s/%s_%04d.wav' "${dst}" "${prefix}" "${n}")"
    ffmpeg -nostdin -loglevel error -y -i "${wav}" \
      -ar "${TARGET_RATE}" -ac 1 -sample_fmt s16 "${out}"
  done
  shopt -u nullglob
  printf '%s' "${n}"
}

# Generiere $count Positive einer Phrase nach $outdir (22k roh).
synth_phrase() {
  local phrase="$1" count="$2" outdir="$3"
  [ "${count}" -gt 0 ] || return 0
  mkdir -p "${outdir}"
  # WICHTIG (SETUP-LOG §1): aus dem Repo-Root laufen, sonst ModuleNotFoundError piper_train.
  # Modell-Args: Multi-Voice-Liste (je ein --model, Generator cycled) oder Einzel-.pt.
  local model_args=()
  if [ -n "${GEN_MODELS}" ]; then
    local m
    for m in ${GEN_MODELS}; do model_args+=(--model "${m}"); done
  else
    model_args=(--model "${GEN_MODEL}")
  fi
  ( cd "${PSG_DIR}" && "${VENV_PY}" -m piper_sample_generator "${phrase}" \
      "${model_args[@]}" \
      --max-samples   "${count}" \
      --batch-size    "${BATCH_SIZE}" \
      --max-speakers  "${MAX_SPEAKERS}" \
      --length-scales ${LENGTH_SCALES} \
      --slerp-weights ${SLERP_WEIGHTS} \
      --noise-scales  ${NOISE_SCALES:-0.667} \
      --noise-scale-ws ${NOISE_SCALE_WS:-0.8} \
      --output-dir    "${outdir}/" )
}

# -----------------------------------------------------------------------------
# [0] Voraussetzungen prüfen (bricht VOR jeder Generierung ab, wenn etwas fehlt)
# -----------------------------------------------------------------------------
log "Prüfe Voraussetzungen …"
[ -x "${VENV_PY}" ]      || die "venv-Python fehlt: ${VENV_PY} (siehe SETUP-LOG.md §1/§2)."
[ -d "${PSG_DIR}" ]      || die "piper-sample-generator fehlt: ${PSG_DIR} (SETUP-LOG.md §2)."
command -v ffmpeg >/dev/null 2>&1 || die "ffmpeg fehlt im PATH (brew install ffmpeg)."
if [ -n "${GEN_MODELS}" ]; then
  # Multi-Voice-Modus (.onnx): jede Stimme + zugehörige .onnx.json muss liegen.
  for m in ${GEN_MODELS}; do
    [ -f "${m}" ]      || die "Piper-Stimme fehlt: ${m}"
    [ -f "${m}.json" ] || die "Voice-Config fehlt: ${m}.json (gehört neben die .onnx)"
  done
else
  [ -f "${GEN_MODEL}" ]    || die "Generator-Modell fehlt: ${GEN_MODEL}
    → curl -fL -o '${GEN_MODEL}' \\
        'https://github.com/rhasspy/piper-sample-generator/releases/download/v2.0.0/en_US-libritts_r-medium.pt'
    (PFLICHT: ${GEN_MODEL_BYTES} Bytes. Großer Download — bewusst NICHT im Skript-Default.)"
  actual_bytes="$(stat -f%z "${GEN_MODEL}" 2>/dev/null || stat -c%s "${GEN_MODEL}")"
  [ "${actual_bytes}" -eq "${GEN_MODEL_BYTES}" ] \
    || die "Generator-Modell hat ${actual_bytes} Bytes, erwartet ${GEN_MODEL_BYTES} (Download abgebrochen? SETUP-LOG.md §3)."
fi
[ "${NEG_PER_PHRASE}" -eq 0 ] || [ -f "${HARD_NEG_FILE}" ] \
  || die "hard-negatives.txt fehlt: ${HARD_NEG_FILE}"
log "Voraussetzungen OK. Ziel: ${OUT_ROOT}"
mkdir -p "${OUT_POS}" "${OUT_NEG}" "${RAW_DIR}"

# -----------------------------------------------------------------------------
# [1] POSITIVE synthetisieren → 22k roh → 16k mono
# Seit 19.07: WAKE_PHRASES (';'-separiert) mischt mehrere AUSSPRACHE-Schreibweisen
# derselben Phrase (the maintainer-Ohr-Befund: espeak-de macht aus "Hey" ein "Hai" — "Hej"/
# "Häi" treffen das gesprochene "Hey" besser; "Hoschi" erzwingt den ʃ-Laut).
# POS_SAMPLES wird gleichmäßig auf die Schreibweisen verteilt; Prefixe pos1_/pos2_/…
# halten die Nummerierung kollisionsfrei. Default = das alte Ein-Phrasen-Verhalten.
# -----------------------------------------------------------------------------
WAKE_PHRASES="${WAKE_PHRASES:-${WAKE_PHRASE}}"
IFS=';' read -r -a _phrases <<< "${WAKE_PHRASES}"
_pcount=${#_phrases[@]}
_per=$(( (POS_SAMPLES + _pcount - 1) / _pcount ))
log "[1] Positive synthetisieren: ${_pcount} Schreibweise(n) × ~${_per} = ${POS_SAMPLES} gesamt (batch ${BATCH_SIZE}) …"
pos_n=0
_idx=0
for _phrase in "${_phrases[@]}"; do
  _idx=$((_idx + 1))
  POS_RAW="${RAW_DIR}/positives-${_idx}"
  rm -rf "${POS_RAW}"
  log "[1.${_idx}] '${_phrase}' × ${_per} …"
  synth_phrase "${_phrase}" "${_per}" "${POS_RAW}"
  _n="$(resample_dir "${POS_RAW}" "${OUT_POS}" "pos${_idx}")"
  log "[1.${_idx}] → ${_n} WAV (Prefix pos${_idx}_)"
  pos_n=$((pos_n + _n))
done
log "Positive fertig: ${pos_n} WAV in ${OUT_POS}"

# -----------------------------------------------------------------------------
# TODO[REAL] — echte Haushalts-Aufnahmen einklinken (HÖCHSTE Priorität, hardware-step)
# -----------------------------------------------------------------------------
# Echte "Hey Hoshi"-Clips aus recordings/ sind der Goldstandard (euer Raum/Mic).
# Sie ersetzen NICHTS, sie ERGÄNZEN den synthetischen Pool. Sobald the maintainer+Familie
# aufgenommen haben (Anleitung: recordings/README.md), hier einkommentieren —
# REIN-Resample auf 16k mono, in DENSELBEN positives/-Ordner:
#
#   if compgen -G "${SCRIPT_DIR}/recordings/*.wav" > /dev/null; then
#     log "[REAL] Echte Aufnahmen resampeln → ${OUT_POS} …"
#     real_n="$(resample_dir "${SCRIPT_DIR}/recordings" "${OUT_POS}" "real")"
#     log "Echte Positive: ${real_n}"
#   fi
#   # Echte Hard-Negative-Aufnahmen (recordings/negatives/) analog → ${OUT_NEG} (prefix realneg).
echo ">> TODO[REAL]: echte Aufnahmen aus recordings/ einklinken, sobald vorhanden (s. recordings/README.md)."

# -----------------------------------------------------------------------------
# TODO[DE-VOICES] — deutsche Aussprache zumischen (großer Download → NICHT im Default)
# -----------------------------------------------------------------------------
# Der EN-LibriTTS-Generator ist English-only; "Hey Hoshi" trägt lautlich trotzdem.
# Für echte DE-Aussprache zusätzliche normale Piper-Voices (.onnx + .onnx.json) laden
# (de_DE-Katalog: huggingface.co/rhasspy/piper-voices) und durchrotieren. Pro Voice
# ~60–120 MB → bewusst NICHT automatisch gezogen. Wenn geladen:
#
#   for v in "${PSG_DIR}/models/de_DE-thorsten-medium.onnx" \
#            "${PSG_DIR}/models/de_DE-kerstin-low.onnx"; do
#     GEN_MODEL="${v}" synth_phrase "${WAKE_PHRASE}" 300 "${RAW_DIR}/positives_de_$(basename "$v" .onnx)"
#     resample_dir "${RAW_DIR}/positives_de_$(basename "$v" .onnx)" "${OUT_POS}" "posde"
#   done
echo ">> TODO[DE-VOICES]: optional de_DE-Piper-Voices zumischen (manueller Download, s. README §2)."

# -----------------------------------------------------------------------------
# [2] HARD-NEGATIVES synthetisieren → 22k roh → 16k mono
# -----------------------------------------------------------------------------
if [ "${NEG_PER_PHRASE}" -gt 0 ]; then
  log "[2] Hard-Negatives synthetisieren (${NEG_PER_PHRASE}/Phrase) aus $(basename "${HARD_NEG_FILE}") …"
  while IFS= read -r phrase || [ -n "${phrase}" ]; do
    # leere Zeilen + Kommentare (#) überspringen
    phrase="${phrase%%$'\r'}"                       # evtl. CR entfernen
    case "${phrase}" in ''|'#'*) continue ;; esac
    # führende/abschließende Spaces trimmen
    phrase="$(printf '%s' "${phrase}" | sed -e 's/^[[:space:]]*//; s/[[:space:]]*$//')"
    [ -n "${phrase}" ] || continue
    slug="$(slugify "${phrase}")"
    neg_raw="${RAW_DIR}/neg_${slug}"
    rm -rf "${neg_raw}"
    log "   • '${phrase}' → ${NEG_PER_PHRASE} Samples"
    synth_phrase "${phrase}" "${NEG_PER_PHRASE}" "${neg_raw}"
    resample_dir "${neg_raw}" "${OUT_NEG}" "neg_${slug}" >/dev/null
  done < "${HARD_NEG_FILE}"
  neg_total="$(find "${OUT_NEG}" -name '*.wav' | wc -l | tr -d ' ')"
  log "Hard-Negatives fertig: ${neg_total} WAV in ${OUT_NEG}"
else
  log "[2] Hard-Negatives übersprungen (NEG_PER_PHRASE=0)."
fi

# -----------------------------------------------------------------------------
# Aufräumen + Bilanz
# -----------------------------------------------------------------------------
RM_RAW="${RM_RAW:-1}"                      # RM_RAW=0 behält die 22k-Roh-WAVs
[ "${RM_RAW}" = "1" ] && rm -rf "${RAW_DIR}"

log "FERTIG."
printf '   Positives : %s\n' "$(find "${OUT_POS}" -name '*.wav' 2>/dev/null | wc -l | tr -d ' ')"
printf '   Negatives : %s\n' "$(find "${OUT_NEG}" -name '*.wav' 2>/dev/null | wc -l | tr -d ' ')"
printf '   Format    : %s Hz mono PCM16\n' "${TARGET_RATE}"
printf '   Ablage    : %s\n' "${OUT_ROOT}"
echo  "   Nächster Schritt: Augmentation + Training im microWakeWord-Trainer →"
echo  "                     wake/train/TRAIN-RUNBOOK.md  (NICHT hier; externe GPU/Colab)."
