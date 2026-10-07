# train_model.py: train a chord classifier on the labelled clips (phase 5, first model)

# This trains two models on the clips made by slice_takes.py and prints how well each one does:
#   1. a baseline: the clip's "chroma" (how much of each of the 12 notes C, C#, D ... B it contains, averaged
#      over the 2 seconds) fed to a logistic regression. Simple, fast, and easy to see why it works.
#   2. a small CNN (convolutional neural network) on the clip's log-mel spectrogram (a picture of loudness
#      at each pitch over time). Small enough to run on the ESP32-S3 later.
# It runs on the computer, not the ESP32.
#
# How to use it (from the project folder):
#   .venv\Scripts\python scripts\train_model.py                          train both, test on the end of every take
#   .venv\Scripts\python scripts\train_model.py --test-session 2026-10-09   test on a whole session instead
#     --test-fraction 0.2   fraction of each chord folder kept for testing (default 0.2)
#     --epochs 40           how many times the CNN sees the training clips (default 40)
#
# How the clips are split into training and test clips:
#   The test clips must be ones the model never saw while training, or the score means nothing. Clips cut
#   from the same take one after another sound almost the same, so a random split would put near-copies on
#   both sides. Instead, the last 20% of each chord folder (the end of its take) is kept for testing.
#   Once there are several sessions, --test-session holds out a whole day, which is the honest test: will it
#   recognise a chord played on a different day, with the guitar retuned?
#
# where the files are:
#   clips in:    data/raw/<session>/<chord>/clip_###.wav
#   model out:   models/chord_cnn.keras  and  models/labels.txt  (the chord names, in the model's output order)

# ---------------------------------------------------------------------------------------------
# Libraries
# ---------------------------------------------------------------------------------------------

# reads the --test-session / --test-fraction / --epochs options from the command line
import argparse
# hides TensorFlow's start-up messages (must be set before TensorFlow is imported)
import os
os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")

# fast arrays of numbers
import numpy as np
# audio analysis: chroma and mel spectrograms
import librosa
# reads WAV files
import soundfile as sf
# the baseline model, and the scores
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import accuracy_score, confusion_matrix
# the neural network library (Keras runs on top of TensorFlow)
import keras
from keras import layers

# the project folder (from record_clip.py)
from record_clip import PROJECT_DIR

# ---------------------------------------------------------------------------------------------
# Settings
# ---------------------------------------------------------------------------------------------

# where slice_takes.py saves the clips
RAW_DIR = PROJECT_DIR / "data" / "raw"
# where the trained model is saved
MODEL_DIR = PROJECT_DIR / "models"
# samples per second of every clip (the ESP32 records at 16000, see SAMPLE_RATE in main.cpp)
SAMPLE_RATE = 16000
# every clip is cut to (or padded to) exactly this many seconds, so they all give the same size picture
CLIP_SECONDS = 2.0
# the 12 note names, in chroma order
NOTES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

# --- log-mel spectrogram settings (the CNN's input). Keep these small: the ESP32 has to compute them too ---
# samples per FFT window: 1024 samples = 64 ms, which separates pitches about 16 Hz apart
N_FFT = 1024
# samples between the starts of two windows: 320 = 20 ms, so 2 s gives 101 time steps
HOP = 320
# how many pitch bands (rows of the picture)
N_MELS = 64
# lowest and highest pitch kept, in Hz. A guitar's low E is 82 Hz; above 4 kHz is mostly pick noise
F_MIN = 70
F_MAX = 4000
# quietest level kept, in dB below the clip's loudest point (anything quieter is treated as silence)
TOP_DB = 80

# makes the shuffling and the network's starting weights the same every run, so results are repeatable
SEED = 0


# ---------------------------------------------------------------------------------------------
# load_clips: read every clip and work out which ones are for training and which for testing
#   test_session  - a session name to hold out completely, or None to hold out the end of every folder
#   test_fraction - with no test_session: the fraction of each chord folder (its last clips) kept for testing
# returns (list of audio arrays, list of chord names, list of True/False "is a test clip")
# ---------------------------------------------------------------------------------------------
def load_clips(test_session, test_fraction):
    # every clip's audio, chord name, and whether it's a test clip
    audio, chords, is_test = [], [], []
    # how many samples every clip should have
    clip_len = int(CLIP_SECONDS * SAMPLE_RATE)
    # each chord folder: data/raw/<session>/<chord>/ (skips the old data/raw/clips folder, which has no chord)
    for folder in sorted(RAW_DIR.glob("*/*/")):
        # the clips in this folder, in number order (which is also the order they were played in)
        paths = sorted(folder.glob("clip_*.wav"))
        # not a chord folder, or empty
        if not paths:
            continue
        # the folder's chord and session are its names
        chord = folder.name
        session = folder.parent.name
        # the position in this folder where the test clips start (only used with no test_session)
        first_test = int(round(len(paths) * (1 - test_fraction)))
        for i, path in enumerate(paths):
            # read the clip as floats from -1.0 to 1.0
            samples, rate = sf.read(path, dtype="float32")
            # a clip at a different rate would give a different size picture: resample it
            if rate != SAMPLE_RATE:
                samples = librosa.resample(samples, orig_sr=rate, target_sr=SAMPLE_RATE)
            # remove the mic's DC offset (a constant shift that isn't sound)
            samples -= samples.mean()
            # make it exactly clip_len long: cut extra samples off the end, or add silence
            samples = np.pad(samples[:clip_len], (0, max(0, clip_len - len(samples))))
            # keep it
            audio.append(samples)
            chords.append(chord)
            # is it a test clip? either its whole session is held out, or it's near the end of its folder
            if test_session:
                is_test.append(session == test_session)
            else:
                is_test.append(i >= first_test)
    return audio, chords, np.array(is_test)


# ---------------------------------------------------------------------------------------------
# chroma_features: the baseline's input, 12 numbers per clip (how much of each note name it contains)
#   samples - one clip's audio
# returns an array of 12 numbers from 0 to 1
# ---------------------------------------------------------------------------------------------
def chroma_features(samples):
    # chroma over time: 12 rows (notes) x time steps. Every C on the guitar (any octave) adds to the C row.
    # n_fft=4096 (256 ms windows) separates low notes, which are only a few Hz apart
    chroma = librosa.feature.chroma_stft(y=samples, sr=SAMPLE_RATE, n_fft=4096, hop_length=1024)
    # average over time: one number per note for the whole clip
    profile = chroma.mean(axis=1)
    # scale so the strongest note is 1, so loud and soft strums give the same numbers
    return profile / (profile.max() + 1e-9)


# ---------------------------------------------------------------------------------------------
# mel_features: the CNN's input, a log-mel spectrogram (pitch bands x time steps, values 0 to 1)
#   samples - one clip's audio
# returns an array of N_MELS x 101 numbers
# ---------------------------------------------------------------------------------------------
def mel_features(samples):
    # loudness (power) in each of N_MELS pitch bands, every HOP samples
    mel = librosa.feature.melspectrogram(y=samples, sr=SAMPLE_RATE, n_fft=N_FFT, hop_length=HOP,
                                         n_mels=N_MELS, fmin=F_MIN, fmax=F_MAX)
    # convert to decibels relative to the clip's loudest point (0 dB), with nothing quieter than -TOP_DB.
    # Ears hear loudness on a log scale, and this also makes loud and soft strums look alike
    db = librosa.power_to_db(mel, ref=np.max, top_db=TOP_DB)
    # shift and scale from -80..0 dB to 0..1, the range neural networks train best on
    return (db + TOP_DB) / TOP_DB


# ---------------------------------------------------------------------------------------------
# augment: make extra, slightly different copies of the training clips, so the CNN doesn't memorise them
#   audio  - list of training clips
#   labels - their chord numbers
#   copies - how many extra copies of each clip to make
#   rng    - the random number generator
# returns (the original clips plus the copies, and their labels)
# No pitch shifting: it would turn one chord into another, and the label would be wrong.
# ---------------------------------------------------------------------------------------------
def augment(audio, labels, copies, rng):
    # start with the originals
    out_audio, out_labels = list(audio), list(labels)
    for _ in range(copies):
        for samples, label in zip(audio, labels):
            # a random volume between half and double (a softer or harder strum, closer or further from the mic)
            louder = samples * rng.uniform(0.5, 2.0)
            # quiet random hiss, like a noisier room
            noise = rng.normal(0, rng.uniform(0.0005, 0.005), len(samples)).astype(np.float32)
            # slide the clip later by up to 0.1 s (the strum lands slightly after the clip starts)
            shift = rng.integers(0, int(0.1 * SAMPLE_RATE))
            moved = np.roll(louder + noise, shift)
            # the samples that wrapped round to the start become silence
            moved[:shift] = 0
            # keep it, with the same label
            out_audio.append(moved)
            out_labels.append(label)
    return out_audio, np.array(out_labels)


# ---------------------------------------------------------------------------------------------
# build_cnn: make the small convolutional network
#   input_shape - the size of one spectrogram, (N_MELS, time steps, 1)
#   n_chords    - how many chords it chooses between
# returns the untrained Keras model
# ---------------------------------------------------------------------------------------------
def build_cnn(input_shape, n_chords):
    return keras.Sequential([
        # one spectrogram: pitch bands x time steps x 1 "colour" channel
        keras.Input(shape=input_shape),
        # 8 small 3x3 pattern detectors slide over the picture (e.g. "a band that's loud for a while")
        layers.Conv2D(8, 3, padding="same", activation="relu"),
        # halve the picture's size, keeping the strongest response in each 2x2 square
        layers.MaxPooling2D(2),
        # 16 detectors that combine the first layer's patterns into bigger ones
        layers.Conv2D(16, 3, padding="same", activation="relu"),
        layers.MaxPooling2D(2),
        # 32 detectors for bigger patterns again (e.g. "these 3 notes together")
        layers.Conv2D(32, 3, padding="same", activation="relu"),
        layers.MaxPooling2D(2),
        # lay everything out as one long list of numbers. Unlike averaging, this keeps WHERE (which pitch)
        # each pattern was found, and pitch is what tells chords apart
        layers.Flatten(),
        # while training, randomly switch off 30% of those numbers, so it can't rely on any single one
        layers.Dropout(0.3),
        # one score per chord; softmax turns the scores into probabilities that add up to 1
        layers.Dense(n_chords, activation="softmax"),
    ])


# ---------------------------------------------------------------------------------------------
# print_results: print a model's test accuracy and its confusion matrix
#   name      - the model's name for the heading
#   true      - the real chord number of every test clip
#   predicted - the chord number the model guessed for every test clip
#   names     - the chord names, in number order
# ---------------------------------------------------------------------------------------------
def print_results(name, true, predicted, names):
    # fraction of test clips it got right
    print(f"\n{name}: {accuracy_score(true, predicted):.1%} of {len(true)} test clips correct")
    # rows = the real chord, columns = what the model said. Everything off the diagonal is a mistake
    matrix = confusion_matrix(true, predicted, labels=range(len(names)))
    # every column as wide as the longest chord name (and at least 4 characters)
    w = max(4, *(len(n) for n in names))
    print("  real \\ said  " + "".join(n.rjust(w + 1) for n in names))
    for chord, row in zip(names, matrix):
        print("  " + chord.ljust(12) + "".join(str(c).rjust(w + 1) for c in row))


# ---------------------------------------------------------------------------------------------
# main: load the clips, train and test both models, save the CNN
# ---------------------------------------------------------------------------------------------
def main():
    # command line options
    parser = argparse.ArgumentParser(description="Train a chord classifier on the labelled clips.")
    parser.add_argument("--test-session", help="hold out this whole session for testing")
    parser.add_argument("--test-fraction", type=float, default=0.2,
                        help="with no --test-session: fraction of each chord folder kept for testing (default 0.2)")
    parser.add_argument("--epochs", type=int, default=40, help="training passes for the CNN (default 40)")
    args = parser.parse_args()

    # the random number generators, seeded so every run gives the same result
    rng = np.random.default_rng(SEED)
    keras.utils.set_random_seed(SEED)

    # ---- load ----
    audio, chords, is_test = load_clips(args.test_session, args.test_fraction)
    # nothing to train on
    if not audio:
        print(f"No clips found in {RAW_DIR.relative_to(PROJECT_DIR)}. Run slice_takes.py first.")
        return
    # the chord names in alphabetical order; a chord's number is its position in this list
    names = sorted(set(chords))
    # every clip's chord as a number (what the models actually predict)
    labels = np.array([names.index(c) for c in chords])
    # a held-out session that doesn't exist (typo?) leaves no test clips
    if not is_test.any() or is_test.all():
        print("The split left no training clips or no test clips. Check --test-session / --test-fraction.")
        return
    # report the split
    print(f"{len(audio)} clips, chords: {', '.join(names)}")
    for i, name in enumerate(names):
        print(f"  {name}: {np.sum((labels == i) & ~is_test)} training, {np.sum((labels == i) & is_test)} test")
    # the training and test clips' audio, as separate lists
    train_audio = [a for a, t in zip(audio, is_test) if not t]
    test_audio = [a for a, t in zip(audio, is_test) if t]
    train_labels, test_labels = labels[~is_test], labels[is_test]

    # ---- model 1: chroma + logistic regression ----
    # 12 numbers per clip
    train_chroma = np.array([chroma_features(a) for a in train_audio])
    test_chroma = np.array([chroma_features(a) for a in test_audio])
    # show each chord's average chroma: a C chord (C E G) should be strongest on C, E and G
    print("\naverage chroma per chord (how much of each note it contains, # = 0.1):")
    for i, name in enumerate(names):
        profile = train_chroma[train_labels == i].mean(axis=0)
        # the 3 strongest notes, which should be the chord's notes
        top = ", ".join(NOTES[n] for n in np.argsort(profile)[::-1][:3])
        print(f"  {name} (strongest: {top})")
        for note, value in zip(NOTES, profile):
            print(f"    {note:<3}{'#' * int(round(value * 10))}")
    # logistic regression learns one weight per note per chord, then picks the chord with the highest total
    baseline = LogisticRegression(max_iter=1000)
    baseline.fit(train_chroma, train_labels)
    print_results("baseline (chroma + logistic regression)", test_labels, baseline.predict(test_chroma), names)

    # ---- model 2: log-mel spectrogram + small CNN ----
    # 2 extra, slightly changed copies of every training clip (3x as much training data)
    aug_audio, aug_labels = augment(train_audio, train_labels, copies=2, rng=rng)
    # spectrograms, with a 1-long "channel" axis on the end, which Conv2D expects
    train_mel = np.array([mel_features(a) for a in aug_audio])[..., np.newaxis]
    test_mel = np.array([mel_features(a) for a in test_audio])[..., np.newaxis]
    # build it and print its layers and size
    cnn = build_cnn(train_mel.shape[1:], len(names))
    cnn.summary()
    # adam = the usual way of adjusting the weights; the loss measures how wrong the probabilities are
    cnn.compile(optimizer="adam", loss="sparse_categorical_crossentropy", metrics=["accuracy"])
    # shuffle the training clips, so the 15% held back for validation has every chord in it
    order = rng.permutation(len(train_mel))
    # train. Each epoch prints training accuracy and val_accuracy (on 15% of training clips it doesn't learn
    # from). If accuracy keeps rising but val_accuracy doesn't, it's memorising instead of learning
    cnn.fit(train_mel[order], aug_labels[order], epochs=args.epochs, batch_size=16,
            validation_split=0.15, verbose=2)
    # the CNN's guess for every test clip: the chord with the highest probability
    predicted = cnn.predict(test_mel, verbose=0).argmax(axis=1)
    print_results("CNN (log-mel spectrogram)", test_labels, predicted, names)

    # ---- save ----
    MODEL_DIR.mkdir(exist_ok=True)
    # the network (layers and learned weights)
    cnn.save(MODEL_DIR / "chord_cnn.keras")
    # the chord names, one per line, in the order of the network's outputs
    (MODEL_DIR / "labels.txt").write_text("\n".join(names) + "\n")
    print(f"\nsaved {(MODEL_DIR / 'chord_cnn.keras').relative_to(PROJECT_DIR)} and labels.txt")


# run main() only when this file is run directly (not when it's imported by another script)
if __name__ == "__main__":
    main()
