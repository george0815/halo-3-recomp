// Halo 3's own UI fonts, read straight from the game's
// maps/fonts/font_package.bin, so the launcher's menus use the same
// typeface (Conduit ITC) as the game's without shipping a font file.
//
// The package format (worked out from the file and from the game's glyph
// decompressor, sub_821F26A0 in the recompiled code). All fields are
// little-endian (the package is shared with the PC tools):
//
//   header (first 32 KiB)
//     0x000  u32 version (0xC0000003)
//     0x004  u32 font count
//     0x008  font table: per font { u32 header offset, u32 header size,
//            u16 first block, u16 block count }
//     0x108  u32 headers offset, u32 headers size, u32 block table
//            offset, u32 block count
//   font header (one per font)
//     0x00   u32 version (0xF0000005)
//     0x04   char name[32]         "conduit-16"
//     0x24   i16 ascent, i16 descent (pixels above/below the baseline)
//     0x2C   u32 kerning pairs offset (from the font header)
//     0x30   u32 kerning pair count
//     0x34   u8 first pair index per first character (256 entries)
//            pairs: { u8 second character, i8 adjustment }
//   blocks (32 KiB each, starting at 32 KiB)
//     u16 ?, u16 glyph count, u16 data offset, u16 data size
//     per glyph { u32 key = font index << 16 | character, u32 offset }
//   glyph (at block + offset)
//     i16 advance, u16 compressed size, u16 width, u16 height,
//     i16 origin x (left bearing), i16 origin y (ascent: baseline row)
//     then run-length coded 4-bit coverage (see DecodeGlyph).

#pragma once

#include <QHash>
#include <QImage>
#include <QString>

#include <array>
#include <cstdint>
#include <vector>

class QPainter;

namespace h3::launcher
{
    // One character: a coverage mask (white text is the mask itself) and how
    // to place it.
    struct Glyph
    {
        int advance = 0;  // pen movement after the glyph
        int originX = 0;  // left edge relative to the pen
        int originY = 0;  // baseline row inside the mask
        QImage mask;      // Format_Alpha8 coverage
    };

    // One font of the package (e.g. conduit-23).
    class HaloFont
    {
    public:
        QString name;
        int ascent = 0;
        int descent = 0;

        int Height() const { return ascent + descent; }
        const Glyph* Find(char32_t character) const;
        // Pen adjustment between two characters (negative pulls together).
        int Kerning(char32_t first, char32_t second) const;
        // Width of text drawn with Draw.
        int Width(const QString& text) const;
        // Draws text with its baseline starting at (x, baseline).
        void Draw(QPainter& painter, int x, int baseline, const QString& text, const QColor& color) const;

    private:
        friend class FontPackage;
        QHash<char32_t, Glyph> glyphs_;
        std::array<uint8_t, 256> kerningStart_{};  // first pair for each first character
        std::vector<std::pair<uint8_t, int8_t>> kerningPairs_;
    };

    // The fonts of one font_package.bin.
    class FontPackage
    {
    public:
        // Reads and decodes every font. Returns false (with a reason) if the
        // file is missing or not a font package.
        bool Load(const QString& path, QString* error = nullptr);
        // A font by name ("conduit-23"), or nullptr.
        const HaloFont* Find(const QString& name) const;

    private:
        std::vector<HaloFont> fonts_;
    };
}
