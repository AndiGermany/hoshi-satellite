#!/usr/bin/env bash
# fetch-de-voices.sh — lädt die 5 deutschen Piper-Stimmen (.onnx + .onnx.json) für die
# „Hey Hoshi"-Synthese (Multi-Voice-Modus von generate-samples.sh, DE-Fix 18.07).
#
# Hintergrund: de_DE-mls-medium.pt (MLS) produziert Kauderwelsch — bekanntes Problem
# der MLS-Familie (HF piper-voices Discussion #13). Ersatz: echte DE-Qualitätsstimmen.
# Quelle: https://huggingface.co/rhasspy/piper-voices (je Stimme eigene Lizenz im MODEL_CARD).
#
# Aufruf: bash fetch-de-voices.sh        # idempotent — überspringt schon geladene Stimmen
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)/piper-sample-generator/models"
BASE="https://huggingface.co/rhasspy/piper-voices/resolve/main/de/de_DE"
VOICES=(
  "thorsten/high/de_DE-thorsten-high"
  "thorsten_emotional/medium/de_DE-thorsten_emotional-medium"
  "eva_k/x_low/de_DE-eva_k-x_low"
  "karlsson/low/de_DE-karlsson-low"
  "ramona/low/de_DE-ramona-low"
)

mkdir -p "${DIR}"
for v in "${VOICES[@]}"; do
  f="$(basename "${v}")"
  onnx="${DIR}/${f}.onnx"
  json="${DIR}/${f}.onnx.json"
  if [ -f "${onnx}" ] && [ "$(stat -f%z "${onnx}")" -gt 1000000 ] && [ -f "${json}" ]; then
    echo "✓ ${f} liegt schon ($(stat -f%z "${onnx}") Bytes) — übersprungen"
    continue
  fi
  echo "→ lade ${f} …"
  curl -fL --retry 3 -o "${onnx}" "${BASE}/${v}.onnx"
  curl -fL --retry 3 -o "${json}" "${BASE}/${v}.onnx.json"
  bytes="$(stat -f%z "${onnx}")"
  [ "${bytes}" -gt 1000000 ] || { echo "FEHLER: ${f}.onnx nur ${bytes} Bytes (Download kaputt?)"; exit 1; }
  echo "✓ ${f}: ${bytes} Bytes + Config"
done
echo
echo "Alle 5 Stimmen bereit in ${DIR}"
