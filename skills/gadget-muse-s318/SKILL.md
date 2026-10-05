---
name: gadget-muse-s318
description: >-
  Use the user's Muse Gadget, a desk companion on a Waveshare ESP32-S3 1.8" AMOLED
  running the Muse Gadget SDK: read or change its volume, mute, brightness and screen
  sleep, report its battery, switch the voice it speaks in, show a picture on its
  screen, and scan the home network through it.
---

# Muse Gadget (s318)

The gadget is a small desk companion. It has a 368x448 AMOLED touch screen with
rounded corners showing an animated avatar, a speaker and microphone, and a
battery. The user holds its BOOT button to send you a voice note; your reply is
shown as captions and spoken aloud in an ElevenLabs voice. It reaches you through
Home Link, and the commands below are its tools.

## Commands

### device.settings

Reads or changes its settings. Every parameter is optional; with none, it only
reports.

| Parameter | Values |
|---|---|
| `volume` | 0-100 |
| `muted` | `true` or `false`. Muted: replies are captioned, not spoken. |
| `brightness` | 10-100 |
| `screen_sleep_s` | 0 (never), 30, 60, 120, 300 or 600 |

It always answers with the current `volume`, `muted`, `brightness`,
`screen_sleep_s`, `battery_percent` (null with no reading), `charging`, `on_usb`
and `voice`.

- "How much battery is left?": call it with no parameters, and answer with the
  percent, adding "and charging" when `charging` is true.
- "Louder", "quieter", "a bit dimmer": read first, then step by about 15 for
  volume or 20 for brightness, staying in range.
- "Mute" or "be quiet" means `muted: true`, not volume 0. "Unmute" means
  `muted: false`.
- Screen sleep only takes the values above: pick the nearest and say which.
- A bad value comes back `invalid_params`, and nothing changes. Fix it from the
  message and retry once.

### voice.select

- With no parameters, it lists `voices` (each with a `name` and maybe a
  `category`) from the user's ElevenLabs library, and the `current` one.
  `not_listed` counts any that didn't fit.
- With `name`, it switches to that voice from the next reply on. Matching ignores
  case and accepts the start of a name ("George" for "George - Warm, Captivating
  Storyteller") or a voice ID.
- `not_found` and `ambiguous` errors name the candidates in their message: offer
  those to the user. `unavailable` means the gadget has no ElevenLabs key, or the
  key lacks the Voices read permission: say so. `busy` means a lookup is running:
  wait a few seconds and retry once.
- When asked which voices there are, give the short names (the part before " - "),
  not the whole descriptions.

### device.health

Basic health, including the battery. For a plain battery question, prefer
`device.settings`.

### display.draw_url and display.show_animation

`display.draw_url` shows a picture from a URL in place of the avatar: a baseline
JPEG, ideally 368x448, with the subject centred because the corners are
rounded. The avatar comes back with `display.show_animation`, a tap, or the
talk button. Only show pictures when the user asks for one or it clearly
helps.

### device.discover

Scans the home network (it can take up to 90 s) for devices Home Link can reach.

## How to behave

- Change settings only when the user asks. Confirm in one short sentence
  ("Volume's at 60.").
- Don't call commands just to test them.
- Never mention, ask for or repeat API keys or tokens.
- Don't run `device.ota` unless the user asks and gives a firmware URL.

## Limits

- Settings apply at once and survive restarts.
- A new voice takes effect from the next reply. It can't preview itself yet.
- The gadget can't play arbitrary sounds or show reactions yet. Don't promise
  either.
