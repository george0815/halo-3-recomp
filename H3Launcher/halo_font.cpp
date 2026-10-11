// Halo 3 font package reader and text drawing (halo_font.h).

#include "halo_font.h"

#include <QFile>
#include <QPainter>

#include <cstring>

namespace h3::launcher
{
    namespace
    {
        constexpr uint32_t kPackageVersion = 0xC0000003;
        constexpr uint32_t kFontVersion = 0xF0000005;
        constexpr size_t kBlockSize = 0x8000;  // the header page and each glyph block
        constexpr size_t kFontTable = 0x008;
        constexpr size_t kFontTableEntry = 12;
        constexpr size_t kMaxFonts = 16;
        constexpr size_t kBlockCountField = 0x114;

        // Font header fields.
        constexpr size_t kFontName = 0x04;
        constexpr size_t kFontNameSize = 32;
        constexpr size_t kFontAscent = 0x24;
        constexpr size_t kFontDescent = 0x26;
        constexpr size_t kFontKerningOffset = 0x2C;
        constexpr size_t kFontKerningCount = 0x30;
        constexpr size_t kFontKerningStart = 0x34;

        // Block and glyph headers.
        constexpr size_t kBlockHeader = 8;
        constexpr size_t kBlockEntry = 8;
        constexpr size_t kGlyphHeader = 12;

        // Little-endian reads with bounds checks (false/0 past the end).
        struct Reader
        {
            const QByteArray& data;
            bool In(size_t offset, size_t size) const { return offset + size <= size_t(data.size()); }
            uint32_t U32(size_t offset) const
            {
                uint32_t value = 0;
                if (In(offset, 4))
                    memcpy(&value, data.constData() + offset, 4);
                return value;
            }
            uint16_t U16(size_t offset) const
            {
                uint16_t value = 0;
                if (In(offset, 2))
                    memcpy(&value, data.constData() + offset, 2);
                return value;
            }
            int16_t S16(size_t offset) const { return int16_t(U16(offset)); }
            uint8_t U8(size_t offset) const { return In(offset, 1) ? uint8_t(data[qsizetype(offset)]) : 0; }
        };

        // Coverage (0-15) to an 8-bit alpha.
        uchar Alpha4(uint32_t value) { return uchar(value * 17); }
        // A 3-bit coverage code as the game stores it: shifted left once
        // into the 4-bit alpha (so 7 is 14/15).
        uchar Alpha3(uint32_t value) { return Alpha4(value << 1); }

        // Decompresses a glyph's run-length coded coverage into a width x
        // height Alpha8 image. Each byte (b) is one code, by its top two bits:
        //
        //   00nnnnnn  n transparent pixels; n = 0 is an escape: the next two
        //             bytes are one explicit ARGB4444 pixel
        //   01nnnnnn  n opaque pixels
        //   10aaaccc  one pixel of coverage a, then by c: 0 nothing more,
        //             1-3 (5 - c) opaque pixels, 4-7 (9 - c) transparent pixels
        //   11aaabbb  two pixels, coverage a then b
        //
        // Matches the game's decoder (sub_821F26A0).
        QImage DecodeGlyph(const uint8_t* data, size_t size, int width, int height)
        {
            QImage image(std::max(width, 1), std::max(height, 1), QImage::Format_Alpha8);
            image.fill(0);
            const size_t total = size_t(width) * size_t(height);
            size_t pixel = 0;
            auto put = [&](uchar alpha, size_t count)
            {
                for (size_t i = 0; i < count && pixel < total; i++, pixel++)
                    image.scanLine(int(pixel / width))[pixel % width] = alpha;
            };
            for (size_t i = 0; i < size && pixel < total; i++)
            {
                uint8_t code = data[i];
                uint32_t count = code & 0x3F;
                switch (code >> 6)
                {
                case 0:
                    if (count == 0 && i + 2 < size)
                    {
                        put(Alpha4(data[i + 1] >> 4), 1);  // high nibble of the big-endian ARGB4444 pixel
                        i += 2;
                    }
                    else
                        put(0, count);
                    break;
                case 1:
                    put(Alpha3(7), count);
                    break;
                case 2:
                {
                    put(Alpha3((code >> 3) & 7), 1);
                    uint32_t tail = code & 7;
                    if (tail >= 1 && tail <= 3)
                        put(Alpha4(15), 5 - tail);
                    else if (tail >= 4)
                        put(0, 9 - tail);
                    break;
                }
                case 3:
                    put(Alpha3((code >> 3) & 7), 1);
                    put(Alpha3(code & 7), 1);
                    break;
                }
            }
            return image;
        }
    }

    const Glyph* HaloFont::Find(char32_t character) const
    {
        auto found = glyphs_.constFind(character);
        return found != glyphs_.constEnd() ? &found.value() : nullptr;
    }

    int HaloFont::Kerning(char32_t first, char32_t second) const
    {
        if (first > 0xFF || second > 0xFF)
            return 0;
        size_t begin = kerningStart_[first];
        size_t end = first < 0xFF ? kerningStart_[first + 1] : kerningPairs_.size();
        for (size_t i = begin; i < end && i < kerningPairs_.size(); i++)
            if (kerningPairs_[i].first == second)
                return kerningPairs_[i].second;
        return 0;
    }

    int HaloFont::Width(const QString& text) const
    {
        int width = 0;
        char32_t previous = 0;
        for (char32_t character : text.toUcs4())
        {
            if (const Glyph* glyph = Find(character))
                width += glyph->advance + Kerning(previous, character);
            previous = character;
        }
        return width;
    }

    void HaloFont::Draw(QPainter& painter, int x, int baseline, const QString& text, const QColor& color) const
    {
        // Masks are drawn into a scratch image, then the color is filled
        // through them (SourceIn), so any color works with one set of masks.
        constexpr int kPad = 4;  // room for glyphs that overhang their advance
        QImage scratch(Width(text) + 2 * kPad, Height() + 2 * kPad, QImage::Format_ARGB32_Premultiplied);
        scratch.fill(Qt::transparent);
        {
            QPainter layer(&scratch);
            int pen = kPad;
            char32_t previous = 0;
            for (char32_t character : text.toUcs4())
            {
                const Glyph* glyph = Find(character);
                if (!glyph)
                    continue;
                pen += Kerning(previous, character);
                layer.drawImage(pen + glyph->originX, kPad + ascent - glyph->originY, glyph->mask);
                pen += glyph->advance;
                previous = character;
            }
            layer.setCompositionMode(QPainter::CompositionMode_SourceIn);
            layer.fillRect(scratch.rect(), color);
        }
        painter.drawImage(x - kPad, baseline - ascent - kPad, scratch);
    }

    bool FontPackage::Load(const QString& path, QString* error)
    {
        auto fail = [&](const QString& reason)
        {
            if (error)
                *error = reason;
            fonts_.clear();
            return false;
        };
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return fail(QStringLiteral("can't open %1").arg(path));
        const QByteArray data = file.readAll();
        Reader read{ data };
        if (read.U32(0) != kPackageVersion)
            return fail(QStringLiteral("%1 isn't a Halo 3 font package").arg(path));

        // Font headers.
        uint32_t fontCount = std::min<uint32_t>(read.U32(4), kMaxFonts);
        fonts_.assign(fontCount, HaloFont());
        for (uint32_t i = 0; i < fontCount; i++)
        {
            size_t header = read.U32(kFontTable + i * kFontTableEntry);
            if (read.U32(header) != kFontVersion)
                return fail(QStringLiteral("font %1 has an unknown header").arg(i));
            HaloFont& font = fonts_[i];
            char name[kFontNameSize + 1] = {};
            if (read.In(header + kFontName, kFontNameSize))
                memcpy(name, data.constData() + header + kFontName, kFontNameSize);
            font.name = QString::fromLatin1(name);
            font.ascent = read.S16(header + kFontAscent);
            font.descent = read.S16(header + kFontDescent);
            uint32_t pairs = read.U32(header + kFontKerningCount);
            size_t pairOffset = header + read.U32(header + kFontKerningOffset);
            for (size_t c = 0; c < 256; c++)
                font.kerningStart_[c] = read.U8(header + kFontKerningStart + c);
            for (uint32_t p = 0; p < pairs && read.In(pairOffset + p * 2, 2); p++)
                font.kerningPairs_.emplace_back(read.U8(pairOffset + p * 2), int8_t(read.U8(pairOffset + p * 2 + 1)));
        }

        // Glyph blocks: every glyph's key names its font.
        uint32_t blockCount = read.U32(kBlockCountField);
        for (uint32_t b = 0; b < blockCount; b++)
        {
            size_t block = (b + 1) * kBlockSize;
            uint16_t count = read.U16(block + 2);
            for (uint32_t g = 0; g < count; g++)
            {
                size_t entry = block + kBlockHeader + g * kBlockEntry;
                uint32_t key = read.U32(entry);
                size_t glyph = block + read.U32(entry + 4);
                uint32_t fontIndex = key >> 16;
                if (fontIndex >= fontCount || !read.In(glyph, kGlyphHeader))
                    continue;
                uint16_t size = read.U16(glyph + 2);
                int width = read.U16(glyph + 4), height = read.U16(glyph + 6);
                if (!read.In(glyph + kGlyphHeader, size))
                    continue;
                Glyph decoded;
                decoded.advance = read.S16(glyph + 0);
                decoded.originX = read.S16(glyph + 8);
                decoded.originY = read.S16(glyph + 10);
                decoded.mask = DecodeGlyph(reinterpret_cast<const uint8_t*>(data.constData() + glyph + kGlyphHeader), size, width, height);
                fonts_[fontIndex].glyphs_.insert(char32_t(key & 0xFFFF), std::move(decoded));
            }
        }
        return true;
    }

    const HaloFont* FontPackage::Find(const QString& name) const
    {
        for (const HaloFont& font : fonts_)
            if (font.name == name)
                return &font;
        return nullptr;
    }
}
