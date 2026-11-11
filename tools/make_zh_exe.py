"""Write a 1.10a th123.exe carrying the 汉化 (simplified Chinese, GBK) UI strings, for the
build_vita.py --gbk port.

The 汉化 patch ships its own th123.exe, a 1.10 build that Touhou123Vita does not accept (it
checks the 1.10a entry point and image size). The patch translated ~60 UI strings in that exe
(prompts, dialogs, error messages, the window title); this copies them into the same strings of
your original 1.10a exe, in place, so code, entry point and image size stay the same. The
translations are read from your copy of the patch's exe: only their positions are listed here,
plus our own translations of three strings the patch's exe does not have.

Usage: python tools/make_zh_exe.py ORIGINAL_1.10a_th123.exe ZH_PATCH_th123.exe OUT_th123.exe"""
import hashlib
import struct
import sys

SHA_110A = "f0db93660ceb8635a1921e0ba4686fcfbd5c492311dee6ef7061b7bada16731e"
SHA_ZH = "4da093ee7e9b3712990d0e2173fd7954b23b0376c45f571023b8512a5900d4e6"  # the patch's th123.exe (1.10)

# (offset of the string in 1.10a, offset of its translation in the patch's exe)
PAIRS = [
    (0x459090, 0x448090), (0x4590c0, 0x4480c0), (0x459218, 0x448218), (0x459234, 0x448234),
    (0x45925c, 0x44825c), (0x4592a8, 0x4482a8), (0x4592c0, 0x4482c0), (0x459374, 0x448374),
    (0x459390, 0x448390), (0x4593a8, 0x4483a8), (0x4593bc, 0x4483bc), (0x4593d0, 0x4483d0),
    (0x4593f0, 0x4483f0), (0x4593f8, 0x4483f8), (0x45940c, 0x44840c), (0x459474, 0x448474),
    (0x459498, 0x448498), (0x4595a8, 0x4485a8), (0x4596bc, 0x4486bc), (0x4596cc, 0x4486cc),
    (0x4596e0, 0x4486e0), (0x4597a0, 0x4487a0), (0x459930, 0x448928), (0x459960, 0x448958),
    (0x459b08, 0x448b00), (0x459b20, 0x448b18), (0x459b30, 0x448b28), (0x459b40, 0x448b38),
    (0x459b60, 0x448b58), (0x459b7c, 0x448b74), (0x459b90, 0x448b88), (0x459ba4, 0x448b9c),
    (0x459bb8, 0x448bb0), (0x461b0c, 0x450afc), (0x461ba4, 0x450b94), (0x471148, 0x45e380),
    (0x471170, 0x45e3a8), (0x4711a8, 0x45e3e0), (0x4711c4, 0x45e3fc), (0x4711e4, 0x45e41c),
    (0x4711fc, 0x45e434), (0x471214, 0x45e44c), (0x47122c, 0x45e464), (0x471288, 0x45e4c0),
    (0x4712a8, 0x45e4e0), (0x4712cc, 0x45e504), (0x4712e8, 0x45e520), (0x471308, 0x45e540),
    (0x47133c, 0x45e574), (0x47135c, 0x45e594), (0x47138c, 0x45e5c4), (0x4713b0, 0x45e5e8),
    (0x4713d4, 0x45e60c), (0x4713fc, 0x45e634), (0x471424, 0x45e65c), (0x471448, 0x45e680),
]
# strings the patch's exe does not have (new in 1.10a or left in Japanese): our translations
OURS = [(0x458d5c, "replay文件夹创建失败"), (0x458d78, "profile文件夹创建失败"), (0x459078, "录像已保存")]
# the window title: the patch's own, with its version made 1.10a
TITLE = (0x461b58, 0x450b48, b"Ver1.10(beta)", b"Ver1.10a")


def cstr(data, off):
    return data[off:data.index(b"\0", off)]


def entry_and_image(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    return struct.unpack_from("<I", data, pe + 40)[0], struct.unpack_from("<I", data, pe + 80)[0]


def put(out, off, text):
    slot = len(cstr(out, off))  # the 1.10a string's own length
    assert len(text) <= slot, f"{off:#x}: translation longer than the original ({len(text)} > {slot})"
    out[off:off + slot] = text + b"\0" * (slot - len(text))


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    jp, zh = open(sys.argv[1], "rb").read(), open(sys.argv[2], "rb").read()
    if hashlib.sha256(jp).hexdigest() != SHA_110A:
        sys.exit(f"{sys.argv[1]}: not the original 1.10a th123.exe (SHA-256 {SHA_110A})")
    if hashlib.sha256(zh).hexdigest() != SHA_ZH:
        sys.exit(f"{sys.argv[2]}: not the 汉化 patch's th123.exe this table was made for (SHA-256 {SHA_ZH})")
    out = bytearray(jp)
    for off, zh_off in PAIRS:
        put(out, off, cstr(zh, zh_off))
    for off, text in OURS:
        put(out, off, text.encode("gbk"))
    off, zh_off, old, new = TITLE
    title = cstr(zh, zh_off)
    assert title.endswith(old)
    put(out, off, title[:-len(old)] + new)
    assert entry_and_image(out) == entry_and_image(jp) and len(out) == len(jp)
    open(sys.argv[3], "wb").write(out)
    print(f"{len(PAIRS) + len(OURS) + 1} strings written -> {sys.argv[3]}")


if __name__ == "__main__":
    main()
