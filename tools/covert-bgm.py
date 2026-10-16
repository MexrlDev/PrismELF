#!/usr/bin/env python3
"""covert-bgm.py - convert the audio file in music/ into the best format for PrismELF.

Input : first audio file in music/ (mp3, wav, ogg, flac, m4a, aac, wma, opus, aiff).
        A file named bgm.<ext> is preferred; otherwise the first one alphabetically.
Output: gen_music/bgm.wav  = 16-bit PCM little-endian WAV (what the PS2 player accepts),
        44.1 kHz stereo when it fits, stepping down in rate/channels only if the file
        would be bigger than --max-mb, and trimmed as a last resort.
The Makefile embeds gen_music/bgm.wav into the ELF (falls back to a ready-made
music/*.wav if this script was not run). Needs ffmpeg for anything that is not
already a suitable WAV.

Usage: python3 tools/covert-bgm.py [--src music] [--out gen_music] [--max-mb 8] [--keep-going]
"""
import argparse, glob, os, shutil, subprocess, sys, wave

AUDIO_EXT = {".wav", ".mp3", ".ogg", ".oga", ".flac", ".m4a", ".aac", ".wma", ".opus", ".aif", ".aiff"}
LADDER = [(44100, 2), (32000, 2), (22050, 2), (22050, 1), (16000, 1)]   # (rate, channels), best first
OK_RATES = {8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000}


def log(msg):
    print(f"[covert-bgm] {msg}", flush=True)


def find_source(src_dir):
    files = sorted(f for f in glob.glob(os.path.join(src_dir, "*"))
                   if os.path.isfile(f) and os.path.splitext(f)[1].lower() in AUDIO_EXT
                   and not os.path.basename(f).startswith("."))
    if not files:
        return None, []
    pref = [f for f in files if os.path.splitext(os.path.basename(f))[0].lower() == "bgm"]
    return (pref or files)[0], files


def duration(path):
    exe = shutil.which("ffprobe")
    if not exe:
        return None
    try:
        out = subprocess.run([exe, "-v", "error", "-show_entries", "format=duration",
                              "-of", "csv=p=0", path], capture_output=True, text=True, check=True).stdout
        return float(out.strip())
    except Exception:
        return None


def wav_info(path):
    """(rate, channels, bits, bytes) for a PCM WAV, or None if the stdlib cannot read it."""
    try:
        with wave.open(path, "rb") as w:
            return w.getframerate(), w.getnchannels(), w.getsampwidth() * 8, os.path.getsize(path)
    except Exception:
        return None


def run_ffmpeg(ffmpeg, src, dst, rate, ch, seconds=None):
    cmd = [ffmpeg, "-y", "-v", "error", "-i", src, "-vn", "-map_metadata", "-1",
           "-ac", str(ch), "-ar", str(rate), "-c:a", "pcm_s16le", "-bitexact"]
    if seconds:
        cmd += ["-t", f"{seconds:.2f}"]
    cmd += ["-f", "wav", dst]
    subprocess.run(cmd, check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", default="music")
    ap.add_argument("--out", default="gen_music")
    ap.add_argument("--max-mb", type=float, default=8.0, help="max size of the embedded WAV (default 8)")
    ap.add_argument("--keep-going", action="store_true", help="on failure print a warning and exit 0")
    a = ap.parse_args()
    limit = int(a.max_mb * 1024 * 1024)

    shutil.rmtree(a.out, ignore_errors=True)            # never keep stale output
    src, files = find_source(a.src)
    if not src:
        log(f"no audio in {a.src}/ -> nothing to embed (the app will use USB / mc0 / mc1 at runtime)")
        return 0
    if len(files) > 1:
        log(f"{len(files)} audio files found, using '{os.path.basename(src)}' (only one is embedded)")
    os.makedirs(a.out, exist_ok=True)
    dst = os.path.join(a.out, "bgm.wav")
    ffmpeg = shutil.which("ffmpeg")

    try:
        if not ffmpeg:
            info = wav_info(src) if src.lower().endswith(".wav") else None
            if info and info[1] in (1, 2) and info[2] == 16 and info[0] in OK_RATES and info[3] <= limit:
                shutil.copyfile(src, dst)
                log(f"ffmpeg not found; '{src}' is already a suitable WAV ({info[0]} Hz, {info[1]}ch) -> copied")
                return 0
            raise RuntimeError("ffmpeg is required to convert this file (install ffmpeg), "
                               "or provide a 16-bit PCM WAV under the size limit")

        dur = duration(src)
        rate, ch = LADDER[-1]
        trim = None
        if dur:
            for r, c in LADDER:                         # best quality that fits the limit
                if dur * r * c * 2 + 44 <= limit:
                    rate, ch = r, c
                    break
            else:
                trim = (limit - 4096) / (rate * ch * 2)  # nothing fits: trim at the smallest format
        run_ffmpeg(ffmpeg, src, dst, rate, ch, trim)
        size = os.path.getsize(dst)
        if size > limit:                                 # duration was unknown: shrink by trimming
            run_ffmpeg(ffmpeg, src, dst, rate, ch, (limit - 4096) / (rate * ch * 2))
            trim = True
            size = os.path.getsize(dst)
        info = wav_info(dst)
        if not info or info[2] != 16 or info[1] not in (1, 2):
            raise RuntimeError("converted file failed validation")
        log(f"'{os.path.basename(src)}' -> {dst}: {rate} Hz, {ch}ch, 16-bit PCM, {size / 1048576:.2f} MB"
            + (" (TRIMMED to fit the size limit)" if trim else ""))
        return 0
    except Exception as e:                               # noqa: BLE001
        shutil.rmtree(a.out, ignore_errors=True)
        log(f"ERROR: {e}")
        return 0 if a.keep_going else 1


if __name__ == "__main__":
    sys.exit(main())
