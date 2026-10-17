# PrismELF
- PS2 file manager / ELF launcher / viewer

Built with PS2SDK + gsKit. Developed by **MexrlDev**. MIT licensed. Test on a real console.

## Features
- Dark-blue splash (X/O skips), background, per-type icons: folder, **blue ELF**, audio, image, video, text/markdown, archive, generic file
- Starts on USB (`mass:`). **Memory cards use the console's ROM driver + libmc (like wLaunchELF)**, loaded only when you open them, read in the background with **O = cancel**
- Select (or Start > Devices) = **Devices root** (mc0, mc1, USB). `..` at a device root also goes there
- Copy / cut / paste (folders too), delete, rename, new folder, multi-select, info, hex view - on USB and memory cards
- **Viewers** (X on the file): `.txt .log .cfg .ini .json ...` text, `.md` markdown (headings, lists, quotes, code blocks, rules), and **png / jpg / bmp images**.
  D-pad Left/Right = previous/next file of the same kind (says "No more files" at the ends), O = close. Text: Up/Down/stick scroll (accelerates), L1/R1 page, Triangle top
- **Music player**: X on a `.wav` or `.mp3` plays it as the background music (replacing the current one). Start > Music: pause/resume / restore default. Header shows what is playing. L2 mutes
- Background music engine converts everything to 48 kHz stereo itself (fixes the periodic glitching); music runs above the UI thread
- Crash-safe log: `FileExplorer-Logs/LOGn/LOG.txt` on USB, every line written immediately
- **Settings screen** (Start > Settings): hidden files on/off, sort, music on/off + volume, delete confirmation, button hints, log file, pause-music-while-reading-a-card, start screen. **Changes auto-save** (to `PrismELF/settings.cfg` on USB, or mc0/mc1 in the background if there is no USB); Save now / Reset to defaults are in the screen
- Files the app cannot open (rar, tiff, unknown binaries ...) show an **Unsupported file** popup instead of doing anything
- A song you play from the file list plays once, then the default music comes back
- **L2 + R2 together = credits**

## Controls
| Button | Action |
|---|---|
| Stick / D-pad Up,Down | Scroll (accelerates) |
| D-pad Left/Right | Page (10 rows) in lists; previous/next file in viewers |
| X | Open folder / launch ELF / view / play |
| O | Parent folder (device root -> Devices) / close viewer / cancel memory card read |
| Triangle | Context menu (copy, cut, paste, delete, rename, new folder, info, hex) |
| Square | Mark / unmark the highlighted item (cursor moves down) |
| L1 / R1 | Previous / next device (acts when released) |
| L1 + R1 together | Select all / select none in this folder |
| L2 | Mute music (saved) |
| R2 | Show/hide .hidden files (saved) |
| L2 + R2 | Credits |
| L3 / R3 | Cycle sort / Refresh (also in the menus) |
| Select | Devices root |
| Start | Main menu (devices, music, log, sort, credits, exit to OSD) |

## Build
Push to GitHub: `.github/workflows/build.yml` builds `PrismELF.elf` in the `ps2dev/ps2dev` image. Locally: `make` with the ps2dev toolchain.
PNG / JPEG / MP3 support is compiled in only if `ps2sdk-ports` provides libpng / libjpeg / libmad (headers are detected by the Makefile); BMP and WAV always work.

## Your own images and music
- Put images in `images/` (`logo`, `bg`, `splashbg`, `file`, `folder`, `elf`, `audio`, `image`, `video`, `text`, `archive`), any size; `tools/covert-images.py` (run by the workflow) converts them to `assets/*.rgba`
- Put ONE audio file in `music/`; `tools/covert-bgm.py` converts it for the ELF. Without it the app looks for `PrismELF/bgm.wav` on USB / mc0 / mc1

## Limits
- Images are shown scaled to fit 512x384 (PNG up to ~2.4 MP, JPEG any size). GIF (first frame) and TGA work; TIFF/WebP show a "not supported" message. No video playback, hdd0:, network.
- Memory-card logging was dropped; logs go to USB.

## License
MIT License, Copyright (c) 2026 MexrlDev (see `LICENSE`). PS2SDK/gsKit (AFL) apply to the linked libraries. Not affiliated with Sony.

## Recent additions
- Wrap-around list navigation, scrolling (marquee) text for long names and toasts.
- Formats opened inside the app: WAV, MP3, MP2/MPA and FLAC audio; PNG, JPEG, BMP, GIF (first frame) and TGA images; text and Markdown. Video (MP4, AVI, DivX, Xvid, MPG/MPEG) and AAC, M4A, Ogg, WMA, AC-3 and DTS audio are handed to a media player ELF: pick the player once (Triangle on its ELF > "Set as media player"), then opening such a file asks "Play this in your media player?" and starts it with the file path as the first argument. Whether the player honours that argument depends on the player.
- ZIP: opening a .zip asks "Do you want to unzip ...?" and extracts into a folder named after the zip (a copy suffix is added if that folder exists). "Unzip" also appears in the file menu, only on .zip and .7z files. Zip: stored and deflate only; no passwords, no ZIP64.
- Selecting: Square marks one item, L1+R1 marks everything (again = none); the file menu also has Select all / Select none / Invert selection. Copy, Cut, Delete and Compress work on all marked items, or on the highlighted one if nothing is marked (a folder counts as one item and includes everything inside).
- Compress: file menu > **Compress to .zip** / **Compress to .7z**. The archive is created next to the items (one item: its name, several: the current folder's name; never overwrites, uses name_2 ...). Zip uses deflate (already-compressed files like mp3/png/zip are stored). 7z uses a built-in LZMA encoder in one solid block (1 MB window): smaller than zip, but slow on a PS2 - O cancels and removes the partial archive. Limits: zip 4 GB / 65535 files; no passwords.
- 7z: same flow for .7z (own decoder: LZMA, LZMA2, Deflate, Copy, BCJ/ARM/PPC/SPARC/Delta filters, solid archives, compressed headers, CRC check). Not supported: passwords/AES, PPMd, BZip2, BCJ2, multi-volume - those files are skipped and counted as failed. Big dictionaries need RAM (7-Zip's default is 16 MB; it never allocates more than the unpacked size). **RAR is not supported.**
- Text detection: Makefile, README, LICENSE, Dockerfile, dotfiles and many source extensions open as text; any other file with no usable extension is checked by content (first 1 KB) and opens as text if it looks like text.
- Discs: a CD/DVD in the drive shows up as "CD / DVD (cdfs:)" (read-only; PS2 CD/DVD, PS1 CD, DVD video); MP3s and images open straight from it. "Rescan devices" (Start menu, or R3 on the devices list) re-checks USB, host, the disc drive and both memory cards and says what changed.
- Settings: "Show all devices" lists hdd0:, cdfs:, host:, rom0:, rom1: even when empty.
- Music pauses for memory-card reads and resumes afterwards (silence-only suspend, no audsrv stop).
- If no `BGM/bgm.wav|mp3` is found on USB, a `BGM/` folder with a README is created; a USB BGM overrides the embedded track at boot.
- "Loading..." screen when launching an ELF; USB-removed popup with refresh (menu entry or any button after reinserting).
- Settings load before music starts, so a muted setup never plays during the splash.
- Text widths can be tuned via `CWIDTH` in `src/gfx.h`.
- Settings: "Emulator host: (PCSX2)" adds `host:` to the devices list (served through the fio driver).
- Launch: the Makefile links `libelf-loader-nocolour` automatically when the SDK provides it (no rainbow flash).
