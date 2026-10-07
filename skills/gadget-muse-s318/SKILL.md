---
name: gadget-muse-s318
description: >-
  Use the user's Muse Gadget, a desk companion on a Waveshare ESP32-S3 1.8" AMOLED
  running the Muse Gadget SDK: read or change its volume, mute, brightness and screen
  sleep, report its battery, switch the voice it speaks in, show reactions on its avatar, offer reply buttons on its screen, show what's playing with cover art, make it say things aloud,
  play sound clips and chimes, show a picture on its screen, and scan the home
  network through it. Also use it for every push-to-talk voice note from the
  gadget (a voice-note message with an auto-transcript and one attachment named
  voice_note-<n>.wav, with no phone app device tag),
  every message ending in [tapped on the gadget], and every message containing
  [pressed on the gadget's Now Playing tile.
---

# Muse Gadget (s318)

The gadget is a small desk companion. It has a 368x448 AMOLED touch screen with
rounded corners showing an animated avatar, a speaker and microphone, and a
battery. The user holds its BOOT button to send you a voice note; your reply is
shown as captions and spoken aloud in an ElevenLabs voice. It reaches you through
Home Link, and the commands below are its tools.

## Recognising the gadget

A push-to-talk voice note from the gadget reaches you as a voice-note message
with an auto-transcript and one audio attachment named `voice_note-<n>.wav`
(e.g. `voice_note-113.wav`). It does not carry the phone app's device tag.
Treat every such message as the user talking to you through the gadget, even
when they don't mention it: your reply will be spoken by the gadget in its
voice, with its avatar on screen.

A message like `next [pressed on the gadget's Now Playing tile; player_id: …]`
is the user pressing a button on the Now Playing tile, or tapping the tile to
refresh it. It is not a conversation turn: follow "Now Playing presses" below,
and reply with only `ok`. Nothing you write on that turn is spoken or shown.

A message ending in `[tapped on the gadget]` is the user tapping one of the
reply buttons you offered with `display.options`: the words before the tag are
their reply. Treat it exactly like a voice note (your answer is spoken by the
gadget; react if it fits; offer new buttons if they fit). Don't repeat the tag.

On those turns, before you write your reply:

1. Decide whether a reaction fits what the user said (see `avatar.react`): good
   news, a joke, a kind word, a yes/no question, a weather question, and so on.
   Most turns still need none.
2. If one fits, call `avatar.react` first, with `seconds` 15. The gadget shows
   it while you think and keeps it up while your reply is spoken, which starts
   about 10 seconds after the user lets go of the button.
3. Then reply as usual, in one or two spoken sentences.

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
and `voice`: the voice's name, or its ElevenLabs ID if none has been chosen with
`voice.select` yet (`voice.select` with no parameters gives its name).

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
  `not_listed` counts any that didn't fit in the list; those can still be chosen
  by name.
- With `name`, it switches to that voice from the next reply on. Matching ignores
  case and accepts the start of a name ("George" for "George - Warm, Captivating
  Storyteller") or a voice ID.
- `not_found` and `ambiguous` errors name the candidates in their message: offer
  those to the user. `unavailable` means the gadget has no ElevenLabs key, or the
  key lacks the Voices read permission: say so. `busy` means a lookup is running:
  wait a few seconds and retry once.
- When asked which voices there are, give the short names (the part before " - "),
  not the whole descriptions.
- After switching, the gadget says hello in the new voice by itself. Don't
  announce the switch with `voice.say` as well.

### voice.say

Says `text` (1-600 bytes) aloud in the gadget's current voice, with the text as
the caption (`caption: false` hides it). It answers at once with
`{"queued": true, "position": n}`, and the gadget speaks once it's idle, after
any reply it's in the middle of. A press of its talk button cuts it off.

- Use it for announcements the user asks for from elsewhere ("tell the gadget to
  say dinner's ready"), and for reminders they set.
- When the user is talking to you through the gadget, just reply: your reply is
  already spoken. Never repeat your reply through `voice.say`.
- Keep it short, as speech: one or two sentences, no lists or links.
- `muted` means the speaker is off. With a caption, the text is shown on the
  screen instead. Tell the user it's muted. Don't unmute or raise the volume to
  be heard unless they ask.
- `unavailable`: the gadget has no ElevenLabs key. `busy`: four sounds are
  already waiting. Try again a little later.

### audio.chime

Plays a built-in sound: `ding`, `success`, `error`, `alert`, `timer`, `tada` or
`choose` (the "your turn" sound reply buttons play by themselves). Queued like
`voice.say`.

- `success` or `error` after an action the user asked for, if a sound fits.
- `timer` when a countdown the user set is up.
- `alert` for something that needs them now. `tada` for celebrations. `ding`
  for a gentle nudge.
- Don't play chimes the user didn't ask for or clearly expect.

### audio.play_url

Plays an MP3 from an `https://` URL: at most 1 MB and 60 s, which cuts longer
clips off. Queued like `voice.say`. Only use URLs the user gives you, or
well-known public sound clips. If it can't play the clip, the gadget shows
"COULDN'T PLAY THE CLIP"; you won't hear about it, so don't promise it played.

### avatar.react

Shows a reaction on the avatar for `seconds` (1-30, default 4), then it eases
back. `name` is one of: `love`, `laugh`, `excited`, `surprised`, `confused`,
`sad`, `grumpy`, `sleepy`, `nervous`, `cool`, `wink`, `yes`, `no`,
`celebrate`, and for weather `sunny`, `rainy`, `stormy`, `cold`, `windy`, `hot`,
`rainbow`. `none` clears it. It answers at once. While the gadget speaks, the
reaction shows and the mouth keeps talking, so call it just before or as you
reply.

- React sparingly: at most one reaction a reply, and not on every turn. Most
  replies need none.
- Weather answers use the matching weather reaction (rain or showers `rainy`,
  thunder `stormy`, snow or freezing `cold`, clear `sunny`, very warm `hot`,
  breezy `windy`, sun after rain `rainbow`).
- How long: on a gadget voice note, 15 seconds, so it lasts until your reply is
  spoken (see "Recognising the gadget"). From the app, 4-8 seconds; a weather
  answer 8.
- `yes` and `no` go with a clear yes or no answer. `celebrate` or `excited`
  with good news. `love` when the user says something kind. `laugh` at a joke.
- `sad`, `nervous` and `grumpy` only when they fit what the user said, never
  at the user.
- Don't narrate the reaction ("I'm showing an umbrella"), and don't react on
  the user's behalf to something they didn't share.

### display.options

Shows 2-4 reply buttons on the gadget's screen once your spoken reply ends,
with a short chime. `options` is a list of 2-4 labels, 1-24 characters each,
worded as the user's reply ("Yes, play it", "Tell me more", "No thanks"). A tap
sends that label to you as the user's next message, tagged
`[tapped on the gadget]`, and your answer is spoken. The buttons go when tapped,
when the user presses talk, or after 30 seconds. A new call replaces them.

- Offer them when your reply ends in a question with a few natural answers, a
  short choice, a confirmation, or a quiz. Most replies need none.
- On a voice note or a tapped message, call it before you finish your reply:
  the buttons wait until the speech is over.
- Keep labels short and distinct; don't offer "Other" or "Something else": the
  user can always just press talk.

### media.update

Updates the Now Playing tile: swipe right from the avatar to see it (the first
of the three page dots: Now Playing, avatar, settings). All parameters are
optional strings; a missing one leaves that field unchanged.

| Parameter | What it shows |
|---|---|
| `player` | Which player, e.g. Kitchen |
| `title` | Track title |
| `artist` | Artist name |
| `album` | Album name |
| `state` | `playing`, `paused` or `idle` (idle greys out the buttons and shows "Tap to update") |
| `player_id` | The bridge's `player_id` for that player. The tile sends it back with every press |
| `art_url` | The cover: the bridge's `image`, exactly as it gives it. Empty clears it |

- Fill it from the NAS bridge: `ma.now_playing` (or `ma.control`'s answer)
  gives `name`, `title`, `artist`, `album`, `state`, `player_id` and `image`.
  Send `image` unchanged as `art_url`: Music Assistant only serves its own sizes.
  If `image` is null (radio, a local file), send `art_url` as `""` so the last
  track's cover goes. Other null fields can be sent as null or left out.
- Don't poll. Call it after a Now Playing press, or when the user asks about
  the music or asks you to play, pause or skip.
- When nothing is playing anywhere, send only `state` as `idle`. The tile keeps
  the last track and cover, greys out its buttons and shows "Tap to update".
- It answers `{"art": "loading" | "unchanged" | "cleared" | "none"}`. The cover
  loads in the background; there's nothing to wait for.
- Don't call it just to test it.

### Now Playing presses

The tile has back, play/pause and skip buttons, and a tap anywhere else on it
asks for a refresh. Each arrives as a message:

`<action> [pressed on the gadget's Now Playing tile; player_id: <id>]`

(without `; player_id: <id>` if the tile hasn't been sent one yet).

1. For `play_pause`, `next` or `previous`: call the bridge's `ma.control` with
   that `player_id` and `action`. It answers with that player's now-playing.
2. For `refresh`: call `ma.now_playing` and find that `player_id` in its
   `players` list. With no id, pick the player that's playing, else the first
   paused one; if none is either, send only `state` as `idle`.
3. Then call `media.update` on the gadget with the result, `player_id` and
   `art_url` included.
4. Reply with only `ok`. Nothing is spoken.
5. If the bridge fails, call `voice.say` with a few words ("The Kitchen speaker
   isn't responding"), then still call `media.update` with what you know, so the
   gadget stops waiting.

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

### device.ota

Flashes new firmware over the air. It downloads a firmware `.bin` from an
`https://` URL and applies it with esp_https_ota, then reboots. Takes a
`url` and an optional `force` flag to flash even if not newer. Only use it
when the user asks and gives a firmware URL — never on your own.

## How to behave

- Change settings only when the user asks. Confirm in one short sentence
  ("Volume's at 60.").
- Don't call commands just to test them.
- Never mention, ask for or repeat API keys or tokens.
- Don't run `device.ota` unless the user asks and gives a firmware URL.

## Limits

- Settings apply at once and survive restarts.
- A new voice takes effect from the next reply, and says hello once it's chosen.
- Sounds play one after another, after any reply in progress. The talk button
  stops them.
- Reactions are the 21 above; it can't show arbitrary images as reactions (use
  `display.draw_url` for a picture).
- Reply buttons hold 2-4 labels of up to 24 characters; longer ones are refused.
- The Sir Michael Caine ElevenLabs voice goes silent on this gadget — never
  offer it as a choice.
