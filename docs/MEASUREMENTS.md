# MEASUREMENTS.md — measure before you claim it

> No estimated number is good enough to gate a decision on. Measure it
> yourself, in your own room, on your own hardware.

## Required measurements (single-room pilot)

| Metric | How | Gate |
|---|---|---|
| **Wake -> first audible response** | 10 turns, report **median + P95** (not just the mean) | a real listening test, not a log timestamp diff alone |
| False wake rate | count over a real day of normal use (false wakes/hour) | in-room, passive |
| Speech-recognition accuracy | a fixed set of test sentences in your target language, A/B across changes | judged by ear |
| Barge-in coherence | speak while TTS is playing -> does the response cut off cleanly? | in-room |

## Reference latency budget (rough estimate — NOT a gate)

Wake (on-device) ~0.1-0.5s · STT ~0.5-1.5s · first LLM sentence ~1-3s ·
first TTS chunk ~0.4-0.5s · transport ~0.2-0.5s -> **roughly 2.5-5s total**.
Treat this only as a sanity-check ballpark — your own pilot measurement is
the number that actually matters, and will differ based on your backend's
models, hardware, and network.

## Template for each measurement session

```
date / firmware commit / LLM model+host / TTS model+host / network (LAN, server IP)
raw: t1..t10 (ms), median, p95
listening-test notes
verdict: good enough to keep using / needs tuning / reconsider this approach
```

> Note anything that drifts between sessions (server IP via DHCP, backend
> version, firmware version) — a "regression" that's actually just a
> different endpoint is a common false alarm.
