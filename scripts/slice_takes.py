# slice_takes.py: cut long chord takes into short labelled clips, one per strum (phase 4)

# record_clip.py saves one long take per chord (3 to 5 minutes of repeated strums). This script finds each
# strum in a take with librosa's onset detection and saves the 2 seconds starting at that strum as a clip.
# It runs on the computer, not the ESP32.

# How to use it (from the project folder):
#   .venv\Scripts\python scripts\slice_takes.py              slice every take that hasn't been sliced yet
#   .venv\Scripts\python scripts\slice_takes.py --dry-run    only show how many clips each take would give
#     --seconds 2        clip length in seconds (default 2)
#     --min-gap 1.0      at least this many seconds between the starts of two clips (default 1.0)
#
# where the files are:
#   takes in:   data/takes/<session>/<chord>/take_###.wav
#   clips out:  data/raw/<session>/<chord>/clip_###.wav
#               data/raw/<session>/<chord>/slices.csv   (which take each clip came from, and where in it)
# A take listed in slices.csv is never sliced again, so clips you delete (bad ones) stay deleted.
# To slice a take again, delete its rows from slices.csv (and its old clips) first.
#
# The "none" chord (silence, room noise, muted strings) has no strums to find, so its takes are cut into
# back-to-back clips instead.

# ---------------------------------------------------------------------------------------------
# Libraries
# ---------------------------------------------------------------------------------------------

# reads the --seconds / --min-gap / --dry-run options from the command line
import argparse
# reads and writes slices.csv
import csv

# fast arrays of numbers
import numpy as np
# audio analysis: finds where each strum starts ("onsets")
import librosa
# reads and writes WAV files
import soundfile as sf

# the project folder, and the shared helper that finds the next free file number (from record_clip.py)
from record_clip import PROJECT_DIR, TAKES_DIR, next_number

# ---------------------------------------------------------------------------------------------
# Settings
# ---------------------------------------------------------------------------------------------

# where the clips go: data/raw/<session>/<chord>/clip_###.wav
RAW_DIR = PROJECT_DIR / "data" / "raw"
# the chord folder name for "no chord" takes, which get cut into back-to-back clips
NO_CHORD_LABEL = "none"
# start each clip this many seconds before the strum, so the very start of the attack isn't cut off
PRE_ROLL = 0.05
# skip clips quieter than this fraction of the take's typical (median) clip, e.g. a stray "strum" in a pause
QUIET_RATIO = 0.2
# the file in each clip folder that records where every clip came from
SLICES_FILE = "slices.csv"


# ---------------------------------------------------------------------------------------------
# already_sliced: return the names of the takes listed in a clip folder's slices.csv
#   out_folder - the clip folder, e.g. data/raw/2026-10-05/C
# ---------------------------------------------------------------------------------------------
def already_sliced(out_folder):
    # the slices.csv file in that folder
    path = out_folder / SLICES_FILE
    # no file yet: nothing sliced
    if not path.exists():
        return set()
    # read every row and collect the "take" column (a set ignores repeats)
    with open(path, newline="") as f:
        return {row["take"] for row in csv.DictReader(f)}


# ---------------------------------------------------------------------------------------------
# find_starts: return the sample positions where clips should start, and counts of what was skipped
#   audio      - the take's samples as floats (-1.0 to 1.0)
#   rate       - samples per second
#   clip_len   - clip length in samples
#   min_gap    - smallest distance in samples between two clip starts
#   even       - True = cut back-to-back clips (for "none"), False = start a clip at each strum
# returns (list of start positions, how many onsets were found, skipped as too close, skipped as too quiet)
# ---------------------------------------------------------------------------------------------
def find_starts(audio, rate, clip_len, min_gap, even):
    # "none" takes: one clip right after another, as many as fit
    if even:
        starts = list(range(0, len(audio) - clip_len + 1, clip_len))
        # no strums looked for, nothing skipped
        return starts, 0, 0, 0

    # find every strum. units="samples" gives positions in samples (not seconds or frames), and
    # backtrack=True moves each one back to where the sound starts rising, instead of its loudest point
    onsets = librosa.onset.onset_detect(y=audio, sr=rate, units="samples", backtrack=True)
    # how many samples PRE_ROLL is
    pre_roll = int(PRE_ROLL * rate)

    # strums that are far enough apart, and leave room for a whole clip before the end of the take
    starts = []
    # strums skipped because they came too soon after the previous clip's start
    too_close = 0
    for onset in onsets:
        # start a little before the strum (but not before the start of the take)
        start = max(onset - pre_roll, 0)
        # not enough take left for a whole clip: this and every later strum won't fit
        if start + clip_len > len(audio):
            break
        # too close to the previous clip: it would be almost the same audio twice
        if starts and start - starts[-1] < min_gap:
            too_close += 1
            continue
        # keep it
        starts.append(start)

    # nothing found: nothing more to check
    if not starts:
        return starts, len(onsets), too_close, 0
    # RMS loudness of every candidate clip
    loudness = [np.sqrt(np.mean(audio[s : s + clip_len] ** 2)) for s in starts]
    # the typical clip's loudness
    typical = np.median(loudness)
    # keep only clips that aren't much quieter than typical
    loud_enough = [s for s, rms in zip(starts, loudness) if rms >= QUIET_RATIO * typical]
    # report how many were dropped as too quiet
    return loud_enough, len(onsets), too_close, len(starts) - len(loud_enough)


# ---------------------------------------------------------------------------------------------
# slice_take: cut one take into clips, save them (unless dry_run) and print a one-line report
#   take_path  - the take's WAV file
#   out_folder - where its clips go
#   chord      - the chord name (the take's folder name)
#   seconds    - clip length in seconds
#   min_gap    - smallest time in seconds between two clip starts
#   dry_run    - True = only report, don't save anything
# returns how many clips were (or would be) saved
# ---------------------------------------------------------------------------------------------
def slice_take(take_path, out_folder, chord, seconds, min_gap, dry_run):
    # read the take as the original 16-bit numbers, so clips are saved exactly as recorded
    pcm, rate = sf.read(take_path, dtype="int16")
    # the same audio as floats from -1.0 to 1.0, which librosa expects
    audio = pcm.astype(np.float32) / 32768
    # remove the mic's DC offset (a constant shift that isn't sound), so it doesn't count as loudness
    audio -= audio.mean()
    # clip length and minimum gap, in samples
    clip_len = int(seconds * rate)
    gap = int(min_gap * rate)

    # where the clips start
    starts, found, too_close, too_quiet = find_starts(audio, rate, clip_len, gap, chord == NO_CHORD_LABEL)
    # one-line report; "none" takes don't look for strums, so they get a shorter one
    report = f"{take_path.relative_to(PROJECT_DIR)} ({len(pcm) / rate:.0f} s): "
    if chord != NO_CHORD_LABEL:
        report += f"{found} onsets, {too_close} too close together, {too_quiet} too quiet, "
    print(report + f"{len(starts)} clips")

    # dry run, or nothing to save: stop here
    if dry_run or not starts:
        return len(starts)

    # make the clip folder
    out_folder.mkdir(parents=True, exist_ok=True)
    # does slices.csv need its column names written first? (only when the file is new)
    new_file = not (out_folder / SLICES_FILE).exists()
    # the first free clip number in the folder
    number = next_number(out_folder, "clip")
    # open slices.csv to add rows to the end ("a" = append)
    with open(out_folder / SLICES_FILE, "a", newline="") as f:
        # writes one comma-separated row per clip
        writer = csv.writer(f)
        # column names for a new file
        if new_file:
            writer.writerow(["clip", "take", "start_seconds"])
        for start in starts:
            # the clip's file name, e.g. clip_001.wav
            name = f"clip_{number:03d}.wav"
            # save the clip's samples as a mono, 16-bit WAV
            sf.write(out_folder / name, pcm[start : start + clip_len], rate, subtype="PCM_16")
            # note where it came from, so you can find it in the take if it sounds wrong
            writer.writerow([name, take_path.name, f"{start / rate:.3f}"])
            # next number
            number += 1
    return len(starts)


# ---------------------------------------------------------------------------------------------
# main: read the options, then slice every take that hasn't been sliced yet
# ---------------------------------------------------------------------------------------------
def main():
    # command line options
    parser = argparse.ArgumentParser(description="Cut long chord takes into short labelled clips.")
    parser.add_argument("--seconds", type=float, default=2.0, help="clip length in seconds (default 2)")
    parser.add_argument("--min-gap", type=float, default=1.0,
                        help="smallest time in seconds between two clip starts (default 1.0)")
    parser.add_argument("--dry-run", action="store_true", help="only report how many clips each take gives")
    args = parser.parse_args()

    # every take, in order: data/takes/<session>/<chord>/take_###.wav
    takes = sorted(TAKES_DIR.glob("*/*/take_*.wav"))
    # nothing recorded yet
    if not takes:
        print(f"No takes found in {TAKES_DIR.relative_to(PROJECT_DIR)}. Record some with record_clip.py --label.")
        return

    # clips saved (or that would be saved) this run
    total = 0
    # takes skipped because they were sliced before
    skipped = 0
    for take_path in takes:
        # the take's chord and session are its folder names
        chord = take_path.parent.name
        session = take_path.parent.parent.name
        # the matching clip folder: data/raw/<session>/<chord>/
        out_folder = RAW_DIR / session / chord
        # already sliced: leave it alone
        if take_path.name in already_sliced(out_folder):
            skipped += 1
            continue
        # slice it
        total += slice_take(take_path, out_folder, chord, args.seconds, args.min_gap, args.dry_run)

    # summary
    action = "would be saved (dry run)" if args.dry_run else f"saved in {RAW_DIR.relative_to(PROJECT_DIR)}"
    print(f"\n{total} clips {action}; {skipped} takes were already sliced")


# run main() only when this file is run directly (not when it's imported by another script)
if __name__ == "__main__":
    main()
