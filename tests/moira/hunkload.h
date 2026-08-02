/*
 * AmiNetXDuo -- an AmigaOS hunk loader, for feeding real linked binaries to a
 * host-side 68k core.
 *
 * The question this answers is "can Moira be fed our actual object code".  It
 * can, but not by flattening: m68k-amigaos-ld emits an AmigaOS load file, and
 * binutils cannot read one back (`objdump: file format not recognized`), so
 * there is no ELF to objcopy and no ELF for addr2line to symbolise against.
 * What there is, is the format Exec's own LoadSeg() eats -- HUNK_CODE with
 * HUNK_RELOC32 fixups and a HUNK_SYMBOL table -- and reading that gives
 * relocation and symbols in one pass.
 *
 * Hand-written position-independent assembly does not need this; both
 * src/net68k/ primitives flatten with objcopy -O binary and run at any
 * address.  Compiled C does: gcc reaches an extern through an absolute
 * `jsr xxx.l`, which is a RELOC32.
 *
 * Only the hunks a load file can actually contain are handled.  Anything else
 * throws, rather than silently loading half an image.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <stdexcept>

namespace hunk {

enum : uint32_t {
    HUNK_CODE   = 0x3E9, HUNK_DATA  = 0x3EA, HUNK_BSS    = 0x3EB,
    HUNK_RELOC32= 0x3EC, HUNK_SYMBOL= 0x3F0, HUNK_DEBUG  = 0x3F1,
    HUNK_END    = 0x3F2, HUNK_HEADER= 0x3F3,
};

struct Segment { uint32_t base, size; bool code; };

struct Image {

    std::vector<Segment>                   seg;
    std::map<std::string, uint32_t>        sym;    /* name  -> address */
    std::map<uint32_t, std::string>        rsym;   /* address -> name  */

    uint32_t operator[](const char *name) const
    {
        auto it = sym.find(name);
        if (it == sym.end()) throw std::runtime_error(std::string("no symbol ") + name);
        return it->second;
    }

    /* The symbol covering an address: the last one at or below it, provided
       both are in the same segment.  There is no size in HUNK_SYMBOL, so this
       is nearest-preceding and nothing better is available. */
    const char *resolve(uint32_t addr) const
    {
        for (auto &s : seg) {
            if (addr < s.base || addr >= s.base + s.size) continue;
            auto it = rsym.upper_bound(addr);
            if (it == rsym.begin()) return "(no symbol)";
            --it;
            return it->second.c_str();
        }
        return "(outside image)";
    }
};

/*
 * `poke` writes one byte; `base` is where segment 0 goes.  Segments are laid
 * out consecutively, longword aligned, which is not what Exec does (it
 * AllocMems each one separately) but is what a flat memory image wants and
 * makes no difference once the RELOC32s are applied.
 */
template <typename Poke, typename Peek>
Image load(const std::string &path, uint32_t base, Poke poke, Peek peek)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);

    std::string d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    size_t   i   = 0;
    auto     U32 = [&](void) -> uint32_t {
        if (i + 4 > d.size()) throw std::runtime_error("truncated hunk file");
        uint32_t v = (uint8_t(d[i]) << 24) | (uint8_t(d[i+1]) << 16) |
                     (uint8_t(d[i+2]) << 8) | uint8_t(d[i+3]);
        i += 4;
        return v;
    };

    if (U32() != HUNK_HEADER) throw std::runtime_error(path + " is not a hunk load file");

    while (U32() != 0) { }                          /* resident library names */

    uint32_t table = U32(), first = U32(), last = U32();
    (void)table;

    Image img;
    uint32_t at = base;

    for (uint32_t n = first; n <= last; n++) {

        uint32_t bytes = U32() * 4;
        img.seg.push_back({ at, bytes, false });
        at += (bytes + 3) & ~3u;
    }

    size_t cur = 0;

    while (i < d.size()) {

        uint32_t t = U32() & 0x3FFFFFFF;

        switch (t) {

        case HUNK_CODE:
        case HUNK_DATA: {

            uint32_t bytes = U32() * 4;
            if (cur >= img.seg.size()) throw std::runtime_error("more hunks than the header declared");
            img.seg[cur].code = (t == HUNK_CODE);
            for (uint32_t k = 0; k < bytes; k++) poke(img.seg[cur].base + k, uint8_t(d[i + k]));
            i += bytes;
            break;
        }

        case HUNK_BSS:
            U32();                                  /* already zero in the image */
            break;

        case HUNK_RELOC32:
            for (;;) {
                uint32_t n = U32();
                if (n == 0) break;
                uint32_t target = U32();
                if (target >= img.seg.size()) throw std::runtime_error("reloc into a hunk that is not there");
                for (uint32_t k = 0; k < n; k++) {
                    uint32_t off = U32();
                    uint32_t site = img.seg[cur].base + off;
                    uint32_t v = (uint32_t(peek(site)) << 24) | (uint32_t(peek(site+1)) << 16) |
                                 (uint32_t(peek(site+2)) << 8) | peek(site+3);
                    v += img.seg[target].base;
                    poke(site, uint8_t(v >> 24)); poke(site+1, uint8_t(v >> 16));
                    poke(site+2, uint8_t(v >> 8)); poke(site+3, uint8_t(v));
                }
            }
            break;

        case HUNK_SYMBOL:
            for (;;) {
                uint32_t n = U32();
                if (n == 0) break;
                std::string name(d, i, n * 4);
                i += n * 4;
                while (!name.empty() && name.back() == '\0') name.pop_back();
                uint32_t v = img.seg[cur].base + U32();
                img.sym[name] = v;
                /* Several linker-emitted markers share the end address; keep
                   the first so a real function is not shadowed by __etext. */
                img.rsym.emplace(v, name);
            }
            break;

        case HUNK_DEBUG: {
            uint32_t n = U32() * 4;
            i += n;
            break;
        }

        case HUNK_END:
            cur++;
            break;

        default: {
            char msg[64];
            snprintf(msg, sizeof msg, "unhandled hunk type %#x", t);
            throw std::runtime_error(msg);
        }
        }
    }

    return img;
}

} /* namespace hunk */
