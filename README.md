# Screensemble Batton

ESP32 cue device for [Screensemble Conductor](https://github.com/Screensemble/Conductor):
a GPIO pin going active fires a pin event at the conductor, and the conductor's
automations decide what happens — sync-play a playlist across displays, release a
prepared performance, play one item, stop, resume, reboot. All policy lives
server-side, so behavior changes never need a reflash.

## Setup

1. Copy `include/config.h.example` → `include/config.h`, fill in WiFi.
2. `pio run -t upload`, then `pio device monitor` — it prints the device IP.
3. Open `http://<device-ip>/` — the Batton's setup page:
   - Enter the conductor URL (`http://<conductor>:8090`)
   - Press **Pair** — then open the conductor's **Battons** page and authorize it
   - Map GPIO pins to input ids (`door`, `start-button`, …) and edge (LOW/HIGH)
4. On the conductor, create an automation: **when** = this Batton's input,
   **then** = what the displays should do.

Everything set in the web UI persists in flash — a reboot keeps URL, pairing
token, and pin map.

## Firing

- Pin goes active → `POST /api/battons/<id>/event {"input":"<id>"}` with the
  device's bearer token (firmware debounces 40 ms, 1 s re-fire floor)
- Heartbeat every 60 s keeps it "online" in the Battons list
- External systems can fire the same automations via generated webhooks:
  `POST /api/hooks/<key>` (created in the conductor's automation editor)

Legacy path still works: raw `POST /api/cues/<cue-id>` with `CUE_TOKEN`
fires cue-map entries directly — no Batton required.
