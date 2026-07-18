#!/usr/bin/env bash
# =============================================================================
# generate-samples.sh — wake-word sample generator (TURNKEY)
# =============================================================================
#
#   What this script does (all LOCAL, all FREE, no cloud calls):
#     1) Synthesize POSITIVES  — your wake phrase via piper-sample-generator
#                                 (en_US-libritts_r-medium, multi-speaker).
#     2) Synthesize HARD NEGATIVES — phrases from ./hard-negatives.txt.
#     3) RESAMPLE to 16 kHz mono PCM16 — the microWakeWord/ESPHome standard —
#                                 into a structured output layout.
#
#   Output layout (under $OUT_ROOT, default ./generated-samples/dataset-v1/):
#       positives/   <- synthetic wake-phrase samples, 16 kHz mono PCM16
#       negatives/   <- synthetic hard negatives, 16 kHz mono PCM16
#     (real recordings are added separately — see the TODO[REAL] block below
#      and write your own recordings/README.md)
#
#   Doctrine: synthesis = volume (cheap, local, free). Real recordings = quality.
#             Cloud TTS is NOT a volume source (only ~10 voices -> overfit risk).
#             Augmentation (background/RIR/gain/pitch) belongs in the TRAINER,
#             NOT in this resample step — see train/TRAIN-RUNBOOK.md.
#
#   ---------------------------------------------------------------------------
#   PREREQUISITES (set these up once — see SETUP-LOG.md for a worked example):
#     - a venv under ./.venv with piper-sample-generator installed
#     - the generator model at ./piper-sample-generator/models/en_US-libritts_r-medium.pt
#       (REQUIRED to be exactly the expected size — the size check below aborts otherwise)
#     - ffmpeg in PATH (for clean, augmentation-free 16k resampling)
#   If a prerequisite is missing, the script reports it clearly and aborts
#   before generating anything. No large downloads happen by default.
#
#   USAGE:
#     bash generate-samples.sh                 # default: 200 positives + negatives
#     POS_SAMPLES=1000 bash generate-samples.sh
#     POS_SAMPLES=2000 NEG_PER_PHRASE=40 OUT_ROOT=./generated-samples/dataset-v2 \
#                                              bash generate-samples.sh
#     SMOKE=1 bash generate-samples.sh         # 10 positives, no negatives (smoke test)
#
#   Sources:
#     - github.com/rhasspy/piper-sample-generator
#     - github.com/OHF-Voice/micro-wake-word        (formerly kahrendt/microWakeWord)
#     - esphome.io/components/micro_wake_word/
# =============================================================================

set -euo pipefail

# -----------------------------------------------------------------------------
# Configuration (all overridable via env)
# -----------------------------------------------------------------------------
WAKE_PHRASE="${WAKE_PHRASE:-Hey Hoshi}"   # a 3+ syllable phrase deliberately chosen for lower FAR than a shorter one

# Directories — relative to the script; nothing lands outside of wake/.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_PY="${VENV_PY:-${SCRIPT_DIR}/.venv/bin/python}"
PSG_DIR="${PSG_DIR:-${SCRIPT_DIR}/piper-sample-generator}"
GEN_MODEL="${GEN_MODEL:-${PSG_DIR}/models/en_US-libritts_r-medium.pt}"
GEN_MODEL_BYTES="${GEN_MODEL_BYTES:-204089915}"  # required size of GEN_MODEL (env-overridable for a different model); catches truncated downloads
HARD_NEG_FILE="${HARD_NEG_FILE:-${SCRIPT_DIR}/hard-negatives.txt}"

OUT_ROOT="${OUT_ROOT:-${SCRIPT_DIR}/generated-samples/dataset-v1}"
OUT_POS="${OUT_ROOT}/positives"
OUT_NEG="${OUT_ROOT}/negatives"
RAW_DIR="${OUT_ROOT}/.raw22k"             # intermediate WAVs @22050 Hz before the 16k resample

# Volumes — a MODERATE default, sequential (respect the RAM ceiling on a laptop-class machine).
POS_SAMPLES="${POS_SAMPLES:-200}"         # total synthetic positives (1000-5000 for a reasonably robust model)
NEG_PER_PHRASE="${NEG_PER_PHRASE:-20}"    # synthetic negatives per hard-negative phrase
BATCH_SIZE="${BATCH_SIZE:-10}"            # RAM-friendly on MPS/CPU (see SETUP-LOG.md: 5-25 is fine, lower on OOM)
TARGET_RATE="${TARGET_RATE:-16000}"       # microWakeWord standard

# Diversity knobs (English generator). More speakers/tempo = a broader foundation.
MAX_SPEAKERS="${MAX_SPEAKERS:-700}"       # < 904
LENGTH_SCALES="${LENGTH_SCALES:-0.9 1.0 1.1}"   # speaking tempo, rotated per batch -> covers the tempo diversity axis
SLERP_WEIGHTS="${SLERP_WEIGHTS:-0.0 0.25 0.5 0.75 1.0}"  # speaker blending

# SMOKE=1 -> a tiny smoke-test run (10 positives, no negatives).
SMOKE="${SMOKE:-0}"
if [ "${SMOKE}" = "1" ]; then
  POS_SAMPLES=10
  NEG_PER_PHRASE=0
  BATCH_SIZE=5
  echo ">> SMOKE mode: ${POS_SAMPLES} positives, no negatives."
fi

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------
log()  { printf '\n>> %s\n' "$*"; }
die()  { printf '\nERROR: %s\n' "$*" >&2; exit 1; }

# Build a filename-safe slug from a phrase (lowercase, non-ascii folded, _ for spaces).
slugify() {
  printf '%s' "$1" \
    | tr '[:upper:]' '[:lower:]' \
    | sed -e 's/ä/ae/g; s/ö/oe/g; s/ü/ue/g; s/ß/ss/g' \
    | tr -c 'a-z0-9' '_' \
    | sed -e 's/__*/_/g; s/^_//; s/_$//'
}

# Resample ONE directory (22k WAV) -> 16 kHz mono PCM16 into $2, with prefix $3.
# PURE resample (ffmpeg) — NO augmentation. Augmentation is the trainer's job.
# (Note: piper_sample_generator.augment also resamples, but it additionally
#  adds gain+IR — we do NOT want that here, to keep the raw positives clean.)
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

# Generate $count positives of a phrase into $outdir (raw, 22k).
synth_phrase() {
  local phrase="$1" count="$2" outdir="$3"
  [ "${count}" -gt 0 ] || return 0
  mkdir -p "${outdir}"
  # IMPORTANT (see SETUP-LOG.md §1): must run from the repo root, or you'll hit
  # ModuleNotFoundError for piper_train.
  ( cd "${PSG_DIR}" && "${VENV_PY}" -m piper_sample_generator "${phrase}" \
      --model         "${GEN_MODEL}" \
      --max-samples   "${count}" \
      --batch-size    "${BATCH_SIZE}" \
      --max-speakers  "${MAX_SPEAKERS}" \
      --length-scales ${LENGTH_SCALES} \
      --slerp-weights ${SLERP_WEIGHTS} \
      --noise-scales  0.667 \
      --noise-scale-ws 0.8 \
      --output-dir    "${outdir}/" )
}

# -----------------------------------------------------------------------------
# [0] Check prerequisites (aborts BEFORE any generation if something's missing)
# -----------------------------------------------------------------------------
log "Checking prerequisites..."
[ -x "${VENV_PY}" ]      || die "venv python missing: ${VENV_PY} (see SETUP-LOG.md §1/§2)."
[ -d "${PSG_DIR}" ]      || die "piper-sample-generator missing: ${PSG_DIR} (see SETUP-LOG.md §2)."
command -v ffmpeg >/dev/null 2>&1 || die "ffmpeg missing from PATH (e.g. brew install ffmpeg)."
[ -f "${GEN_MODEL}" ]    || die "generator model missing: ${GEN_MODEL}
  -> curl -fL -o '${GEN_MODEL}' \\
      'https://github.com/rhasspy/piper-sample-generator/releases/download/v2.0.0/en_US-libritts_r-medium.pt'
  (required: ${GEN_MODEL_BYTES} bytes. Large download — deliberately not automatic.)"
actual_bytes="$(stat -f%z "${GEN_MODEL}" 2>/dev/null || stat -c%s "${GEN_MODEL}")"
[ "${actual_bytes}" -eq "${GEN_MODEL_BYTES}" ] \
  || die "generator model is ${actual_bytes} bytes, expected ${GEN_MODEL_BYTES} (download truncated? see SETUP-LOG.md §3)."
[ "${NEG_PER_PHRASE}" -eq 0 ] || [ -f "${HARD_NEG_FILE}" ] \
  || die "hard-negatives.txt missing: ${HARD_NEG_FILE}"
log "Prerequisites OK. Target: ${OUT_ROOT}"
mkdir -p "${OUT_POS}" "${OUT_NEG}" "${RAW_DIR}"

# -----------------------------------------------------------------------------
# [1] Synthesize POSITIVES -> raw 22k -> 16k mono
# -----------------------------------------------------------------------------
log "[1] Synthesizing positives: '${WAKE_PHRASE}' x ${POS_SAMPLES} (batch ${BATCH_SIZE})..."
POS_RAW="${RAW_DIR}/positives"
rm -rf "${POS_RAW}"
synth_phrase "${WAKE_PHRASE}" "${POS_SAMPLES}" "${POS_RAW}"

log "[1b] Resampling positives -> ${TARGET_RATE} Hz mono PCM16..."
pos_n="$(resample_dir "${POS_RAW}" "${OUT_POS}" "pos")"
log "Positives done: ${pos_n} WAV in ${OUT_POS}"

# -----------------------------------------------------------------------------
# TODO[REAL] — hook in real household recordings (highest priority, needs you)
# -----------------------------------------------------------------------------
# Real wake-phrase clips from recordings/ are the gold standard (your room, your
# mic). They don't REPLACE anything, they ADD to the synthetic pool. Once you've
# recorded some (write your own recordings/README.md as a guide), uncomment
# this — pure-resample to 16k mono, into the SAME positives/ folder:
#
#   if compgen -G "${SCRIPT_DIR}/recordings/*.wav" > /dev/null; then
#     log "[REAL] Resampling real recordings -> ${OUT_POS}..."
#     real_n="$(resample_dir "${SCRIPT_DIR}/recordings" "${OUT_POS}" "real")"
#     log "Real positives: ${real_n}"
#   fi
#   # Real hard-negative recordings (recordings/negatives/) similarly -> ${OUT_NEG} (prefix realneg).
echo ">> TODO[REAL]: hook in real recordings from recordings/ once you have them (see wake/README.md §3)."

# -----------------------------------------------------------------------------
# TODO[OTHER-VOICES] — mix in other-language pronunciation (large download -> NOT by default)
# -----------------------------------------------------------------------------
# The English LibriTTS generator is English-only; a short wake phrase usually
# still carries phonetically. For pronunciation in another target language,
# load additional regular Piper voices (.onnx + .onnx.json) for that language
# (see huggingface.co/rhasspy/piper-voices) and rotate through them. ~60-120 MB
# per voice -> deliberately not fetched automatically. If loaded:
#
#   for v in "${PSG_DIR}/models/<lang>-voice-a.onnx" \
#            "${PSG_DIR}/models/<lang>-voice-b.onnx"; do
#     GEN_MODEL="${v}" synth_phrase "${WAKE_PHRASE}" 300 "${RAW_DIR}/positives_$(basename "$v" .onnx)"
#     resample_dir "${RAW_DIR}/positives_$(basename "$v" .onnx)" "${OUT_POS}" "posalt"
#   done
echo ">> TODO[OTHER-VOICES]: optionally mix in other-language Piper voices (manual download, see README §2)."

# -----------------------------------------------------------------------------
# [2] Synthesize HARD NEGATIVES -> raw 22k -> 16k mono
# -----------------------------------------------------------------------------
if [ "${NEG_PER_PHRASE}" -gt 0 ]; then
  log "[2] Synthesizing hard negatives (${NEG_PER_PHRASE}/phrase) from $(basename "${HARD_NEG_FILE}")..."
  while IFS= read -r phrase || [ -n "${phrase}" ]; do
    # skip empty lines + comments (#)
    phrase="${phrase%%$'\r'}"                       # strip a trailing CR if present
    case "${phrase}" in ''|'#'*) continue ;; esac
    # trim leading/trailing whitespace
    phrase="$(printf '%s' "${phrase}" | sed -e 's/^[[:space:]]*//; s/[[:space:]]*$//')"
    [ -n "${phrase}" ] || continue
    slug="$(slugify "${phrase}")"
    neg_raw="${RAW_DIR}/neg_${slug}"
    rm -rf "${neg_raw}"
    log "   - '${phrase}' -> ${NEG_PER_PHRASE} samples"
    synth_phrase "${phrase}" "${NEG_PER_PHRASE}" "${neg_raw}"
    resample_dir "${neg_raw}" "${OUT_NEG}" "neg_${slug}" >/dev/null
  done < "${HARD_NEG_FILE}"
  neg_total="$(find "${OUT_NEG}" -name '*.wav' | wc -l | tr -d ' ')"
  log "Hard negatives done: ${neg_total} WAV in ${OUT_NEG}"
else
  log "[2] Hard negatives skipped (NEG_PER_PHRASE=0)."
fi

# -----------------------------------------------------------------------------
# Cleanup + summary
# -----------------------------------------------------------------------------
RM_RAW="${RM_RAW:-1}"                      # RM_RAW=0 keeps the raw 22k WAVs
[ "${RM_RAW}" = "1" ] && rm -rf "${RAW_DIR}"

log "DONE."
printf '   Positives : %s\n' "$(find "${OUT_POS}" -name '*.wav' 2>/dev/null | wc -l | tr -d ' ')"
printf '   Negatives : %s\n' "$(find "${OUT_NEG}" -name '*.wav' 2>/dev/null | wc -l | tr -d ' ')"
printf '   Format    : %s Hz mono PCM16\n' "${TARGET_RATE}"
printf '   Location  : %s\n' "${OUT_ROOT}"
echo  "   Next step: augmentation + training in the microWakeWord trainer ->"
echo  "              wake/train/TRAIN-RUNBOOK.md  (not here; external GPU/Colab)."
