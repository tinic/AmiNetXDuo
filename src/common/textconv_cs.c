/*
 * The machine's codeset for aminetxduo/textconv.h.  codesets.library (v6,
 * Kickstart 3.0 and up) is optional: when it opens, the system's codeset or
 * the one named, with its look-alike text for characters the codeset lacks;
 * otherwise ISO-8859-1.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/nodes.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <inline/macros.h>       /* LP1/LP2 for codesets.library */

#include "aminetxduo/textconv.h"

#define CS_TAG(n)              (0xfec901f4UL + (n))   /* libraries/codesets.h */
#define CSA_SourceLen          CS_TAG(1)
#define CSA_Source             CS_TAG(2)
#define CSA_FallbackToDefault  CS_TAG(12)
#define CSA_DestCodeset        CS_TAG(13)
#define CSA_MapForeignChars    CS_TAG(28)

/* libraries/codesets.h, 6.x: public and read-only. */
struct cs_single { UBYTE code; UBYTE utf8[8]; ULONG ucs4; };
struct cs_codeset
{
    struct MinNode   node;
    char            *name, *alt_name, *characterization;
    LONG             read_only;
    struct cs_single table[256];
};

static struct Library *CodesetsBase;
#define CodesetsFreeA(obj, attrs) \
    LP2NR(0x5a, CodesetsFreeA, APTR, obj, a0, struct TagItem *, attrs, a1, \
    , CodesetsBase)
#define CodesetsFindA(name, attrs) \
    LP2(0x66, struct cs_codeset *, CodesetsFindA, CONST_STRPTR, name, a0, \
    struct TagItem *, attrs, a1, , CodesetsBase)
#define CodesetsUTF8ToStrA(attrs) \
    LP1(0x78, STRPTR, CodesetsUTF8ToStrA, struct TagItem *, attrs, a0, \
    , CodesetsBase)

static struct cs_codeset *tc_cs;
static uint16_t           tc_upper[128];
static txt_codeset        tc_set;           /* upper NULL: ISO-8859-1 */

/* A code point the codeset has no byte for: codesets' look-alike text.  No
   CSA_Dest: 6.22 frees a caller's buffer when the mapping expands a
   character, so the library allocates and CodesetsFreeA() returns it. */
static TXT_STDARGS size_t tc_foreign(void *ctx, uint32_t cp, uint8_t *out, size_t cap)
{
    UBYTE          u[4];
    ULONG          n;
    STRPTR         str;
    size_t         k = 0;
    struct TagItem tags[5];

    (void)ctx;
    if (cp < 0x800)
    {
        u[0] = (UBYTE)(0xC0 | (cp >> 6));
        n = 1;
    }
    else if (cp < 0x10000)
    {
        u[0] = (UBYTE)(0xE0 | (cp >> 12));
        u[1] = (UBYTE)(0x80 | ((cp >> 6) & 0x3F));
        n = 2;
    }
    else
    {
        u[0] = (UBYTE)(0xF0 | (cp >> 18));
        u[1] = (UBYTE)(0x80 | ((cp >> 12) & 0x3F));
        u[2] = (UBYTE)(0x80 | ((cp >> 6) & 0x3F));
        n = 3;
    }
    u[n++] = (UBYTE)(0x80 | (cp & 0x3F));

    tags[0].ti_Tag = CSA_Source;          tags[0].ti_Data = (ULONG)u;
    tags[1].ti_Tag = CSA_SourceLen;       tags[1].ti_Data = n;
    tags[2].ti_Tag = CSA_DestCodeset;     tags[2].ti_Data = (ULONG)tc_cs;
    tags[3].ti_Tag = CSA_MapForeignChars; tags[3].ti_Data = TRUE;
    tags[4].ti_Tag = TAG_END;             tags[4].ti_Data = 0;

    str = CodesetsUTF8ToStrA(tags);
    if (str == NULL)
        return 0;
    while (k < cap && str[k] != '\0')
    {
        out[k] = (uint8_t)str[k];
        k++;
    }
    CodesetsFreeA(str, NULL);
    return k;
}

/* ASCII case-insensitive equality. */
static int tc_same(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; a++, b++)
    {
        char x = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char y = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;

        if (x != y)
            return 0;
    }
    return *a == *b;
}

const txt_codeset *txt_codeset_open(const char *name, int *flags)
{
    struct TagItem tags[2];
    int            i;

    *flags = 0;
    if (CodesetsBase == NULL)
        CodesetsBase = OpenLibrary((CONST_STRPTR)"codesets.library", 6);
    if (CodesetsBase == NULL)
        return &tc_set;
    *flags |= TXT_CS_LIBRARY;

    tags[0].ti_Tag = CSA_FallbackToDefault; tags[0].ti_Data = FALSE;
    tags[1].ti_Tag = TAG_END;               tags[1].ti_Data = 0;
    tc_cs = NULL;
    if (name != NULL && name[0] != '\0')
    {
        tc_cs = CodesetsFindA((CONST_STRPTR)name, tags);
        if (tc_cs == NULL)
            *flags |= TXT_CS_UNKNOWN;
    }
    if (tc_cs == NULL)
        tc_cs = CodesetsFindA(NULL, NULL);
    if (tc_cs == NULL || tc_cs->name == NULL)
    {
        tc_cs = NULL;
        return &tc_set;
    }
    if (tc_same(tc_cs->name, "UTF-8"))
    {
        *flags |= TXT_CS_UTF8;
        return &tc_set;
    }

    for (i = 0; i < 128; i++)
    {
        const struct cs_single *e = &tc_cs->table[0x80 + i];

        tc_upper[i] = (e->code == 0x80 + i && e->ucs4 < 0x10000)
                    ? (uint16_t)e->ucs4 : 0;
    }
    tc_set.upper   = tc_upper;
    tc_set.foreign = tc_foreign;
    return &tc_set;
}

void txt_codeset_close(void)
{
    tc_set.upper   = NULL;
    tc_set.foreign = NULL;
    tc_cs          = NULL;
    if (CodesetsBase != NULL)
        CloseLibrary(CodesetsBase);
    CodesetsBase = NULL;
}
