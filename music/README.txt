Put ONE music file here (mp3, wav, ogg, flac, m4a, aac, wma, opus, aiff). It is baked into PrismELF.elf
so it plays with no USB / memory card.

* The GitHub Action runs tools/covert-bgm.py, which converts it to the best PS2 format:
  16-bit PCM WAV, 44.1 kHz stereo (stepped down / trimmed only if it would exceed 8 MB).
* Locally: python3 tools/covert-bgm.py   (needs ffmpeg), then make.
* A file named bgm.<ext> is preferred; otherwise the first one alphabetically. Only one is embedded.
* Nothing here = nothing embedded; the app uses PrismELF/bgm.wav on USB (mass:), then mc0:, then mc1:.
  That runtime fallback is unchanged.
