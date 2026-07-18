#!/usr/bin/env bash
# Cloud-TTS wake-phrase mix-in samples for microWakeWord (small refinement pass).
# Reads the API key ONLY at runtime from ~/.hoshi/openai.key, never prints it.
# Sends only the phrase text (nothing sensitive). Output: 16 kHz mono PCM16 WAV.
# Note: a cloud TTS API is a small mix-in only (~10 voices -> overfit risk if
#   used as the main source). The foundation stays piper-sample-generator
#   (diversity) + real recordings.
set -uo pipefail

KEYFILE="$HOME/.hoshi/openai.key"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${OUT:-${SCRIPT_DIR}/generated-samples/openai-v0}"
POS="$OUT/positives"; NEG="$OUT/hard-negatives"; RAW="$OUT/.raw"
MODEL="gpt-4o-mini-tts"
API="https://api.openai.com/v1/audio/speech"
WAKE_PHRASE="${WAKE_PHRASE:-Hey Hoshi}"

mkdir -p "$POS" "$NEG" "$RAW"
[ -s "$KEYFILE" ] || { echo "ERROR: $KEYFILE missing/empty"; exit 1; }
KEY="$(tr -d ' \t\r\n' < "$KEYFILE")"
[ -n "$KEY" ] || { echo "ERROR: key is empty"; exit 1; }

gen() { # $1=text $2=voice $3=instructions $4=out.wav
  local text="$1" voice="$2" instr="$3" out="$4"
  local raw="$RAW/tmp.wav" code body
  body="$(jq -n --arg m "$MODEL" --arg i "$text" --arg v "$voice" --arg ins "$instr" \
        '{model:$m,input:$i,voice:$v,instructions:$ins,response_format:"wav"}')"
  code=$(curl -sS -w "%{http_code}" -o "$raw" "$API" \
        -H "Authorization: Bearer $KEY" -H "Content-Type: application/json" -d "$body")
  if [ "$code" = "200" ]; then
    ffmpeg -nostdin -loglevel error -y -i "$raw" -ar 16000 -ac 1 -sample_fmt s16 "$out" \
      && { echo "ok  $out"; return 0; } || { echo "ERROR ffmpeg $out"; return 1; }
  else
    echo "ERROR http=$code voice=$voice text=\"$text\": $(head -c 160 "$raw" | tr -d '\n')"; return 1
  fi
}

# --- Validation (1 call) ---
if ! gen "$WAKE_PHRASE" "alloy" "Neutral, clear." "$POS/_probe_alloy.wav"; then
  echo "ABORT: first call failed (invalid key / model / network?)"; rm -rf "$RAW"; exit 2
fi

VOICES=(alloy ash ballad coral echo fable nova onyx sage shimmer)
p=0
for v in "${VOICES[@]}"; do
  gen "$WAKE_PHRASE" "$v" "Natural, everyday, neutral delivery."        "$POS/pos_${v}_neutral.wav" && p=$((p+1))
  gen "$WAKE_PHRASE" "$v" "Quick and casual, said in passing." "$POS/pos_${v}_casual.wav"  && p=$((p+1))
done

# Near-miss negatives (for hard-negative augmentation) — edit these to be
# close acoustic neighbors of your own wake phrase.
n=0
for ph in "Hey Joshi" "Hey Sushi" "Hey Yoshi" "Hi Hoshi" "Okay Hoshi"; do
  for v in alloy nova onyx; do
    safe="$(echo "$ph" | tr ' ' '_')"
    gen "$ph" "$v" "Neutral." "$NEG/neg_${safe}_${v}.wav" && n=$((n+1))
  done
done

rm -rf "$RAW"
echo "=== DONE ==="
echo "Positives: $(ls -1 "$POS" 2>/dev/null | wc -l | tr -d ' ') | Hard negatives: $(ls -1 "$NEG" 2>/dev/null | wc -l | tr -d ' ')"
echo "Location: $OUT"
