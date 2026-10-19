# pdftops-lite

Minimal PDF → PostScript converter used as a CUPS filter on small OpenWrt
routers where installing Ghostscript is not possible (flash space).

## Why

CUPS on OpenWrt ships an extremely stripped `cups-filters` (only
`rastertopwg` / `rastertohp`). There is no `pdftops`, no `poppler`, no
`ghostscript` in the package feed, so any IPP client that submits a PDF
(every Android/iOS print stack) gets `client-error-not-document-type-supported`
(IPP 1034).

`pdftops-lite` fills exactly that gap: it converts a PDF into PostScript,
which the printer (HP LaserJet 400 M401d) understands natively.

## Build

Cross compile for MIPS little-endian soft-float musl. The toolchain is taken
from the [`cross-tools/musl-cross`](https://github.com/cross-tools/musl-cross)
GitHub Releases rather than musl.cc: it is smaller (61 MB vs 101 MB), hosted on
GitHub (fast and reliable inside Actions), and ships a `.sha256` checksum.

```sh
VER=20250928
URL=https://github.com/cross-tools/musl-cross/releases/download/$VER
curl -fsSLO $URL/mipsel-unknown-linux-muslsf.tar.xz
curl -fsSLO $URL/mipsel-unknown-linux-muslsf.tar.xz.sha256
echo "$(cat mipsel-unknown-linux-muslsf.tar.xz.sha256)  mipsel-unknown-linux-muslsf.tar.xz" > tc.check
sha256sum -c tc.check
tar xJf mipsel-unknown-linux-muslsf.tar.xz

# bin/ contains only version-suffixed real files, no bare `gcc` symlink
TC=mipsel-unknown-linux-muslsf
ln -sf "$PWD/$TC/bin/mipsel-unknown-linux-muslsf-gcc-15.2.0" "$TC/bin/mipsel-gcc"
"$TC/bin/mipsel-gcc" -O2 -static \
    -o pdftops-lite buf.c pdfobj.c psgen.c pdfimage.c main.c -lm
```

Alternatively just push and let `.github/workflows/build.yml` do it; the
binary lands in the `pdftops-lite-mipsel` artifact.

The resulting binary is a few hundred KB and runs on mipsel_24kc OpenWrt
with no shared libraries.

## Install as a CUPS filter

```sh
mkdir -p /usr/lib/cups/filter
cp pdftops-lite /usr/lib/cups/filter/
chmod 755 /usr/lib/cups/filter/pdftops-lite
```

`pdftops-lite` follows CUPS filter conventions: it reads the document on
stdin (or from the file given as the last argument) and writes PostScript
to stdout. Extra arguments (job-id, user, title, copies, options) are
accepted and ignored.

Wire it into CUPS by pointing the printer PPD at it, or by adding a
conversion rule in `/usr/share/cups/mime/mime.convs`:

```
application/pdf  application/vnd.cups-postscript  66  pdftops-lite
```

## Scope

Designed for text-oriented documents (Word / WPS / web exports) with the
occasional image.

- Text is emitted as real PostScript text (vector, crisp at 1200 dpi).
- JPEG (DCTDecode) and raw 8-bit gray/RGB images are embedded.
- Flate, ASCIIHex and ASCII85 stream encodings are supported by a built-in
  inflate (no zlib dependency).
- CMYK, JPX, CCITT and JBIG2 images are skipped.
- Fonts are mapped to the standard PostScript 14, so unusual typefaces are
  substituted (Helvetica family). Unicode/CID fonts are rendered as
  Latin-1 where possible.

## Exit codes

0 on success, 1 on unreadable input. Errors go to stderr, which CUPS records
in its job log.
