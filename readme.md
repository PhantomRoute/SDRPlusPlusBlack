# SDR++Black

![Build Status](https://img.shields.io/github/actions/workflow/status/PhantomRoute/SDRPlusPlusBlack/build_all.yml?branch=master)

Welcome to **SDR++Black**, my personal take on what SDR++ should be.

I built this fork for myself, because the existing versions didn't have everything I wanted - being able to pick which protocols the DSD demodulators sync on, a cleaner dropdown UI, and a few sharp edges filed off. It reflects my priorities, not anyone else's.

**If that isn't what you're after, no hard feelings.** Both projects this one is built on are excellent and actively maintained, and you should use whichever suits you:

* **[SDR++Brown](https://github.com/sannysanoff/SDRPlusPlusBrown)** by sannysanoff - the fork this one is based on, with a large set of features of its own.
* **[SDR++](https://github.com/AlexandreRouma/SDRPlusPlus)** by Alexandre Rouma - the original, and the reason any of this exists.

Parts of this fork are written with AI assistance - mostly the hairier decoder work, where I'd rather say so than pretend otherwise. Worth knowing that the original SDR++ forbids AI-generated contributions as a matter of policy. If you feel the same way, that's a perfectly good reason to use it instead.

> **Pre-release.** This is under rapid development. Things move, settings get renamed, and the
> README will be rewritten properly, with screenshots, once there is a beta.

## What's different

A quick tour of what this fork adds on top of SDR++Brown. Most of it lives in the menus named below.

### Radio

* **More than one radio.** **Audio > Add another radio** adds a full Radio instance with its own
  VFO, mode and audio, all on the same SDR. Remove it the same way. Deleting a radio or any other
  module also deletes its saved settings, so a new radio with the same name starts clean.
* **CTCSS and DCS tone squelch** (NFM). The radio identifies the tone or code it hears, can open
  only on a chosen tone, on any tone, or on a custom list of them, and can strip the tone from the
  audio the way a handheld does, harmonic buzz included. The tone is saved with a bookmark.
* **Any bandwidth** unlocks each mode's bandwidth limits.
* **AM AGC hang** holds the gain through the pauses between words so static doesn't swell up.

### Digital voice

Both digital voice modes in the Radio module have a **Protocols** dropdown. Leave everything ticked and it auto-detects; untick a protocol to stop it syncing on that one.

* **DSD** - P25p1, DMR
* **oldDSD** - P25p1, DMR, NXDN48, NXDN96, D-STAR, X2-TDMA, ProVoice

Your selection is saved per VFO, so NXDN96 (which is off by default) stays on once you enable it.
Both have a readable status panel and a **call log** that keeps who was talking across fades,
including D-STAR callsigns and the short radio message.

### Looking at signals

* **Signal ID** measures the signal under the VFO: peak, width, shape, SNR, timing, drift and
  modulation measurements. It reports numbers only and never guesses what the signal is. It can
  also **follow** a drifting signal and keep the VFO on it.
* **Display > Panels** puts extra views in the strip along the bottom of the window:
  * **SNR chart** - SNR history, no noise reduction needed.
  * **Occupancy** - how busy each channel in view has been. Click a busy channel to tune to it.
  * **IQ plot** - I against Q, or I and Q against time, with DC offset, imbalance and clipping readouts.
  * **Signal analyzer** - instantaneous frequency, eye and constellation views of the signal in the
    VFO, or of what the DSD decoders actually sampled. **Pause** freezes the view so you can
    change the span and look at what you caught.
* **Min hold** and **Persistence** sit beside **Peak hold** in the Display menu.

### Frequency manager and scanner

* Bookmarks keep **notes**, their **mode**, their **tone** and their own **skip** flag, can be moved
  between lists, and round-trip through **CSV** so a list can be edited in a spreadsheet.
* **Prev/Next** steps through the bookmarks in a list.
* The **scanner** was rebuilt, and a **channel activity history** shows when each channel was heard,
  hour by hour, across restarts.

### Recording and decoders

* The **Recorder** panel was redone: one big Record button, a proper peak meter with clip light,
  and audio in **WAV, FLAC or MP3**. Baseband stays WAV.
* **Radiosonde** decoder, including iMet-54, with a flight graph, burst marker and tropopause estimate.
* **MSK144** in the FT8 decoder, for meteor scatter.

### Safety and robustness

* Switching on **Bias-T** or a **HackRF RF amp** asks first. The RTL-SDR bias-T and the HackRF amp
  are switched off when the radio stops.
* If the SDR stops sending samples the radio is stopped with a notice, and repeated dropouts
  suggest checking the cable or port.
* A lot of crash, hang and teardown fixes, several of them found by stress testing module
  add/remove under a debugger.

### Phone and tablet

* **Display > Big controls** gives a touch layout with larger controls.
* **Auto** interface size on Android follows the device's screen density and font size.

## Themes

Every colour setting lives in the **Theme** menu - the theme itself, the waterfall colour map (was
in Display), and the per-VFO colours (was a top-level section of its own).

Everything the UI draws is themable, not just the ImGui widgets: the waterfall background, the FFT
grid and trace, the squelch and scanner bars, the VFO and notch markers, the band plan, the SNR and
volume meters, and the frequency selector.

**Theme > Customize** opens an editor with a colour picker for every one of them. Changes preview
live against the running waterfall, so you can judge a colour where you'll actually be looking at
it. Give the theme a name and hit **Save** and it lands in `themes/` next to your config, where it
survives upgrades. The shipped themes are read-only - editing one and saving suggests a name for
your own copy instead of overwriting it.

A theme also names its waterfall gradient, so picking one switches the colour map too and a theme
you send someone arrives looking the way you built it. Changing the colour map by hand afterwards
overrides it and sticks - including across restarts - until you pick a different theme. The theme
file is where a gradient choice comes *from*; your config is what remembers the one you're on.

### Spectrum style

The **Spectrum** section of the Theme menu controls how the live FFT trace is drawn, the way
SDRangel does it:

* **Trace style** - *Solid* draws it in the theme's FFT trace colour. *Reflection* colours it by the
  waterfall colour map, so a peak takes the colour it would have in the waterfall below it and the
  spectrum reflects the waterfall. *Gradient* colours it by the theme's own gradient instead.
* **Fill style** - what goes under the trace: *None*, *Solid* in the trace colour, or *Reflection* /
  *Gradient*, which fade from the colour at the signal's level down to the colour at the bottom of
  the spectrum.
* **Trace intensity** / **Fill intensity** - how strongly each is drawn.

Pick *Gradient* for either and a gradient editor appears: a preview strip, then one row per colour
stop with a colour picker, its position on the spectrum's vertical range (0 at the bottom, 1 at the
top) and a remove button, plus **Add stop**, which drops a new stop into the widest gap in the
colour it already has there so you get a handle to pull rather than a jump. Stops can be dragged
past each other, there is no limit on how many you use, and the colours' own alpha is multiplied by
the intensity slider - so a stop can fade out entirely while its neighbours stay solid.

All of it, the gradient included, is part of the theme like the colours are, so it saves, exports and
imports with it, and it behaves like the colour map does: picking a theme takes its styling,
changing something by hand afterwards overrides it until the next theme change. Fill style replaces
the Display menu's old **Shadow** checkbox, which is now *Fill style: None* - if you had it off, it
stays off.

**Export** writes the theme out as a self-contained `.json` with every colour spelled out, so
sending someone a theme is sending them one file. **Import** on the Theme menu reads one back; if
you already have a theme by that name, the imported one is renamed rather than replacing yours. A
theme asking for a gradient you don't have applies everything else and leaves the gradient alone.

**Please do not report bugs in this fork to Alexandre Rouma or sannysanoff.** They did not write this code and cannot fix it. If something is broken in SDR++Black, file an [issue](https://github.com/PhantomRoute/SDRPlusPlusBlack/issues) here.

[Changelog](changelog.md)

WINDOWS INSTALL TROUBLESHOOTING: https://youtu.be/Q3CV5U-2IIU

## Thanks / Credits

Thanks and due respect to:
 
* The original author, Alexandre Rouma, for his great [work](https://github.com/AlexandreRouma/SDRPlusPlus). Due credits go to all contributors in the upstream project.
* MSHV author, LZ2HV, for his great [work](http://lz2hv.org/mshv).
* logmmse/python authors for their great [work](https://github.com/wilsonchingg/logmmse).
* OMLSA authors for their great [idea](https://github.com/yuzhouhe2000/OMLSA-IMCRA) and [implementation](https://github.com/xiaochunxin/OMLSA-MCRA).
* imgui-notify author for his great [work](https://github.com/patrickcjk/imgui-notify)
* implot author for his great [work](https://github.com/epezent/implot/)
* alexander-sholohov (github) for his work on soapy_sdr module.
* Cropinghigh / Indir for his [work](github.com/cropinghigh/sdrpp-vhfvoiceradio) on extra VHF modes.
* monolifed for his [pbkdf2 header-only implementation](https://github.com/monolifed/pbkdf2-hmac-sha256)  
* ruse39 for his contributions on code quality

## Feedback

Found an issue? File an [issue](https://github.com/PhantomRoute/SDRPlusPlusBlack/issues).

## Debugging reminders

* to debug in windows in virtualbox env, download mesa opengl32.dll from https://downloads.fdossena.com/Projects/Mesa3D/Builds/MesaForWindows-x64-20.1.8.7z
* make sure you put rtaudiod.dll in the build folder's root otherwise audio sink will not load.
* use system monitor to debug missing dlls while they fail to load.

## Local Android build:

* put into your ~/.gradle/gradle.properties this line: sdrKitRoot=/home/user/SDRPlusPlus/android-sdr-kit/sdr-kit
  * it can obtained + built from: https://github.com/AlexandreRouma/android-sdr-kit 
  * docker build --platform linux/amd64 -t android-sdr-kit  .
  * docker start android-sdr-kit    # it will exit
  * docker cp be03210da56a:/sdr-kit .    # will create directory with built binary libs, replace be03210da56a with id obtained from 'docker ps -a'
* use jdk11 for gradle in android studio. Android Studio -> Settings -> ... -> Gradle -> Gradle JDK . This is needed if you have various errors with java.io unaccessible fields.
* in case of invalid keystore error (should not happen with jdk11): 
  * you may create new keystore with current jdk version:
    ~/soft/jdk8/bin/keytool -genkey -v -keystore debug2.keystore -storepass android -alias androiddebugkey -keypass android -keyalg RSA -keysize 2048 -validity 10000
  * use this filename (debug2.keystore) in app/build.gradle along with passwords in the signingConfigs -> debug section.

Good luck.
