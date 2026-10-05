# record_clip.py: record clips from the ESP32 chord recognizer over USB serial and save them as WAVs (phase 3)

# This script asks the ESP32 (running src/main.cpp) for a clip, receives the raw audio bytes, counts them
# to check nothing was dropped, and saves a 16 kHz, mono, 16-bit WAV. It runs on the computer, not the ESP32.

# How to use it (from the project folder, with the phase 3 firmware uploaded):
#   testing the connection (one clip, saved in data/raw/clips/ with the date and time, then played back):
#     .venv\Scripts\python scripts\record_clip.py                 record 5 s
#     .venv\Scripts\python scripts\record_clip.py --seconds 10    record 10 s
#     .venv\Scripts\python scripts\record_clip.py --no-play       don't play the clip afterwards
#   building the chord dataset (many takes of one chord, saved in data/raw/clips/<label>/, no playback):
#     .venv\Scripts\python scripts\record_clip.py --label C_major --count 30 --seconds 2
#       -> data/raw/clips/C_major/C_major_001.wav, C_major_002.wav, ...
#       running it again later carries on numbering (C_major_031.wav, ...) instead of overwriting
#     --countdown 2      seconds of "3, 2, 1" before each take (default 3, 0 = none)
#   any mode:
#     --port COM5        pick the serial port yourself (default: find the ESP32 automatically)
# Press Ctrl+C to stop a batch early; takes already saved are kept.
# Close PlatformIO's serial monitor first: only one program can have the port open at a time.

# How one recording travels over the serial port (must match the comments at the top of main.cpp):
#   computer -> ESP32:  'R' + how many samples it wants                                   =  5 bytes
#   ESP32 -> computer:  header: "CLIP" + sample rate + number of samples + clip number    = 16 bytes
#                       audio:  number of samples x 2 bytes (16-bit samples)
#                       footer: "DONE" + samples sent + I2S overflows + clipped samples   = 16 bytes
# every number is a 4-byte unsigned integer sent lowest byte first ("little-endian").

# ---------------------------------------------------------------------------------------------
# Libraries
# ---------------------------------------------------------------------------------------------

# reads the --seconds / --label / --count / ... options from the command line
import argparse
# packs and unpacks numbers to and from raw bytes (for the request, header and footer)
import struct
# sys.exit() to stop with an error code
import sys
# time.monotonic() to time the transfer and enforce timeouts, time.sleep() for the countdown
import time
# today's date and time, used in the file name of test clips
from datetime import datetime
# file paths that work the same on Windows, macOS and Linux
from pathlib import Path

# fast arrays of numbers: turns the raw bytes into samples and measures them
import numpy as np
# pyserial: talks to the serial port
import serial
# pyserial's helper that lists the serial ports on this computer
import serial.tools.list_ports
# writes the WAV files
import soundfile as sf

# ---------------------------------------------------------------------------------------------
# Settings
# ---------------------------------------------------------------------------------------------

# the start marker the ESP32 sends before each clip's header
START_MARKER = b"CLIP"
# the end marker the ESP32 sends right after the audio
END_MARKER = b"DONE"
# the header and footer are both 16 bytes: a 4-byte marker and three 4-byte numbers
HEADER_SIZE = 16
FOOTER_SIZE = 16
# bytes per sample (16-bit audio = 2 bytes)
BYTES_PER_SAMPLE = 2
# the sample rate we expect; the header says what the ESP32 really used
SAMPLE_RATE = 16000
# USB vendor ID of Espressif boards, used to find the ESP32's port automatically
ESPRESSIF_VID = 0x303A
# the project folder (this file is in <project>/scripts/)
PROJECT_DIR = Path(__file__).resolve().parent.parent
# where the WAV files go
OUT_DIR = PROJECT_DIR / "data" / "raw" / "clips"
# seconds to wait for the header after asking for a clip, before asking again
HEADER_TIMEOUT = 2.0
# how many times to ask (the board may be rebooting when the port opens, and miss the first request)
REQUEST_TRIES = 3
# if no audio bytes arrive for this many seconds in the middle of a clip, give up on the rest
STALL_TIMEOUT = 1.0
# in a batch, stop after this many bad takes in a row (something needs fixing, retrying won't help)
MAX_BAD_IN_A_ROW = 3
# characters Windows doesn't allow in file names, so they can't be used in a label
BAD_LABEL_CHARS = '<>:"/\\|?*'


# ---------------------------------------------------------------------------------------------
# find_port: return the name of the ESP32's serial port (e.g. "COM5"), or stop with a helpful message
# ---------------------------------------------------------------------------------------------
def find_port():
    # every serial port on this computer
    ports = list(serial.tools.list_ports.comports())
    # the ones made by Espressif
    esp_ports = [p for p in ports if p.vid == ESPRESSIF_VID]
    # exactly one ESP32 found: use it
    if len(esp_ports) == 1:
        return esp_ports[0].device
    # otherwise, if there's only one port of any kind, it's probably the board
    if len(ports) == 1:
        return ports[0].device
    # can't tell which one to use: list them and stop
    print("Couldn't pick the ESP32's serial port automatically. Ports found:")
    for p in ports:
        print(f"  {p.device}: {p.description}")
    sys.exit("Run again with --port <name>, e.g. --port COM5")


# ---------------------------------------------------------------------------------------------
# open_port: open the serial port without resetting the ESP32
#   name - port name, e.g. "COM5"
# ---------------------------------------------------------------------------------------------
def open_port(name):
    # create the port object without opening it yet, so DTR/RTS can be set first
    ser = serial.Serial()
    # which port
    ser.port = name
    # ignored by the ESP32-S3's USB serial (it always runs at full USB speed), but pyserial needs a value
    ser.baudrate = 115200
    # read() gives up after 0.5 s if not enough bytes arrive
    ser.timeout = 0.5
    # keep the DTR and RTS control lines low: some combinations of them reset the ESP32
    ser.dtr = False
    ser.rts = False
    # now open it
    ser.open()
    return ser


# ---------------------------------------------------------------------------------------------
# wait_for_marker: read bytes until the start marker appears, and return how many bytes came before it
# (e.g. boot messages), or None if it didn't show up within `timeout` seconds
#   ser     - the open serial port
#   timeout - seconds to wait
# ---------------------------------------------------------------------------------------------
def wait_for_marker(ser, timeout):
    # the last few bytes received, to compare against the marker
    window = b""
    # bytes skipped before the marker
    skipped = 0
    # stop looking at this time
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        # read one byte (returns b"" if none arrives within ser.timeout)
        byte = ser.read(1)
        # nothing came: keep waiting until the deadline
        if not byte:
            continue
        # add the byte and keep only the last 4 (the marker's length)
        window = (window + byte)[-len(START_MARKER):]
        # found it: report how many bytes came before the marker
        if window == START_MARKER:
            return skipped - (len(START_MARKER) - 1)
        # count the byte as skipped (the marker's own bytes are subtracted above)
        skipped += 1
    # timed out
    return None


# ---------------------------------------------------------------------------------------------
# request_clip: ask the ESP32 for a clip and wait for its header; returns the header's numbers
#   ser     - the open serial port
#   samples - how many samples to ask for
# ---------------------------------------------------------------------------------------------
def request_clip(ser, samples):
    for attempt in range(1, REQUEST_TRIES + 1):
        # throw away anything already waiting in the receive buffer (old data, boot messages)
        ser.reset_input_buffer()
        # the request: the letter R, then the sample count as a 4-byte little-endian number ("<I")
        ser.write(b"R" + struct.pack("<I", samples))
        # wait for the start marker
        skipped = wait_for_marker(ser, HEADER_TIMEOUT)
        # got it
        if skipped is not None:
            # stray bytes before the marker are normal right after a reset (boot messages), so just mention them
            if skipped > 0:
                print(f"(skipped {skipped} bytes before the start marker)")
            # the other 12 header bytes: sample rate, number of samples, clip number
            rest = ser.read(HEADER_SIZE - len(START_MARKER))
            # the header got cut off: something is wrong with the connection
            if len(rest) < HEADER_SIZE - len(START_MARKER):
                sys.exit("Header was cut short. Check the USB cable and try again.")
            # unpack the three 4-byte little-endian numbers
            return struct.unpack("<III", rest)
        # no marker yet: say so and ask again
        print(f"No reply from the ESP32 (try {attempt} of {REQUEST_TRIES})...")
    # never answered
    sys.exit("The ESP32 didn't answer. Is the phase 3 firmware uploaded, and the serial monitor closed?")


# ---------------------------------------------------------------------------------------------
# receive_audio: read the clip's audio bytes, showing progress; returns the bytes and how long it took
#   ser            - the open serial port
#   expected_bytes - how many bytes the header said would come
# ---------------------------------------------------------------------------------------------
def receive_audio(ser, expected_bytes):
    # collects the received chunks (joining them once at the end is faster than adding as we go)
    chunks = []
    # bytes received so far
    received = 0
    # when the audio started arriving
    start = time.monotonic()
    # when the last byte arrived, to spot a stalled stream
    last_data = start
    while received < expected_bytes:
        # read whatever is waiting (at least 1 byte, at most what's left of the clip)
        chunk = ser.read(min(max(ser.in_waiting, 1), expected_bytes - received))
        # got some bytes
        if chunk:
            chunks.append(chunk)
            received += len(chunk)
            last_data = time.monotonic()
            # progress on one line ("\r" goes back to the start of the line instead of a new line)
            print(f"\rreceiving: {received:,} / {expected_bytes:,} bytes", end="", flush=True)
        # nothing arrived for too long: the stream stalled, stop with what we have
        elif time.monotonic() - last_data > STALL_TIMEOUT:
            break
    # finish the progress line
    print()
    # all chunks as one block of bytes, and the time from first to last byte
    return b"".join(chunks), time.monotonic() - start


# ---------------------------------------------------------------------------------------------
# record_take: record one clip, check that it arrived complete, and print a short report
#   ser    - the open serial port
#   wanted - how many samples to ask for
# returns (samples as a numpy array, sample rate, list of problems found; empty = all good)
# ---------------------------------------------------------------------------------------------
def record_take(ser, wanted):
    # ask for the clip and read the header
    rate, samples, clip_no = request_clip(ser, wanted)
    # the ESP32 caps clips at MAX_CLIP_SECONDS, so it may send fewer samples than we asked for
    if samples != wanted:
        print(f"(the ESP32 will send {samples:,} samples instead of {wanted:,})")
    # how many audio bytes to expect = samples x 2
    expected_bytes = samples * BYTES_PER_SAMPLE
    # receive the audio
    audio, elapsed = receive_audio(ser, expected_bytes)
    # the footer that should follow the audio
    footer = ser.read(FOOTER_SIZE)

    # collects a short description of every problem found
    problems = []

    # 1) count: compare received bytes with the expected count
    print(f"clip #{clip_no}: expected {expected_bytes:,} bytes, received {len(audio):,}")
    if len(audio) < expected_bytes:
        problems.append(f"DROPPED DATA: {expected_bytes - len(audio):,} bytes never arrived")

    # 2) the footer must come right after the audio; if it doesn't, bytes were lost or extra bytes got in
    if len(footer) == FOOTER_SIZE and footer[:4] == END_MARKER:
        # unpack the ESP32's own counts
        sent, overflows, clipped = struct.unpack("<III", footer[4:])
        print(f"ESP32 says: sent {sent:,} samples, {overflows} I2S overflows, {clipped:,} clipped samples")
        # the ESP32 couldn't write everything (the computer wasn't reading fast enough)
        if sent != samples:
            problems.append(f"the ESP32 only managed to send {sent:,} of {samples:,} samples")
        # the ESP32 lost audio before sending it: this is heard as gaps or clicks
        if overflows:
            problems.append(f"{overflows} I2S overflows (gaps): raise DMA_BUF_COUNT in main.cpp, or switch to WiFi")
        # more than 0.1% of samples clipped: distortion
        if clipped > samples * 0.001:
            problems.append(f"{clipped:,} samples clipped (too loud): raise SAMPLE_SHIFT in main.cpp, or play softer")
    else:
        problems.append("end marker not found right after the audio: bytes were lost, or stray text "
                        "got into the stream (is anything printing to Serial?)")

    # 3) speed: the ESP32 can only send audio as fast as it records it, so a clip of N seconds should take
    #    about N seconds to arrive. A big difference means the sample rate isn't what the header says.
    duration = samples / rate
    print(f"{duration:.2f} s of audio arrived in {elapsed:.2f} s")
    if len(audio) == expected_bytes and abs(elapsed - duration) > max(0.05 * duration, 0.2):
        problems.append(f"timing is off ({elapsed:.2f} s for {duration:.2f} s of audio): "
                        f"the real sample rate may not be {rate} Hz, so playback speed would be wrong")

    # if the audio was cut short by an odd number of bytes, drop the half sample at the end
    audio = audio[: len(audio) // BYTES_PER_SAMPLE * BYTES_PER_SAMPLE]
    # turn the bytes into numbers: "<i2" = little-endian signed 16-bit integers
    pcm = np.frombuffer(audio, dtype="<i2")

    # a few numbers about the sound itself (skipped if nothing arrived)
    if len(pcm):
        # average sample = the mic's DC offset (a constant shift that isn't sound)
        dc = pcm.mean()
        # loudest single sample, measured from the DC offset
        peak = np.abs(pcm - dc).max()
        # RMS loudness, measured from the DC offset
        rms = np.sqrt(np.mean((pcm - dc) ** 2))
        print(f"level: peak {peak:.0f}, rms {rms:.0f}, DC offset {dc:.0f} (out of 32767)")

    # list the problems, if any
    for p in problems:
        print(f"  PROBLEM: {p}")
    return pcm, rate, problems


# ---------------------------------------------------------------------------------------------
# next_number: find the next free take number for a label, so new takes never overwrite old ones
#   folder - the label's folder, e.g. data/raw/clips/C_major
#   label  - the label, e.g. "C_major"
# ---------------------------------------------------------------------------------------------
def next_number(folder, label):
    # the highest number used so far (0 = none yet)
    highest = 0
    # every file named like C_major_001.wav in the folder
    for f in folder.glob(f"{label}_*.wav"):
        # the part after the last "_", e.g. "001"
        number = f.stem.rsplit("_", 1)[-1]
        # only count it if it really is a number
        if number.isdigit():
            highest = max(highest, int(number))
    # one more than the highest
    return highest + 1


# ---------------------------------------------------------------------------------------------
# countdown: print "3.. 2.. 1.. GO" so you know when to play
#   seconds - how many seconds to count down (0 = no countdown)
# ---------------------------------------------------------------------------------------------
def countdown(seconds):
    # count down one second at a time, on one line
    for s in range(seconds, 0, -1):
        print(f"{s}.. ", end="", flush=True)
        time.sleep(1)
    # the request goes out right after this, and recording starts within a few milliseconds
    print("GO")


# ---------------------------------------------------------------------------------------------
# play: play a WAV file and wait until it finishes
#   path - the WAV file
# ---------------------------------------------------------------------------------------------
def play(path):
    try:
        # winsound is built into Python on Windows
        import winsound
        print("playing...")
        # plays the file and waits until it finishes
        winsound.PlaySound(str(path), winsound.SND_FILENAME)
    except ImportError:
        # not on Windows: open the file in any audio player instead
        print(f"open {path} in an audio player to listen to it")


# ---------------------------------------------------------------------------------------------
# main: read the options, then record one test clip or a batch of labelled takes
# ---------------------------------------------------------------------------------------------
def main():
    # command line options
    parser = argparse.ArgumentParser(description="Record clips from the ESP32 and save them as WAVs.")
    parser.add_argument("--seconds", type=float, default=5.0, help="clip length in seconds (default 5, max 60)")
    parser.add_argument("--label", help="chord name, e.g. C_major: saves numbered takes in data/raw/clips/<label>/")
    parser.add_argument("--count", type=int, default=1, help="how many takes to record (default 1)")
    parser.add_argument("--countdown", type=int, default=3, help="seconds of countdown before each take (default 3)")
    parser.add_argument("--port", help="serial port, e.g. COM5 (default: find the ESP32 automatically)")
    parser.add_argument("--no-play", action="store_true", help="don't play a single test clip after saving it")
    args = parser.parse_args()

    # a label becomes part of the folder and file names, so it can't contain characters Windows forbids
    if args.label and any(c in BAD_LABEL_CHARS for c in args.label):
        sys.exit(f"--label can't contain any of these characters: {BAD_LABEL_CHARS}")
    # where this run's files go: the label's own folder, or the main clips folder for test clips
    folder = OUT_DIR / args.label if args.label else OUT_DIR
    # make sure it exists
    folder.mkdir(parents=True, exist_ok=True)
    # only play back a single test clip; when building the dataset it would just slow you down
    should_play = not args.label and args.count == 1 and not args.no_play

    # which port to use
    port = args.port or find_port()
    # samples to ask for = seconds x samples per second
    wanted = round(args.seconds * SAMPLE_RATE)
    # takes saved so far
    saved = 0
    # bad takes in a row
    bad_in_a_row = 0
    # test clips that had problems but were saved anyway (so you can listen to them)
    kept_bad = 0
    print(f"Recording {args.count} x {args.seconds:g} s from {port} into {folder.relative_to(PROJECT_DIR)}")

    # open the port once for the whole batch; "with" closes it again at the end, even after an error
    with open_port(port) as ser:
        try:
            while saved < args.count:
                # which take this is
                print(f"\n--- take {saved + 1} of {args.count} ---")
                # give you time to get ready
                countdown(args.countdown)
                # record and check it
                pcm, rate, problems = record_take(ser, wanted)

                if problems:
                    # one more bad take in a row
                    bad_in_a_row += 1
                    # too many in a row: something needs fixing first
                    if bad_in_a_row >= MAX_BAD_IN_A_ROW:
                        print(f"\n{MAX_BAD_IN_A_ROW} bad takes in a row: fix the problem above and run again.")
                        break
                    # in a batch, a bad take is thrown away and recorded again, so the dataset only gets clean clips
                    if args.label:
                        print("not saved, recording this take again")
                        continue
                    # a test clip is still saved below, so you can listen to what went wrong
                    kept_bad += 1
                else:
                    # a good take resets the bad streak
                    bad_in_a_row = 0
                    # test clips get the all-clear message (a batch would print it hundreds of times)
                    if not args.label:
                        print("OK: every byte arrived and the timing matches. Listen for clicks or speed changes.")

                # file name: numbered for a label (C_major_001.wav), dated for a test clip
                if args.label:
                    path = folder / f"{args.label}_{next_number(folder, args.label):03d}.wav"
                else:
                    path = folder / f"clip_{datetime.now():%Y-%m-%d_%H-%M-%S}.wav"
                # write a mono, 16-bit WAV at the ESP32's sample rate
                sf.write(path, pcm, rate, subtype="PCM_16")
                print(f"saved {path.relative_to(PROJECT_DIR)}")
                saved += 1

                # play it back (single test clips only)
                if should_play:
                    play(path)
        # Ctrl+C stops the batch; everything saved so far stays saved
        except KeyboardInterrupt:
            print("\nstopped")

    # summary
    print(f"\n{saved} of {args.count} takes saved in {folder.relative_to(PROJECT_DIR)}")
    # exit code 1 if not everything was recorded cleanly (useful if another script runs this one)
    sys.exit(0 if saved == args.count and not kept_bad else 1)


# run main() only when this file is run directly (not when it's imported by another script)
if __name__ == "__main__":
    main()
