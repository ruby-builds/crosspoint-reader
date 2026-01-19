#!python3
import freetype
import zlib
import sys
import re
import math
import argparse
from collections import namedtuple

# Originally from https://github.com/vroland/epdiy

parser = argparse.ArgumentParser(description="Generate a header file from a font to be used with epdiy.")
parser.add_argument("name", action="store", help="name of the font.")
parser.add_argument("size", type=int, help="font size to use.")
parser.add_argument("fontstack", action="store", nargs='+', help="list of font files, ordered by descending priority.")
parser.add_argument("--2bit", dest="is2Bit", action="store_true", help="generate 2-bit greyscale bitmap instead of 1-bit black and white.")
parser.add_argument("--additional-intervals", dest="additional_intervals", action="append", help="Additional code point intervals to export as min,max. This argument can be repeated.")
parser.add_argument("--binary", dest="isBinary", action="store_true", help="output a binary .epdfont file instead of a C header.")
args = parser.parse_args()

GlyphProps = namedtuple("GlyphProps", ["width", "height", "advance_x", "left", "top", "data_length", "data_offset", "code_point"])

font_stack = [freetype.Face(f) for f in args.fontstack]
is2Bit = args.is2Bit
isBinary = args.isBinary
size = args.size
font_name = args.name

# inclusive unicode code point intervals
# must not overlap and be in ascending order
intervals = [
    ### Basic Latin ###
    # ASCII letters, digits, punctuation, control characters
    (0x0000, 0x007F),
    ### Latin-1 Supplement ###
    # Accented characters for Western European languages
    (0x0080, 0x00FF),
    ### Latin Extended-A ###
    # Eastern European and Baltic languages
    (0x0100, 0x017F),
    ### General Punctuation (core subset) ###
    # Smart quotes, en dash, em dash, ellipsis, NO-BREAK SPACE
    (0x2000, 0x206F),
    ### Basic Symbols From "Latin-1 + Misc" ###
    # dashes, quotes, prime marks
    (0x2010, 0x203A),
    # misc punctuation
    (0x2040, 0x205F),
    # common currency symbols
    (0x20A0, 0x20CF),
    ### Combining Diacritical Marks (minimal subset) ###
    # Needed for proper rendering of many extended Latin languages
    (0x0300, 0x036F),
    ### Greek & Coptic ###
    # Used in science, maths, philosophy, some academic texts
    # (0x0370, 0x03FF),
    ### Cyrillic ###
    # Russian, Ukrainian, Bulgarian, etc.
    (0x0400, 0x04FF),
    ### Math Symbols (common subset) ###
    # General math operators
    (0x2200, 0x22FF),
    # Arrows
    (0x2190, 0x21FF),
]

add_ints = []
if args.additional_intervals:
    add_ints = [tuple([int(n, base=0) for n in i.split(",")]) for i in args.additional_intervals]

def norm_floor(val):
    return int(math.floor(val / (1 << 6)))

def norm_ceil(val):
    return int(math.ceil(val / (1 << 6)))

def chunks(l, n):
    for i in range(0, len(l), n):
        yield l[i:i + n]

def load_glyph(code_point):
    face_index = 0
    while face_index < len(font_stack):
        face = font_stack[face_index]
        glyph_index = face.get_char_index(code_point)
        if glyph_index > 0:
            face.load_glyph(glyph_index, freetype.FT_LOAD_RENDER)
            return face
        face_index += 1
    print(f"code point {code_point} ({hex(code_point)}) not found in font stack!", file=sys.stderr)
    return None

unmerged_intervals = sorted(intervals + add_ints)
intervals = []
unvalidated_intervals = []
for i_start, i_end in unmerged_intervals:
    if len(unvalidated_intervals) > 0 and i_start + 1 <= unvalidated_intervals[-1][1]:
        unvalidated_intervals[-1] = (unvalidated_intervals[-1][0], max(unvalidated_intervals[-1][1], i_end))
        continue
    unvalidated_intervals.append((i_start, i_end))

for i_start, i_end in unvalidated_intervals:
    start = i_start
    for code_point in range(i_start, i_end + 1):
        face = load_glyph(code_point)
        if face is None:
            if start < code_point:
                intervals.append((start, code_point - 1))
            start = code_point + 1
    if start != i_end + 1:
        intervals.append((start, i_end))

for face in font_stack:
    face.set_char_size(size << 6, size << 6, 150, 150)

total_size = 0
all_glyphs = []

for i_start, i_end in intervals:
    for code_point in range(i_start, i_end + 1):
        face = load_glyph(code_point)
        bitmap = face.glyph.bitmap

        # Build out 4-bit greyscale bitmap
        pixels4g = []
        px = 0
        for i, v in enumerate(bitmap.buffer):
            y = i / bitmap.width
            x = i % bitmap.width
            if x % 2 == 0:
                px = (v >> 4)
            else:
                px = px | (v & 0xF0)
                pixels4g.append(px);
                px = 0
            # eol
            if x == bitmap.width - 1 and bitmap.width % 2 > 0:
                pixels4g.append(px)
                px = 0

        if is2Bit:
            # 0-3 white, 4-7 light grey, 8-11 dark grey, 12-15 black
            # Downsample to 2-bit bitmap
            pixels2b = []
            px = 0
            pitch = (bitmap.width // 2) + (bitmap.width % 2)
            for y in range(bitmap.rows):
                for x in range(bitmap.width):
                    px = px << 2
                    bm = pixels4g[y * pitch + (x // 2)]
                    bm = (bm >> ((x % 2) * 4)) & 0xF

                    if bm >= 12:
                        px += 3
                    elif bm >= 8:
                        px += 2
                    elif bm >= 4:
                        px += 1

                    if (y * bitmap.width + x) % 4 == 3:
                        pixels2b.append(px)
                        px = 0
            if (bitmap.width * bitmap.rows) % 4 != 0:
                px = px << (4 - (bitmap.width * bitmap.rows) % 4) * 2
                pixels2b.append(px)
        else:
            # Downsample to 1-bit bitmap - treat any 2+ as black
            pixelsbw = []
            px = 0
            pitch = (bitmap.width // 2) + (bitmap.width % 2)
            for y in range(bitmap.rows):
                for x in range(bitmap.width):
                    px = px << 1
                    bm = pixels4g[y * pitch + (x // 2)]
                    px += 1 if ((x & 1) == 0 and bm & 0xE > 0) or ((x & 1) == 1 and bm & 0xE0 > 0) else 0

                    if (y * bitmap.width + x) % 8 == 7:
                        pixelsbw.append(px)
                        px = 0
            if (bitmap.width * bitmap.rows) % 8 != 0:
                px = px << (8 - (bitmap.width * bitmap.rows) % 8)
                pixelsbw.append(px)

        pixels = pixels2b if is2Bit else pixelsbw

        # Build output data
        packed = bytes(pixels)
        glyph = GlyphProps(
            width = bitmap.width,
            height = bitmap.rows,
            advance_x = norm_floor(face.glyph.advance.x),
            left = face.glyph.bitmap_left,
            top = face.glyph.bitmap_top,
            data_length = len(packed),
            data_offset = total_size,
            code_point = code_point,
        )
        total_size += len(packed)
        all_glyphs.append((glyph, packed))

# pipe seems to be a good heuristic for the "real" descender
face = load_glyph(ord('|'))

glyph_data = []
glyph_props = []
for index, glyph in enumerate(all_glyphs):
    props, packed = glyph
    glyph_data.extend([b for b in packed])
    glyph_props.append(props)

if isBinary:
    import struct
    with open(f"{font_name}.epdfont", "wb") as f:
        # Magic
        f.write(b"EPDF")
        # Metrics (22 bytes)
        # intervalCount (uint32_t), advanceY (uint8_t), ascender (int32_t), descender (int32_t), is2Bit (uint8_t), totalGlyphCount (uint32_t)
        f.write(struct.pack("<IBiiBI", len(intervals), norm_ceil(face.size.height), norm_ceil(face.size.ascender), norm_floor(face.size.descender), 1 if is2Bit else 0, len(glyph_props)))
        # Intervals
        offset = 0
        for i_start, i_end in intervals:
            f.write(struct.pack("<III", i_start, i_end, offset))
            offset += i_end - i_start + 1
        # Glyphs
        for g in glyph_props:
            # dataOffset (uint32_t), dataLength (uint16_t), width (uint8_t), height (uint8_t), advanceX (uint8_t), left (int8_t), top (int8_t)
            # wait, GlyphProps has width, height, advance_x, left, top, data_length, data_offset, code_point
            # We need: dataOffset (4), dataLength (2), width (1), height (1), advanceX (1), left (1), top (1) = 11 bytes?
            # Let's use 13 bytes as planned to be safer or just 11. 
            # Original EpdGlyph: 
            #   uint32_t dataOffset;
            #   uint16_t dataLength;
            #   uint8_t width;
            #   uint8_t height;
            #   uint8_t advanceX;
            #   int8_t left;
            #   int8_t top;
            # Total: 4+2+1+1+1+1+1 = 11 bytes.
            # I will use 13 bytes to align better if needed, but 11 is fine.
            # Let's use 13 bytes as per plan: 4+2+1+1+1+1+1 + 2 padding.
            # CustomEpdFont.cpp expects:
            # glyphBuf[0] = w
            # glyphBuf[1] = h
            # glyphBuf[2] = adv
            # glyphBuf[3] = l (signed)
            # glyphBuf[4] = unused
            # glyphBuf[5] = t (signed)
            # glyphBuf[6] = unused
            # glyphBuf[7-8] = dLen
            # glyphBuf[9-12] = dOffset
            f.write(struct.pack("<BBB b B b B H I", g.width, g.height, g.advance_x, g.left, 0, g.top, 0, g.data_length, g.data_offset))
        # Bitmaps
        f.write(bytes(glyph_data))
    print(f"Generated {font_name}.epdfont")
else:
    print(f"/**\n * generated by fontconvert.py\n * name: {font_name}\n * size: {size}\n * mode: {'2-bit' if is2Bit else '1-bit'}\n */")
    print("#pragma once")
    print("#include \"EpdFontData.h\"\n")
    print(f"static const uint8_t {font_name}Bitmaps[{len(glyph_data)}] = {{")
    for c in chunks(glyph_data, 16):
        print ("    " + " ".join(f"0x{b:02X}," for b in c))
    print ("};\n");

    print(f"static const EpdGlyph {font_name}Glyphs[] = {{")
    for i, g in enumerate(glyph_props):
        print ("    { " + ", ".join([f"{a}" for a in list(g[:-1])]),"},", f"// {chr(g.code_point) if g.code_point != 92 else '<backslash>'}")
    print ("};\n");

    print(f"static const EpdUnicodeInterval {font_name}Intervals[] = {{")
    offset = 0
    for i_start, i_end in intervals:
        print (f"    {{ 0x{i_start:X}, 0x{i_end:X}, 0x{offset:X} }},")
        offset += i_end - i_start + 1
    print ("};\n");

    print(f"static const EpdFontData {font_name} = {{")
    print(f"    {font_name}Bitmaps,")
    print(f"    {font_name}Glyphs,")
    print(f"    {font_name}Intervals,")
    print(f"    {len(intervals)},")
    print(f"    {norm_ceil(face.size.height)},")
    print(f"    {norm_ceil(face.size.ascender)},")
    print(f"    {norm_floor(face.size.descender)},")
    print(f"    {'true' if is2Bit else 'false'},")
    print("};")

