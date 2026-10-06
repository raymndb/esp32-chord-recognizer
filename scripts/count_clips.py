# count_clips.py: show how many clips each chord has in each session, and play a few to check them (phase 4)

# The model learns best when every chord has about the same number of clips, spread over several sessions.
# This prints a table of clip counts (chords down the side, sessions across the top) and warns about
# chords that are falling behind. It runs on the computer, not the ESP32.

# How to use it (from the project folder):
#   .venv\Scripts\python scripts\count_clips.py                     print the table
#   .venv\Scripts\python scripts\count_clips.py --listen 5          then play 5 random clips of every chord
#   .venv\Scripts\python scripts\count_clips.py --listen 5 --chord C     ...of just one chord
# Listening catches clips with the wrong label, clicks, or no strum in them: delete those WAV files
# (slices.csv still says where each one came from in the take).

# ---------------------------------------------------------------------------------------------
# Libraries
# ---------------------------------------------------------------------------------------------

# reads the --listen / --chord options from the command line
import argparse
# picks random clips to listen to
import random
# counts things by name
from collections import Counter

# the project folder, and the shared playback helper (from record_clip.py)
from record_clip import PROJECT_DIR, play

# ---------------------------------------------------------------------------------------------
# Settings
# ---------------------------------------------------------------------------------------------

# where slice_takes.py saves the clips: data/raw/<session>/<chord>/clip_###.wav
RAW_DIR = PROJECT_DIR / "data" / "raw"
# the first goal: at least this many clips per chord...
TARGET_CLIPS = 50
# ...recorded in at least this many separate sessions
TARGET_SESSIONS = 4
# warn when a chord has less than this fraction of the clips of the biggest chord
IMBALANCE_RATIO = 0.5


# ---------------------------------------------------------------------------------------------
# main: count the clips, print the table and warnings, then play some clips if asked
# ---------------------------------------------------------------------------------------------
def main():
    # command line options
    parser = argparse.ArgumentParser(description="Count clips per chord and session, and listen to a sample.")
    parser.add_argument("--listen", type=int, default=0, help="play this many random clips of each chord")
    parser.add_argument("--chord", help="only listen to this chord")
    args = parser.parse_args()

    # every clip: data/raw/<session>/<chord>/clip_###.wav
    clips = sorted(RAW_DIR.glob("*/*/clip_*.wav"))
    # nothing sliced yet
    if not clips:
        print(f"No clips found in {RAW_DIR.relative_to(PROJECT_DIR)}. Run slice_takes.py first.")
        return

    # clips per (chord, session) pair; a clip's chord and session are its folder names
    counts = Counter((c.parent.name, c.parent.parent.name) for c in clips)
    # all chord and session names, in alphabetical order
    chords = sorted({chord for chord, _ in counts})
    sessions = sorted({session for _, session in counts})
    # clips per chord, all sessions added up
    totals = {chord: sum(counts[chord, s] for s in sessions) for chord in chords}

    # the table: each column is as wide as its longest name (and at least 5 characters)
    chord_w = max(len("chord"), *(len(c) for c in chords))
    widths = [max(len(s), 5) for s in sessions]
    # header row: "chord", every session name, then "total"
    print("chord".ljust(chord_w), *(s.rjust(w) for s, w in zip(sessions, widths)), "total".rjust(5), sep="  ")
    # one row per chord; "-" where the chord has no clips in that session
    for chord in chords:
        cells = (str(counts[chord, s] or "-").rjust(w) for s, w in zip(sessions, widths))
        print(chord.ljust(chord_w), *cells, str(totals[chord]).rjust(5), sep="  ")
    # total size: every clip is about the same size, so this is close enough
    megabytes = sum(c.stat().st_size for c in clips) / 1_000_000
    print(f"\n{len(clips)} clips, {len(chords)} chords, {len(sessions)} sessions, {megabytes:.0f} MB")

    # warnings: chords that need more clips, more sessions, or are far behind the biggest chord
    biggest = max(totals.values())
    for chord in chords:
        # how many sessions this chord has clips in
        n_sessions = sum(1 for s in sessions if counts[chord, s])
        if totals[chord] < TARGET_CLIPS:
            print(f"  {chord}: only {totals[chord]} clips (goal {TARGET_CLIPS})")
        if n_sessions < TARGET_SESSIONS:
            print(f"  {chord}: only {n_sessions} session(s) (goal {TARGET_SESSIONS})")
        if totals[chord] < IMBALANCE_RATIO * biggest:
            print(f"  {chord}: less than half as many clips as the biggest chord ({biggest})")

    # listening: play a few random clips of each chord (or just the one asked for)
    if args.listen:
        for chord in [args.chord] if args.chord else chords:
            # this chord's clips, from every session
            mine = [c for c in clips if c.parent.name == chord]
            # pick up to --listen of them at random
            for clip in random.sample(mine, min(args.listen, len(mine))):
                # say which file is playing, so you know which one to delete if it's bad
                print(f"\n{chord}: {clip.relative_to(PROJECT_DIR)}")
                play(clip)


# run main() only when this file is run directly (not when it's imported by another script)
if __name__ == "__main__":
    main()
