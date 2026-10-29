#!/usr/bin/env python3
"""Compare deux fichiers WAV échantillon par échantillon avec une tolérance.

Usage : compare_wav.py <reference.wav> <candidat.wav> [--tolerance 1e-5]

Formats acceptés : PCM 16/24/32 bits et IEEE float 32 bits, en-tête classique ou
WAVE_FORMAT_EXTENSIBLE. Le nombre de canaux, la fréquence et le masque de canaux
doivent être identiques. Code de retour 0 si identique dans la tolérance, 1 sinon.
Bibliothèque standard uniquement.
"""
import argparse
import struct
import sys


def read_wav(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path} : en-tête RIFF/WAVE invalide")
    pos = 12
    fmt = None
    pcm = None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            tag, channels, rate, _, _, bits = struct.unpack_from("<HHIIHH", body, 0)
            mask = 0
            is_float = tag == 3
            if tag == 0xFFFE:
                mask = struct.unpack_from("<I", body, 20)[0]
                is_float = body[24:26] == b"\x03\x00"
            fmt = (channels, rate, bits, mask, is_float)
        elif cid == b"data":
            pcm = body
        pos += 8 + size + (size & 1)
    if fmt is None or pcm is None:
        raise ValueError(f"{path} : chunks fmt/data absents")
    channels, rate, bits, mask, is_float = fmt
    n = len(pcm) // (bits // 8)
    if is_float:
        samples = list(struct.unpack_from(f"<{n}f", pcm, 0))
    elif bits == 16:
        samples = [s / 32768.0 for s in struct.unpack_from(f"<{n}h", pcm, 0)]
    elif bits == 24:
        samples = []
        for i in range(n):
            b = pcm[3 * i:3 * i + 3]
            v = int.from_bytes(b, "little", signed=True)
            samples.append(v / 8388608.0)
    elif bits == 32:
        samples = [s / 2147483648.0 for s in struct.unpack_from(f"<{n}i", pcm, 0)]
    else:
        raise ValueError(f"{path} : profondeur non prise en charge ({bits})")
    return {"channels": channels, "rate": rate, "mask": mask, "samples": samples}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reference")
    ap.add_argument("candidate")
    ap.add_argument("--tolerance", type=float, default=1e-5)
    args = ap.parse_args()

    ref = read_wav(args.reference)
    cand = read_wav(args.candidate)
    for key in ("channels", "rate", "mask"):
        if ref[key] != cand[key]:
            print(f"ECART : {key} différent ({ref[key]} vs {cand[key]})")
            return 1
    if len(ref["samples"]) != len(cand["samples"]):
        print(f"ECART : nombre d'échantillons différent ({len(ref['samples'])} vs {len(cand['samples'])})")
        return 1
    worst = 0.0
    worst_index = -1
    for i, (a, b) in enumerate(zip(ref["samples"], cand["samples"])):
        d = abs(a - b)
        if d > worst:
            worst, worst_index = d, i
    if worst > args.tolerance:
        frame, ch = divmod(worst_index, ref["channels"])
        print(f"ECART : écart max {worst:.3e} à la trame {frame} canal {ch} (tolérance {args.tolerance:.1e})")
        return 1
    print(f"OK : {len(ref['samples'])} échantillons, écart max {worst:.3e}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
