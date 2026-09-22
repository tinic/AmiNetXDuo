/*
 * httpxml, the XML skimmer.  See httpxml.h for why this is its own file.
 *
 * Includes only its portable siblings, deliberately: this translation unit is
 * compiled once for m68k as part of the server and once natively by
 * src/tools/test/test_httpxml.c.
 *
 * SPDX-License-Identifier: MIT
 */

#include "httpxml.h"
#include "httppath.h"

/* ------------------------------------------------------------- the small --- */

static unsigned long hx_len(const char *s)
{
    unsigned long n = 0;

    while (s[n] != '\0')
        n++;

    return n;
}

static int hx_nicmp(const char *a, const char *b, unsigned long n)
{
    unsigned long i;

    for (i = 0; i < n; i++)
    {
        int ca = (unsigned char)a[i];
        int cb = (unsigned char)b[i];

        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;

        if (ca != cb || ca == 0)
            return ca - cb;
    }

    return 0;
}

static int hx_equal(const char *a, const char *b)
{
    unsigned long n = hx_len(b);

    return (hx_len(a) == n && hx_nicmp(a, b, n) == 0) ? 1 : 0;
}

static void hx_copy(char *dst, unsigned long dstlen, const char *src)
{
    unsigned long n = 0;

    if (dstlen == 0UL)
        return;

    while (src[n] != '\0' && n + 1UL < dstlen)
    {
        dst[n] = src[n];
        n++;
    }

    dst[n] = '\0';
}

static const char *hx_local(const char *qname)
{
    unsigned long i;

    for (i = 0; qname[i] != '\0'; i++)
    {
        if (qname[i] == ':')
            return &qname[i + 1];
    }

    return qname;
}

/* ------------------------------------------------------------- the reset --- */

void http_xml_reset(HttpXml *x)
{
    x->state   = XML_TEXT;
    x->pf_mode = (unsigned char)HTTPD_PF_ALLPROP;
    x->name_n  = 0;
    x->close   = 0;
    x->text_n  = 0;
    x->attr_n  = 0;

    x->in_prop   = 0;
    x->in_owner  = 0;
    x->propfind  = 0;
    x->props     = 0;
    x->props_cut = 0;
    x->nsdecls   = 0;
    x->have_date = 0;
    x->owner[0]  = '\0';
}

/* ----------------------------------------------------------- the skimming --- */

static void hx_note_property(HttpXml *x)
{
    if (x->props >= (unsigned char)HTTPD_PROPS_MAX)
    {
        /* An answer about the ones that fitted reads as one about all. */
        x->props_cut = 1;
        return;
    }

    hx_copy(x->prop_name[x->props], (unsigned long)HTTPD_QNAME_MAX, x->name);
    x->prop_ok[x->props] = 0;
    x->props++;
}

/* The property just closed, with whatever text it held.  AmigaOS keeps one date
   per file, so the modification time is the only thing here that maps to a
   call.  Everything else is answered 403 in the 207. */
static void hx_set_property(HttpXml *x)
{
    const char   *local = hx_local(x->name);
    unsigned char i;

    if (!hx_equal(local, "Win32LastModifiedTime") &&
        !hx_equal(local, "getlastmodified"))
        return;

    x->text[x->text_n] = '\0';

    if (x->date == 0 || !x->date(x->date_ctx, x->text))
        return;

    x->have_date = 1;

    for (i = 0; i < x->props; i++)
    {
        if (hx_equal(x->prop_name[i], x->name))
            x->prop_ok[i] = 1;
    }
}

static void hx_note_nsdecl(HttpXml *x)
{
    unsigned char i;

    if (hx_nicmp(x->attr, "xmlns", 5) != 0)
        return;

    if (x->nsdecls >= (unsigned char)HTTPD_NS_MAX)
        return;

    for (i = 0; i < x->nsdecls; i++)
    {
        if (hx_equal(x->nsdecl[i], x->attr))
            return;
    }

    hx_copy(x->nsdecl[x->nsdecls],
            (unsigned long)(HTTPD_QNAME_MAX + HTTPD_NSURI_MAX), x->attr);
    x->nsdecls++;
}

/* A complete start or end tag.  `selfclose` means both at once. */
static void hx_tag(HttpXml *x, int closing, int selfclose)
{
    const char *local = hx_local(x->name);

    if (!closing)
    {
        if (hx_equal(local, "prop"))
        {
            x->in_prop = 1;

            /* PROPPATCH's <prop> is a set of values and PROPFIND's is a list
               of names.  Only the second changes what the 207 reports on. */
            if (x->propfind)
                x->pf_mode = (unsigned char)HTTPD_PF_NAMED;
        }
        else if (hx_equal(local, "allprop"))
        {
            x->pf_mode = (unsigned char)HTTPD_PF_ALLPROP;
        }
        else if (hx_equal(local, "propname"))
        {
            x->pf_mode = (unsigned char)HTTPD_PF_PROPNAME;
        }
        else if (hx_equal(local, "owner"))
        {
            x->in_owner  = 1;
            x->owner[0]  = '\0';
        }
        else if (x->in_prop)
        {
            hx_note_property(x);
        }

        x->text_n = 0;
    }

    if (closing || selfclose)
    {
        if (hx_equal(local, "prop"))
            x->in_prop = 0;
        else if (hx_equal(local, "owner"))
        {
            /* The collection stopped where the buffer did, which is not where
               a character ends. */
            http_utf8_trim(x->owner);
            x->in_owner = 0;
        }
        else if (x->in_prop)
            hx_set_property(x);
    }
}

/* Everything it is in the middle of is in the struct, not on the stack. */
void http_xml_feed(HttpXml *x, const unsigned char *data, long len)
{
    long i;

    for (i = 0; i < len; i++)
    {
        int ch = data[i];

        switch (x->state)
        {
            case XML_TEXT:
                if (ch == '<')
                {
                    x->state  = XML_NAME;
                    x->name_n = 0;
                    x->close  = 0;
                    x->attr_n = 0;
                }
                else if (x->in_owner)
                {
                    unsigned long n = hx_len(x->owner);

                    /* Markup inside <owner> contributes nothing and its
                       text does.  Everything from 0x20 up but DEL, so a
                       UTF-8 owner keeps its bytes. */
                    if (n + 1UL < sizeof(x->owner) && ch >= 0x20 && ch != 0x7f)
                    {
                        x->owner[n]     = (char)ch;
                        x->owner[n + 1] = '\0';
                    }
                }
                else if (x->text_n + 1U < sizeof(x->text))
                {
                    x->text[x->text_n++] = (char)ch;
                }
                break;

            case XML_NAME:
                if (ch == '/' && x->name_n == 0U)
                {
                    x->close = 1;
                }
                else if (ch == '>' || ch == ' ' || ch == '\t' ||
                         ch == '\r' || ch == '\n' || ch == '/')
                {
                    x->name[x->name_n] = '\0';

                    if (ch == '>')
                    {
                        hx_tag(x, x->close ? 1 : 0, 0);
                        x->state = XML_TEXT;
                        if (!x->close)
                            x->text_n = 0;
                    }
                    else if (ch == '/')
                    {
                        hx_tag(x, 0, 1);
                        x->state = XML_ATTRS;
                        x->close = 1;       /* the '>' has nothing left to do */
                    }
                    else
                    {
                        x->state  = XML_ATTRS;
                        x->attr_n = 0;
                    }
                }
                else if (x->name_n + 1U < sizeof(x->name))
                {
                    x->name[x->name_n++] = (char)ch;
                }
                break;

            case XML_ATTRS:
                if (ch == '"' || ch == '\'')
                {
                    if (x->attr_n + 1U < sizeof(x->attr))
                        x->attr[x->attr_n++] = '"';
                    x->state = XML_QUOTE;
                }
                else if (ch == '>')
                {
                    if (x->close)
                        x->state = XML_TEXT;
                    else
                    {
                        hx_tag(x, 0, 0);
                        x->state  = XML_TEXT;
                        x->text_n = 0;
                    }
                }
                else if (ch == '/')
                {
                    /* "<x a=1/>": a start and an end with nothing between. */
                    if (!x->close)
                    {
                        hx_tag(x, 0, 1);
                        x->close = 1;
                    }
                }
                else if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
                {
                    x->attr[x->attr_n] = '\0';
                    hx_note_nsdecl(x);
                    x->attr_n = 0;
                }
                else if (x->attr_n + 1U < sizeof(x->attr))
                {
                    x->attr[x->attr_n++] = (char)ch;
                }
                break;

            default:                        /* XML_QUOTE                   */
                if (ch == '"' || ch == '\'')
                {
                    if (x->attr_n + 1U < sizeof(x->attr))
                        x->attr[x->attr_n++] = '"';
                    x->attr[x->attr_n] = '\0';
                    hx_note_nsdecl(x);
                    x->attr_n = 0;
                    x->state  = XML_ATTRS;
                }
                else if (x->attr_n + 1U < sizeof(x->attr))
                {
                    x->attr[x->attr_n++] = (char)ch;
                }
                break;
        }
    }
}

/* ------------------------------------------------------------- the answer --- */

int http_xml_want(HttpXml *x, const char *name)
{
    unsigned char i;

    if (x->pf_mode != (unsigned char)HTTPD_PF_NAMED)
        return 1;

    for (i = 0; i < x->props; i++)
    {
        if (hx_equal(hx_local(x->prop_name[i]), name))
        {
            x->prop_ok[i] = 1;
            return 1;
        }
    }

    return 0;
}
