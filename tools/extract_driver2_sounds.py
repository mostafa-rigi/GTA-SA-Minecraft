#!/usr/bin/env python3
"""Extract the Driver 2 cop's own sounds from VOICES2.BLK's lumps and
decode them to WAV: his siren, his engine and his radio phrases.

Provenance (all from the REDRIVER 2 rebuild's own code, followed
exactly - see DRIVER2-STEP3-fa.md for the full map):
  - VOICES2.BLK is a 73-lump container (GAMESND.C LoadBankFromLump:
    u32 blockLimits[73]; lump i = bytes [limits[i], limits[i+1])).
    The slices in driver2-assets/slices/ are byte-exact lump cuts.
  - A lump is a sound bank fragment: u32 num_samples, num_samples x
    SAMPLE_DATA {u32 address, u32 length, u32 loop, u32 samplerate}
    (SOUND.C LoadSoundBankDynamic, DR2TYPES.H), then the sample data;
    addresses are relative to the data region after the table.
    LoadSoundBankDynamic APPENDS a lump's samples to the bank, so a
    bank sample index accumulates across lumps in load order.
  - LoadLevelSFX (GAMESND.C) for Chicago (GameLevel 0) fills bank 2
    in this order: lump 69, lump 29, lump 29 + cop_bank (cop_bank =
    missionNum % 4 + 1, so one of lumps 30..33).
  - The siren: CarHasSiren's general return for external model 0 is
    0x200, and StartGameSounds plays bank (0x200 >> 8) = 2, sample 0
    -> lump 69, sample 0.
  - Radio: DoSpeech plays phrase p at bank 2, sample p. The phrases
    the cop code speaks are in FELONY.C / COP_AI.C (see phrases below).
  - His engine: MapCarIndexToBank maps external model 0 to
    car_banks[0][1] = lump 4. Within a car bank the layers repeat in
    triples (GAMESND.C's car-noise code: sample 3k = the revs layer,
    3k+1 = the idle layer); the first triple is extracted.
  - Sample data is PSX VAG ADPCM: 16-byte frames (shift/filter byte,
    flags byte, 14 bytes of nibbles = 28 samples), decoded with the
    canonical predictor coefficients.

Outputs (driver2-assets/sounds/): WAV files + manifest.json with the
full provenance of every sample (lump, index, table fields).
The .ogg conversion for the game happens in the asset pipeline
(tools/gen_assets.py picks the files up from the repo's sounds dir).
"""
import json
import os
import struct
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.normpath(os.path.join(os.path.dirname(HERE), "..", "driver2-assets"))
SLICES = os.path.join(ASSETS, "slices")
OUT = os.path.join(ASSETS, "sounds")

# the phrases the cop code can speak (bank 2 sample numbers), read off
# the CopSay call sites: FELONY.C speaks 5, (rnd & 1) + 7, 6 and
# phrase + 15 with phrase < 3; COP_AI.C speaks 6, 9, 12, rnd % 2 + 10
# and phrase + 15 with phrase < 3. The whole spoken set is therefore
# {5..12, 15, 16, 17} - and bank 2 (1 + 6 + 11 = 18 samples) holds
# exactly that range.
COP_PHRASES = [5, 6, 7, 8, 9, 10, 11, 12, 15, 16, 17]
# phrases >= 7 live in the mission's cop_bank lump (29 + cop_bank).
# The port has no mission numbers; cop_bank 1 (lump 30) is taken as the
# force's voice set, and lumps 31..33 - the other missions' sets for
# the same phrase numbers - are recorded in the manifest as alternates.
COP_BANK_LUMP = 30


def load_slice(name):
    with open(os.path.join(SLICES, name), "rb") as f:
        return f.read()


def parse_bank(lump_bytes):
    n = struct.unpack_from("<I", lump_bytes, 0)[0]
    table = []
    for i in range(n):
        addr, length, loop, rate = struct.unpack_from("<IIII", lump_bytes, 4 + i * 16)
        table.append({"address": addr, "length": length, "loop": loop, "samplerate": rate})
    data_start = 4 + n * 16
    return table, lump_bytes[data_start:]


# ---------------------------------------------------------------------------
# PSX VAG ADPCM (the canonical decoder: 5 predictor filters, 28 samples
# per 16-byte frame; a frame whose flags byte is 7 ends the sample, 3 =
# loop end - the payload decodes the same way)
# ---------------------------------------------------------------------------
F = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]


def decode_vag(data):
    out = []
    s1 = s2 = 0.0
    for off in range(0, len(data) - 15, 16):
        shift = data[off] & 0xF
        filt = (data[off] >> 4) & 0xF
        if filt >= 5:
            filt = 0
        f0, f1 = F[filt]
        for j in range(28):
            byte = data[off + 2 + (j >> 1)]
            nib = (byte & 0xF) if j % 2 == 0 else (byte >> 4)
            if nib >= 8:
                nib -= 16
            val = ((nib << 12) >> shift) if shift <= 12 else 0
            s = val + (s1 * f0 + s2 * f1) / 64.0
            s2 = s1
            s1 = s
            out.append(max(-32768, min(32767, int(round(s)))))
    return out


def write_wav(path, samples, rate_hz):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate_hz)
        w.writeframes(struct.pack("<%dh" % len(samples), *samples))


def rate_hz(stored):
    """SAMPLE_DATA.samplerate as the rebuild consumes it (SOUND.C: the
    channel rate is samplerate * pitch / 44100 in SPU pitch units, and
    a stored value is the sample's own rate in Hz when it sits in the
    audio band; values in the low thousands are pitch units instead
    (Hz = v * 44100 / 4096). The manifest keeps the raw field either
    way, and the choice is recorded per sample."""
    if stored >= 4000:
        return stored, "hz"
    return int(round(stored * 44100 / 4096.0)), "pitch-units"


def main():
    os.makedirs(OUT, exist_ok=True)
    manifest = {"container": "VOICES2.BLK (73 lumps)", "lumps": {}, "bank2": [], "files": []}

    lumps = {}
    for ln in (69, 29, 30, 31, 32, 33, 4):
        raw = load_slice(f"voices_lump{ln}.bin")
        table, data = parse_bank(raw)
        lumps[ln] = (table, data)
        manifest["lumps"][ln] = {"bytes": len(raw), "num_samples": len(table),
                                 "samples": table}

    # bank 2 accumulates: lump 69, then lump 29, then lump 29+cop_bank
    bank2 = []  # (lump, index in lump)
    for ln in (69, 29):
        for i in range(len(lumps[ln][0])):
            bank2.append((ln, i))
    base_after_29 = len(bank2)
    for ln in (30, 31, 32, 33):
        manifest[f"bank2_copbank_{ln}"] = [(ln, i) for i in range(len(lumps[ln][0]))]
    manifest["bank2"] = bank2
    manifest["bank2_base_after_lump29"] = base_after_29

    def extract(lump_no, idx, out_name, role):
        table, data = lumps[lump_no]
        s = table[idx]
        raw = data[s["address"]:s["address"] + s["length"]]
        pcm = decode_vag(raw)
        hz, how = rate_hz(s["samplerate"])
        write_wav(os.path.join(OUT, out_name), pcm, hz)
        manifest["files"].append({
            "file": out_name, "role": role, "lump": lump_no, "sample": idx,
            "address": s["address"], "length": s["length"], "loop": s["loop"],
            "samplerate_raw": s["samplerate"], "rate_hz": hz, "rate_how": how,
            "frames": len(raw) // 16, "pcm_samples": len(pcm),
            "seconds": round(len(pcm) / hz, 3) if hz else None,
        })
        return s

    # the siren: bank 2 sample 0 = lump 69 sample 0
    extract(69, 0, "cop_siren.wav", "siren (bank 2 sample 0, CarHasSiren 0x200)")

    # his engine: lump 4 holds exactly one triple - revs (3k) and idle
    # (3k+1) are the layers the car-noise code plays; the third sample's
    # consumer was not found in the rebuild's car-noise paths, so it is
    # extracted and carried, not wired.
    extract(4, 0, "cop_engine_revs.wav", "engine revs layer (lump 4 sample 0)")
    extract(4, 1, "cop_engine_idle.wav", "engine idle layer (lump 4 sample 1)")
    extract(4, 2, "cop_engine_extra.wav",
            "lump 4 sample 2 - carried, consumer not found in the car-noise code")

    # radio phrases: bank 2 sample p, resolved through the accumulation;
    # phrases >= 7 sit in the cop_bank lump (see COP_BANK_LUMP above)
    for p in COP_PHRASES:
        if p < len(bank2):
            ln, idx = bank2[p]
        else:
            ln, idx = COP_BANK_LUMP, p - base_after_29
        extract(ln, idx, f"cop_radio_{p:02d}.wav", f"radio phrase {p} (bank 2 sample {p})")

    with open(os.path.join(OUT, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    print(json.dumps({k: manifest[k] for k in ("lumps",)}, indent=1)[:2000])
    print("files:", [f["file"] for f in manifest["files"]])


if __name__ == "__main__":
    main()
