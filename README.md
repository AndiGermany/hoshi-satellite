# Hoshi Satellite — custom firmware for the Home Assistant Voice PE

Custom ESPHome firmware that turns a **Home Assistant Voice Preview
Edition** (ESP32-S3 + XMOS far-field audio front-end) into a **direct**
voice satellite for a self-hosted assistant backend — no Home Assistant in
the audio path, wake word and audio streaming go straight to your own
server over a `wss://.../ws/audio` WebSocket.

---

## Die drei Repos · The three repositories

| | |
|---|---|
| [**hoshi-0.8**](https://github.com/AndiGermany/hoshi-0.8) | Das Backend, mit dem diese Firmware spricht: Assistent, Oberfläche, Sidecars, Wire-Protokoll · *The backend this firmware talks to* |
| **hoshi-satellite** *(hier · here)* | Die Firmware auf dem Gerät · *The firmware on the device* |
| [**collab-os**](https://github.com/AndiGermany/collab-os) | Das Making-of: wie eine Person mit KI-Agenten das gebaut hat, Fehlschläge inklusive · *How it was built, mistakes included* |

Ohne das Backend ist diese Firmware ein Gerät ohne Gegenstelle — der
`ws/audio`-Vertrag ist dort dokumentiert.

---

## EN TL;DR

- **Hardware:** [Home Assistant Voice Preview Edition](https://www.home-assistant.io/voice-pe/)
  — an ESP32-S3 board with an XMOS XU316 far-field mic front-end, a
  speaker, an LED ring, a center button, and a physical mute switch.
- **What the firmware does:** replaces the stock `voice_assistant`+`api`
  components (which bind the device to Home Assistant) with on-device wake
  word detection (`micro_wake_word`) plus a custom bidirectional WebSocket
  client (`hoshi_ws_audio`) that streams audio directly to your own
  backend and plays back whatever it returns. The XMOS audio front-end,
  I2S buses, DAC, and LED hardware are reused byte-for-byte from upstream —
  only the "who does the device talk to" layer changes. Half-duplex (no
  acoustic echo cancellation on this board): the mic mutes while the
  speaker plays.
- **You need your own backend.** This repo is the device side only. Your
  server needs to implement the WebSocket protocol documented in
  [`docs/PROTOCOL.md`](docs/PROTOCOL.md) (`start` → binary WAV chunks →
  `stop`, JSON downlink with `transcript`/`llm_audio`/`llm_done`/etc.) —
  see [Server-side](#server-side) below.
- **Flashing is real hardware work with real (if low) brick risk.** Read
  [`firmware/RUNBOOK.md`](firmware/RUNBOOK.md) before you touch a device —
  it has the exact sequence and the gotchas that cost real debugging time
  to find. The two warnings that matter most:
  - **After any flash, let the device boot completely undisturbed for
    about 60 seconds.** Don't power-cycle it, don't open a serial monitor.
    ESPHome's safe-mode watchdog counts fast resets; interrupting an early
    boot can push it into a boot loop that looks like a dead device but
    isn't.
  - **The ESP32-S3's USB-Serial/JTAG interface dies the moment the
    firmware crashes or hangs** — so a USB serial monitor is useless for
    debugging exactly the moments you need it most. Once the device is on
    WiFi, use `esphome logs ... --device <device>.local` instead. If the
    device becomes completely unresponsive, recovery is a **wired USB
    flash of your backed-up stock firmware** (bootloader mode: unplug
    power → hold the center button → plug in USB-C) — see
    [`firmware/recovery/README.md`](firmware/recovery/README.md). Back up
    your stock firmware **before** your first custom flash; that backup is
    the whole reason this recovery path exists.

Start here: [`firmware/README.md`](firmware/README.md) (build recipe) →
[`firmware/RUNBOOK.md`](firmware/RUNBOOK.md) (exact flash sequence) →
[`docs/PROTOCOL.md`](docs/PROTOCOL.md) (the wire contract your backend
needs to speak).

---

## Was das hier ist

Dieses Repo enthält eine **Custom-Firmware** für die **Home Assistant
Voice Preview Edition** (kurz "Voice PE") — die offizielle ESP32-S3-
Sprachhardware von Home Assistant. Statt die Stock-Firmware zu benutzen
(die das Gerät fest an Home Assistant als Conversation-Agent bindet), läuft
hier eine eigene ESPHome-Firmware, die das Gerät **direkt** mit einem
selbstgehosteten Assistenz-Backend sprechen lässt — ganz ohne Home
Assistant im Audio-Pfad.

Das Gerät bringt ein sehr gutes Stück Hardware mit: einen ESP32-S3 als
Hauptprozessor, ein **XMOS XU316** als Far-Field-Audio-Frontend (Mic-Array-
Cleanup, AGC), einen Lautsprecher, einen LED-Ring, einen Center-Button und
einen physischen Mute-Schalter. Diese Firmware behält den kompletten
Audio-Stack (XMOS, I2S, DAC, LED-Hardware) **byte-für-byte** von der
Upstream-Firmware bei — verändert wird nur, **mit wem** das Gerät spricht.

### Was die Firmware tut

- **On-device Wake-Word-Erkennung** über ESPHomes `micro_wake_word`
  (Stock-Modell als Default; ein eigenes trainiertes Wake-Word ist ein
  optionaler, separater Schritt — siehe [`tools/wake/README.md`](tools/wake/README.md)).
- **Direkter WebSocket-Client** (`hoshi_ws_audio`, eigene ESPHome-External-
  Component) zu eurem Backend: sendet Mikrofon-Audio als WAV in
  Chunks, empfängt Transkript/LLM-Antwort/TTS-Audio zurück und spielt es
  über den Lautsprecher ab.
- **Half-Duplex** (kein Barge-in per Sprache): weil dieses Board keine
  akustische Echo-Unterdrückung hat, wird das Mikrofon stummgeschaltet,
  während die Antwort abgespielt wird. Unterbrechen geht über den
  Center-Button (`abort`).
- **TLS via Leaf-Pinning:** das selbstsignierte Zertifikat eures eigenen
  Servers wird direkt in die Firmware eingebettet als Trust-Anchor (Trust-
  On-First-Use) — kein CA-Overhead nötig für ein einzelnes, festes
  LAN-Backend. Siehe [`docs/DECISIONS.md`](docs/DECISIONS.md) (D3) für die
  Begründung.
- **LED-Feedback** für den kompletten Turn-Zyklus (Listening/Thinking/
  Speaking/Error), plus optionale Erweiterungen wie Nachtmodus-Dimmung,
  Timer-Countdown-Ring und Sprecher-Akzentfarben, falls euer Backend diese
  optionalen Downlink-Frames sendet — siehe [`docs/PROTOCOL.md`](docs/PROTOCOL.md) §5.

Der genaue Grund für jede dieser Entscheidungen (warum flashen statt
No-Flash-Bridge, warum Half-Duplex, warum Leaf-Pinning, warum eine
einfache Energie-VAD statt einer neuronalen) steht in
[`docs/DECISIONS.md`](docs/DECISIONS.md); die Architektur (vier strikt
getrennte Concurrency-Kontexte, damit das Firmware-Netzwerk-I/O nie den
Main-Loop blockiert) steht in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

### Ehrlicher Status

Diese Firmware **kompiliert und läuft** gegen die Referenz-Deployment, für
die sie geschrieben wurde (fünf reale Crash-/Hang-Bugs auf echter Hardware
gefunden und gefixt — siehe `docs/DECISIONS.md`/`docs/ARCHITECTURE.md`).
Sie wurde **nicht** gegen ein zweites, unabhängiges Backend oder ein
zweites physisches Gerät verifiziert. Behandelt die Config-Defaults (VAD-
Schwellen, Gain, Timeouts) als Startpunkt zum Tunen an eurem eigenen Raum
und eurer eigenen Hardware, nicht als Gospel.

## Flash-Anleitung — die Kurzfassung mit den wichtigen Warnungen

**Vollständige Anleitung mit allen Gates:** [`firmware/RUNBOOK.md`](firmware/RUNBOOK.md).
Hier nur die Kurzfassung plus die zwei Warnungen, die am meisten Zeit
sparen, wenn man sie vorher kennt:

1. **Zuerst Backup + Recovery-Übung**, bevor irgendein Custom-Byte aufs
   Gerät kommt — [`firmware/recovery/README.md`](firmware/recovery/README.md).
   Ein voller Flash-Dump der Stock-Firmware ist der garantierte Rückweg.
2. `secrets.yaml.example` → `secrets.yaml` kopieren, eigene WiFi-Zugangsdaten
   (**nur 2,4 GHz**, kein 5 GHz), Hoshi-API-Token und optional ein
   OTA-Passwort eintragen. `secrets.yaml` ist gitignored — niemals committen.
3. Das Platzhalter-Leaf-Zertifikat in
   `firmware/esphome/components/hoshi_ws_audio/hoshi_ws_audio.cpp`
   (`SERVER_LEAF_PEM`) durch das Zertifikat eures eigenen Servers ersetzen.
4. `esphome compile` muss grün sein, **bevor** geflasht wird.
5. Erster Flash über USB-C (`esphome run hoshi-voice-pe.yaml`), danach OTA.

**⚠️ Nach JEDEM Flash: das Gerät ca. 60 Sekunden lang ungestört booten
lassen.** Nicht anfassen, nicht neu starten, keinen Serial-Monitor öffnen,
der einen Reset auslöst. ESPHomes Safe-Mode zählt schnelle Resets — ein
gestörter früher Boot kann das Gerät in eine Boot-Loop schicken, die wie
ein totes Gerät aussieht (App startet nie, nichts reagiert, kein Wake, kein
Log), aber eigentlich nur der übervorsichtige Safe-Mode ist. Die Log-Zeile
**"Boot seems successful; resetting boot loop counter"** ist die
Bestätigung, dass es stabil läuft.

**⚠️ USB-Serial/JTAG stirbt bei einem Crash.** Die USB-Serial/JTAG-
Schnittstelle des ESP32-S3 verschwindet in dem Moment, in dem die Firmware
abstürzt, hängt oder resettet — ein USB-Serial-Monitor ist also genau in
den Momenten nutzlos, in denen man ihn am meisten braucht. Sobald das Gerät
im WLAN ist: `esphome logs hoshi-voice-pe.yaml --device hoshi-voice-pe.local`
verwenden — das verbindet sich über Resets hinweg automatisch neu. Wird das
Gerät komplett unresponsive, ist der Rückweg ein **kabelgebundener
USB-Flash des zuvor gesicherten Stock-Backups** (Bootloader-Modus: Strom
trennen → Center-Button halten → USB-C einstecken → kurz halten) — siehe
[`firmware/recovery/README.md`](firmware/recovery/README.md).

## Server-side

Dieses Repo enthält **nur die Geräte-Seite**. Ihr braucht ein eigenes
Backend, das den in [`docs/PROTOCOL.md`](docs/PROTOCOL.md) dokumentierten
`/ws/audio`-Wire-Contract spricht: `{type:"start", mimeType:"audio/wav",
turnId}` → WAV-Chunks (≤16 KB) → `{type:"stop"}` uplink, und
`transcript`/`llm_delta`/`llm_audio`/`llm_done`/`llm_error` als JSON-
Downlink-Frames. Das Protokoll ist bewusst einfach gehalten und
framework-agnostisch — jedes Backend, das STT → LLM → TTS über genau diese
Frames anbietet, funktioniert mit dieser Firmware. Ein Python-Referenz-
Client, der denselben Contract spricht, liegt unter
[`tools/bridge/napi_bridge.py`](tools/bridge/napi_bridge.py) (als
No-Flash-Alternative, die ein Stock-Gerät per ESPHome-Native-API anbindet).

## Repo-Layout

```
firmware/
  README.md                    <- Build-Rezept, Phasen, Zielbild
  RUNBOOK.md                   <- exakte Flash-Sequenz + Gates + gelöste Bugs
  TESTPLAN.md                  <- Ein-Flash-Abnahmetest (Boot/LED/Turn/Streaming)
  recovery/README.md           <- Stock-Firmware-Backup + Recovery
  esphome/
    hoshi-voice-pe.yaml           <- die Firmware-Config
    secrets.yaml.example          <- nach secrets.yaml kopieren, ausfüllen
    components/hoshi_ws_audio/    <- die Custom-WS-Audio-Component (C++/Python)
docs/
  PROTOCOL.md                  <- exakter /ws/audio-Wire-Contract (Autorität)
  ARCHITECTURE.md              <- die 4-Kontext-Concurrency-Architektur
  DECISIONS.md                 <- warum diese Firmware so gebaut ist, wie sie ist
  HARDWARE.md                  <- Zielhardware, Pin-Belegung
  IDEAS.md                     <- Feature-Roadmap
  MEASUREMENTS.md               <- was/wie zu messen ist, bevor man etwas behauptet
tools/
  bridge/                      <- No-Flash-Alternative (ESPHome-Native-API-Bridge)
  wake/                        <- eigenes Wake-Word trainieren (optional)
```

## Lizenz

Für dieses Release ist noch keine Lizenzdatei festgelegt — bis dahin gilt
der Code als "alle Rechte vorbehalten". Fragen dazu bitte an das Projekt.
