# Essential Audio Enhancements for The Quarry

A mod for **The Quarry** (PC, Steam) that lets the game use **Windows spatial
sound** — Dolby Atmos (for headphones or home theater), Windows Sonic and
DTS — and **surround sound** on devices where it otherwise outputs stereo.

What the game does on its own:

- It never uses Windows spatial sound: no individually positioned sounds and
  no height channels, on any device. Its sound engine supports it, but the
  game ships with it switched off.
- On devices that report stereo hardware — most headphones, headsets and USB
  audio interfaces — it outputs stereo, even when Dolby Atmos or Windows
  Sonic is switched on. (Measured.)
- On devices that report surround hardware, such as a receiver connected by
  HDMI and set up as 5.1 or 7.1, it very likely outputs channel surround
  already, because its sound engine follows the device's hardware layout.
  (Not tested with such a device.)
- Dialogue is placed on the speaking character, but every voice is spread
  over most of the space around you (75% close up, all of it further away)
  and is no quieter far away than close up. So you cannot tell where a voice
  comes from or how far away it is. (Read from the game's sound data, and
  measured in game.)

With this mod:

- **With spatial sound switched on in Windows,** the game sends Windows a 7.1.4
  mix (the usual seven speakers plus four overhead) and individually
  positioned sounds, so Atmos, Sonic or DTS can place each sound around you,
  including behind you and above you. This is the main gain for headphone
  users and for Atmos or DTS:X home theaters alike.
- **When Windows mixes your device wider than its hardware** (for example 7.1
  with Atmos or Sonic on a stereo device), the game outputs that surround
  layout and pans sounds into it.
- **On plain stereo headphones or speakers,** nothing changes unless you ask
  for headphone panning or mono.

It also offers **positional dialogue** (each voice comes from its character,
as seen from the camera, and gets quieter and more muffled with distance),
mono output and a choice of output device, and it works alongside other mods,
including UE4SS-based ones.

## Requirements

- The Quarry, Steam version for Windows (tested September 2026).
- Windows 10 or 11.
- For spatial sound: spatial sound switched on for your output device in
  Windows (Settings, System, Sound, your device, Spatial sound). Windows
  Sonic for Headphones is free; Dolby Atmos for Headphones needs the Dolby
  Access app.

## Installing

1. Download `X3DAudio1_7.dll` and `QuarryEssentialAudio.ini` from the latest release.
2. Open the game folder, then `SMG026\Binaries\Win64` — the folder that
   contains `TheQuarry-Win64-Shipping.exe`. In Steam: right-click The Quarry,
   Manage, Browse local files.
3. Copy both files there.
4. Start the game as usual.

## Settings

Open `QuarryEssentialAudio.ini` in a text editor. Restart the game after changing it.

There are four settings.

`Output=` sets how the game's sound is laid out:

- `auto` (the default) — 3D spatial sound if Windows spatial sound is switched
  on for the device; otherwise the channel layout Windows mixes the device at,
  when that is wider than what the device reports as its hardware (7.1 with
  Atmos or Sonic on); otherwise the game's own output.
- `spatial` — 3D spatial sound: a 7.1.4 mix plus individually positioned
  sounds. This covers every Windows spatial format — Dolby Atmos for
  Headphones, Dolby Atmos for home theater, Windows Sonic, DTS — because
  Windows, not the game, turns the sound into what your headphones or receiver
  needs. Falls back to what `auto` would choose if spatial sound is off.
- `7.1`, `5.1` or `quad` — that speaker layout. `5.1` means front left and
  right, centre, LFE and two side speakers, as most receivers use. With Atmos
  or Sonic on, `7.1` gives you Atmos or Sonic rendering a 7.1 channel mix
  instead of positioned sounds.
- `stereo` — stereo, even on a surround device.
- `headphones` — 3D spatial sound if it is switched on; otherwise stereo with
  the game's sound engine set to headphone panning.
- `mono` — the game mixes everything to one channel, played on both sides, so
  no sound is lost to one ear. Unlike Windows' own Mono audio setting, it
  affects only the game, not other programs.

Before using a layout it has made up (`7.1`, `5.1`, `quad` or `stereo`), the
mod asks Windows whether it accepts that layout on your device. If Windows says
no, the mod says so in the log and uses the next best choice, so a wrong
setting cannot leave the game silent.

`Compression=` is `on` (the default) or `off`. The game runs its whole mix
through a compressor on the way out (3:1 above −20 dB), which narrows the
difference between loud and quiet moments. `off` removes it, for the game's
full dynamic range. The limiter after it, which stops clipping, stays, and
dialogue keeps its own separate levelling. The game's compression is gentle,
so the difference is subtle.

`Device=` sets which output the game plays on. Leave it empty for the Windows
default. Otherwise write part of the device's name, for example `iD4` or
`Headphones`; capitals do not matter. This lets you keep the game on a
different device from other programs, such as voice chat or music. If no
device matches, the log lists the
names of the available devices. The game may start on the default device for
a moment before it switches.

`Dialogue=` is `game` (the default) or `positional`.

- `game` — dialogue as the game has it: wide and central, the same volume at
  any distance.
- `positional` — each voice comes from its character, as seen from the
  camera, so a character on the left of the shot is heard on the left. Voices
  also get quieter and more muffled the further the camera is from the
  speaker. The game's dialogue mostly plays in scenes where it controls the
  camera, and its cuts often jump to the other side of a conversation, so a
  voice changes sides when the shot does. The game's reverb is left as it
  is, so a distant voice also sounds more reverberant. Phone calls and other
  voices the game does not place in the scene are not changed.

With `positional`, ten more settings shape the sound; the file explains each
one, and distances are in metres:

- `DialogueSpreadNear`, `DialogueSpreadFar`, `DialogueSpreadFarAt` — how wide
  a voice sounds, from 0 (a single point) to 100 (all around you): 30 right
  next to the speaker, narrowing to 5 from 3 metres on. The game uses 75
  close up, rising to 100.
- `DialogueFalloffStart`, `DialogueFalloffPerDoubling`, `DialogueFalloffMax` —
  volume: no change within 2 metres, then 6 dB quieter each time the distance
  doubles, never more than 12 dB quieter.
- `DialogueMuffleStart`, `DialogueMuffleFullAt`, `DialogueMuffleMax` —
  muffling, from 0 (none) to 100 (most): none within 2 metres, rising to 35
  at 20 metres. The game's own muffled voices use 35.
- `DialogueMaxDistance` — where all three stop changing: 20 metres, as in the
  game's own settings.

## Headphones without Dolby Atmos

You do not need Dolby Atmos to hear the game in 3D on ordinary headphones.

- **Windows Sonic for Headphones** is free and built into Windows 10 and 11.
  Switch it on for your headphones (Settings, System, Sound, your device,
  Spatial sound) and leave `Output=auto`. The game then sends Windows its
  positioned sounds and a 7.1.4 mix, and Sonic renders them for headphones.
- **If you prefer a different sound**, HeSuVi (a free add-on for Equalizer APO)
  turns a 7.1 channel mix into headphone sound with a choice of dozens of
  virtualisations. Set it up following HeSuVi's own guide, so that Windows
  mixes your device at 7.1, and leave Windows spatial sound off. With
  `Output=auto` the mod then gives the game that 7.1 layout; `Output=7.1` does
  the same explicitly. This uses channels rather than positioned sounds, so
  it is less precise than Sonic or Atmos, but lets you pick the rendering.

## Checking that it works

Each time the game starts, the mod writes `QuarryEssentialAudio.log` next to the DLL.
It is a few lines of plain text:

- the device it used, how many channels its hardware has, what Windows mixes
  it at, and whether Windows spatial sound is on for it;
- the output it chose;
- whether spatial sound was enabled in the game's sound engine;
- what the game's sound engine (Wwise) ended up mixing to, read back from the
  engine itself — for example `Wwise is mixing to: 7.1.4.` in spatial mode or
  `7.1.` in surround mode;
- with `Dialogue=positional`, the settings in use and, once the game loads
  its dialogue, `Positional dialogue applied: 32 of 32 distance settings
  changed.` — or, if anything was not as expected, `Dialogue NOT changed:`
  with the reason, and the game's own dialogue is used.

## Uninstalling

Delete `X3DAudio1_7.dll`, `QuarryEssentialAudio.ini` and `QuarryEssentialAudio.log` from
`SMG026\Binaries\Win64`. The mod changes no game files; everything it does
happens in memory while the game runs.

## Known issues

- The mod reads the audio device when the game starts. If you change your
  default output device or its spatial sound setting, restart the game.
- A game update may change the code the spatial switch depends on. The mod
  checks the exact bytes first; if they differ, it leaves the game alone,
  says so in the log, and surround still works.
- The same goes for the dialogue: each distance setting the mod changes is
  checked against a fingerprint of the version it was made for. Any that
  differ are left as the game has them; if none match, the game's own
  dialogue is used and the log says so.

## How it works

For modders and the curious.

- **Loading.** The game imports `X3DAudio1_7.dll`, a small DirectX library
  with two functions. A DLL of that name in the game folder is loaded before
  any game code runs. The mod passes both functions through to the real copy
  in Windows, so the game's own 3D calculations are unchanged.
- **Why the game outputs stereo.** The game's sound engine, Audiokinetic
  Wwise, sizes its output from the audio device's *hardware* format
  (`PKEY_AudioEngine_DeviceFormat`), not from the format Windows mixes at. A
  stereo USB interface or headset therefore gets stereo even when Atmos has
  Windows mixing at 7.1.
- **Surround.** When the game itself reads that property, the mod answers with
  the format Windows mixes at. Wwise then opens that layout and pans 3D sounds
  into it. Reads made by Windows' own audio code on the game's behalf still
  get the true answer: the mod only changes a read whose immediate caller —
  after the Steam overlay, which sits in front of the same functions — is
  the game.
- **Spatial.** Wwise's Windows spatial sound support is compiled into the
  game but gated on an "Allow 3D Audio" setting the game ships switched off.
  The gate is a single conditional jump in the sound engine's output setup.
  The mod checks the 18 bytes around it against the known Steam build and,
  only if they match exactly, replaces the jump with two no-op instructions.
  Wwise then opens a Windows spatial audio stream with a 7.1.4 bed and up to
  128 positioned objects (measured with Dolby Atmos for Headphones).
- **Mono and device choice.** Windows refuses a one-channel output stream on
  most devices, so for mono the output itself stays as it is. Instead the mod
  calls Wwise's exported `ReplaceOutput` to rebuild the main output with a
  one-channel layout; Wwise mixes to one channel and spreads it over the
  device's real channels. The same call, with the device's Wwise id from the
  exported `AK::GetDeviceID`, moves the output to the chosen device. The
  widened format is only given for that device: the mod checks the device's
  endpoint id inside the property read.
- **Compression.** The game's startup sound bank puts a Compressor, a Peak
  Limiter and a Meter on the Master Audio Bus, always on. `Compression=off`
  calls Wwise's exported `SetBusEffect` to empty the compressor's slot on that
  bus. Measured in the game, Wwise terminates the compressor within 80 ms of
  the request, and it stays gone for the session.
- **Positional dialogue.** The game's dialogue sound bank (`Speech.bnk`)
  already places each voice on its character, with the listener on the
  camera; what blurs it is the distance settings ("attenuations") those
  voices use. The game loads the bank by name, so Wwise reads the file
  itself. The mod hooks that one call (with MinHook), reads the same file
  through Wwise's own file reader, so it gets exactly what the game would
  load, and changes the 32 attenuations used by positioned voices: for each,
  it adds a volume curve, a spread curve and a low-pass (muffling) curve
  and points the attenuation at them, keeping every original curve and the
  reverb sends as they were. It then loads that copy with Wwise's
  `LoadBankMemoryCopy`. Nothing on disk changes. Each attenuation is
  matched to a fingerprint first, and anything unexpected falls back to the
  game's own load. With `Dialogue=game`, nothing is hooked.
- **Reading the result back.** The game exports Wwise's API by name; the mod
  calls `GetSpeakerConfiguration` to log what Wwise is actually mixing to,
  and `SetPanningRule` for the headphones option.

## Building

With MinGW-w64 GCC (for example WinLibs) on `PATH`, or its path in the
`QSA_GCC` environment variable:

    .\build.ps1          # builds build\X3DAudio1_7.dll
    .\build.ps1 -Test    # also builds and runs the offline tests

The tests stand in for the game: the test program exports stand-ins for the
Wwise functions under the game's names, and checks the pass-through, every
output decision, the byte check before the spatial patch (including refusing
a one-byte difference), that only the game's own device-format reads are
changed, a speaker layout set by hand (both accepted and refused by Windows),
mono, headphone panning, the device choice (using another active output on
the test machine, and a name that matches nothing), removing the master
compressor, and positional dialogue: falling back to the game's own load when
the dialogue file will not open or is not the version the mod knows, leaving
other sound banks alone, and doing nothing at all with `Dialogue=game`.

The repository does not include the game's files, so one dialogue test needs
the game's own `Speech.bnk`, extracted from its packages. Set `QSA_SPEECH_BNK`
to its path and the test loads the changed copy, and
`tools\check_dialogue_bank.py` then checks that copy on its own terms: every
other part of the bank unchanged, the new curves where they should be, and
their values on the same scale as the game's own. Set `QSA_WWISER` to
`wwiser.pyz` to have that independent bank parser read it too. Without
`QSA_SPEECH_BNK` the test says it was not exercised. After a game update that
changes the dialogue, `tools\dialogue_table.py` rebuilds the list of
attenuations and their fingerprints.

## License

GPL-3.0-or-later. See `LICENSE`.

Includes MinHook by Tsuda Kageyu (BSD 2-Clause), in `third_party\minhook`
with its own `LICENSE.txt`.

This is an unofficial, fan-made mod. It is not affiliated with, endorsed by or
supported by Supermassive Games, 2K or Microsoft. The Quarry is a trademark of
its respective owners. The mod does not include any of the game's files or
assets, or any Microsoft code; use it at your own risk.
