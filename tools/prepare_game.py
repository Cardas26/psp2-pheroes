#!/usr/bin/env python3
import argparse, hashlib, os, struct, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
TABLES = os.path.join(HERE, "prepare_game")
ZIP_SHA256 = "07f558b3a645ab345d37e881dd0e64ce4e7da5f3df602612ffcaea6bce4d6f72"
EXE_SHA256 = "9a25b56b2174c4a0f1743813a3394758207e18838987fe3c4f5058bf4e8772d0"
IMAGE_MD5 = "d4a6b6fc53b0b6eecb1c4f0fd6f0fc54"
OLD_BASE, NEW_BASE = 0x10000, 0x90000000
START_PATCHES = [(0xe94d8, "0ef0a0e1"), (0xe8fa8, "0000a0e30ef0a0e1")]
TREES = ("Data/", "Maps/")


def fail(msg):
    sys.exit("prepare_game: " + msg)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def image(exe):
    peo, = struct.unpack_from("<I", exe, 0x3C)
    nsec, optsz = struct.unpack_from("<H", exe, peo + 6)[0], struct.unpack_from("<H", exe, peo + 20)[0]
    opt = peo + 24
    base, = struct.unpack_from("<I", exe, opt + 28)
    entry, = struct.unpack_from("<I", exe, opt + 16)
    size, headers = struct.unpack_from("<II", exe, opt + 56)
    secs = [struct.unpack_from("<IIII", exe, opt + optsz + 40 * i + 8) for i in range(nsec)]
    if base != OLD_BASE:
        fail("hmmppc.exe is not based at 0x%x" % OLD_BASE)

    def file_off(va):
        rva = va - OLD_BASE
        if rva < headers:
            return rva
        for vsize, sva, rawsz, rawptr in secs:
            if sva <= rva < sva + rawsz:
                return rawptr + rva - sva
        fail("word 0x%08x is in no section" % va)

    pe = bytearray(exe)
    delta = (NEW_BASE - OLD_BASE) & 0xFFFFFFFF
    with open(os.path.join(TABLES, "words-1053.txt")) as f:
        for line in f:
            off = file_off(int(line, 16))
            word, = struct.unpack_from("<I", pe, off)
            struct.pack_into("<I", pe, off, (word + delta) & 0xFFFFFFFF)

    img = bytearray(size)
    img[:headers] = pe[:headers]
    for vsize, sva, rawsz, rawptr in secs:
        img[sva:sva + rawsz] = pe[rawptr:rawptr + rawsz]
    for va, hx in START_PATCHES:
        b = bytes.fromhex(hx)
        img[va - OLD_BASE:va - OLD_BASE + len(b)] = b

    imps = []
    with open(os.path.join(TABLES, "imports-1053.tsv")) as f:
        for line in f:
            rva, ordinal, name = line.rstrip("\n").split("\t")
            imps.append(struct.pack("<II40s", int(rva, 16), int(ordinal), name.encode()[:39]))
    head = struct.pack("<4sIIII", b"PHE5", NEW_BASE, size, entry, len(imps))
    return head + b"".join(imps) + bytes(img)


def prepare(zip_path, out):
    data = open(zip_path, "rb").read()
    if sha256(data) != ZIP_SHA256:
        fail("%s is not PalmHeroes1.05.3.zip (sha256 %s)" % (zip_path, sha256(data)))
    with zipfile.ZipFile(zip_path) as z:
        exe = z.read("hmmppc.exe")
        if sha256(exe) != EXE_SHA256:
            fail("hmmppc.exe in the zip has sha256 " + sha256(exe))
        n = 0
        for info in z.infolist():
            if info.is_dir() or not info.filename.startswith(TREES):
                continue
            dst = os.path.join(out, *info.filename.split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as f:
                f.write(z.read(info))
            n += 1
    img = image(exe)
    if hashlib.md5(img).hexdigest() != IMAGE_MD5:
        fail("ph.img came out with md5 %s, not %s" % (hashlib.md5(img).hexdigest(), IMAGE_MD5))
    with open(os.path.join(out, "ph.img"), "wb") as f:
        f.write(img)
    print("prepare_game: %d game files and ph.img (md5 %s) in %s" % (n, IMAGE_MD5, out))


def verify(card):
    manifest = os.path.join(TABLES, "manifest-1053.tsv")
    bad = 0
    for line in open(manifest):
        rel, size = line.rstrip("\n").split("\t")
        cands = [os.path.join(card, rel), os.path.join(card, "game", rel)]
        p = next((c for c in cands if os.path.isfile(c)), None)
        got = os.path.getsize(p) if p else None
        if got != int(size):
            bad += 1
            print("%s: %s, want %s bytes" % (rel, "missing" if got is None else "%d bytes" % got, size))
    print("prepare_game: %s" % ("all files present and sized" if not bad else "%d files wrong" % bad))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description="Palm Heroes 1.05.3's Data/, Maps/ and ph.img from the developers' zip",
                                 usage="%(prog)s PalmHeroes1.05.3.zip OUT_DIR | --verify DIR")
    ap.add_argument("zip", nargs="?")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--verify", metavar="DIR")
    a = ap.parse_args()
    if a.verify:
        sys.exit(verify(a.verify))
    if not (a.zip and a.out):
        ap.error("give the zip and an output dir, or --verify DIR")
    prepare(a.zip, a.out)


if __name__ == "__main__":
    main()
