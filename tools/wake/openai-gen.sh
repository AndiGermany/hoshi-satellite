#!/usr/bin/env bash
# OpenAI-TTS "Hey Hoshi" Beimischungs-Samples für microWakeWord (v0-Verfeinerung).
# Liest den Key NUR zur Laufzeit aus ~/.hoshi/openai.key, gibt ihn NIE aus.
# Sendet nur die Phrase (nichts Sensibles). Output: 16 kHz mono PCM16 WAV.
# Hinweis (Eda): OpenAI = kleine Beimischung (~10 Stimmen → Overfit-Gefahr).
#   Basis bleibt piper-sample-generator (Diversität) + echte Aufnahmen.
set -uo pipefail

KEYFILE="$HOME/.hoshi/openai.key"
# v1 seit 19.07 (Aussprache-Fix): deutsche Instruktionen + beide Schreibweisen
# („Hoschi" erzwingt den ʃ-Laut auch bei nicht-deutscher Lesung; „Hoshi" liest ein
# deutscher Sprecher ohnehin als „Hoschi"). openai-v0 (EN-Aussprache) bleibt liegen.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$SCRIPT_DIR/generated-samples/openai-v1"
POS="$OUT/positives"; NEG="$OUT/hard-negatives"; RAW="$OUT/.raw"
MODEL="gpt-4o-mini-tts"
API="https://api.openai.com/v1/audio/speech"

mkdir -p "$POS" "$NEG" "$RAW"
[ -s "$KEYFILE" ] || { echo "FEHLER: $KEYFILE fehlt/leer"; exit 1; }
KEY="$(tr -d ' \t\r\n' < "$KEYFILE")"
[ -n "$KEY" ] || { echo "FEHLER: Key leer"; exit 1; }

gen() { # $1=text $2=voice $3=instructions $4=out.wav
  local text="$1" voice="$2" instr="$3" out="$4"
  local raw="$RAW/tmp.wav" code body
  body="$(jq -n --arg m "$MODEL" --arg i "$text" --arg v "$voice" --arg ins "$instr" \
        '{model:$m,input:$i,voice:$v,instructions:$ins,response_format:"wav"}')"
  code=$(curl -sS -w "%{http_code}" -o "$raw" "$API" \
        -H "Authorization: Bearer $KEY" -H "Content-Type: application/json" -d "$body")
  if [ "$code" = "200" ]; then
    ffmpeg -nostdin -loglevel error -y -i "$raw" -ar 16000 -ac 1 -sample_fmt s16 "$out" \
      && { echo "ok  $out"; return 0; } || { echo "FEHLER ffmpeg $out"; return 1; }
  else
    echo "FEHLER http=$code voice=$voice text=\"$text\": $(head -c 160 "$raw" | tr -d '\n')"; return 1
  fi
}

# --- Validierung (1 Call) ---
if ! gen "Hey Hoshi" "alloy" "Neutral, klar." "$POS/_probe_alloy.wav"; then
  echo "ABBRUCH: erster Call fehlgeschlagen (Key ungültig / Modell / Netz?)"; rm -rf "$RAW"; exit 2
fi

VOICES=(alloy ash ballad coral echo fable nova onyx sage shimmer)
p=0
for v in "${VOICES[@]}"; do
  gen "Hey Hoschi" "$v" "Sprich auf Deutsch, natürlich und alltäglich, neutrale Stimmung." "$POS/pos_${v}_de_neutral.wav" && p=$((p+1))
  gen "Hey Hoschi" "$v" "Sprich auf Deutsch, schnell und beiläufig, wie nebenbei in den Raum gerufen." "$POS/pos_${v}_de_casual.wav" && p=$((p+1))
  gen "Hey Hoshi"  "$v" "Sprich auf Deutsch wie ein deutscher Muttersprachler, ruhig und freundlich." "$POS/pos_${v}_de_ruhig.wav" && p=$((p+1))
done

# Near-Miss-Negatives (für Hard-Negative-Augmentation)
NEG_PHRASES=("Hey Joshi" "Hey Sushi" "Hey Yoshi" "Hi Hoshi" "Okay Hoshi" "Hey Hoshi" )
n=0
for ph in "Hey Joshi" "Hey Sushi" "Hey Yoshi" "Hi Hoshi" "Okay Hoshi"; do
  for v in alloy nova onyx; do
    safe="$(echo "$ph" | tr ' ' '_')"
    gen "$ph" "$v" "Neutral." "$NEG/neg_${safe}_${v}.wav" && n=$((n+1))
  done
done

rm -rf "$RAW"
echo "=== FERTIG ==="
echo "Positives: $(ls -1 "$POS" 2>/dev/null | wc -l | tr -d ' ') | Hard-Negatives: $(ls -1 "$NEG" 2>/dev/null | wc -l | tr -d ' ')"
echo "Ablage: $OUT"
