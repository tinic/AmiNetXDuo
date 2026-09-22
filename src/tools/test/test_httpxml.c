/* Tests for httpxml.c, the skimmer behind PROPFIND, PROPPATCH and LOCK.
 *
 * Each body is fed whole and then again in pieces, down to one byte at a
 * time, and the two skims have to agree: the sink hands the skimmer whatever
 * a recv() returned, and a tag split across two reads is the same tag.
 *
 * SPDX-License-Identifier: MIT
 */

#include "httpxml.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

/* The caller's half: what the date hook was handed, and what it answers. */
static char date_seen[HTTPD_TEXT_MAX];
static int  date_calls;
static int  date_answer = 1;

static int date_hook(void *ctx, const char *text)
{
    CHECK(ctx == (void *)&date_calls);
    date_calls++;
    strncpy(date_seen, text, sizeof(date_seen) - 1);
    date_seen[sizeof(date_seen) - 1] = '\0';
    return date_answer;
}

static void fresh(HttpXml *x, int propfind)
{
    memset(x, 0, sizeof(*x));
    http_xml_reset(x);
    x->propfind = (unsigned char)propfind;
    x->date     = date_hook;
    x->date_ctx = &date_calls;
    date_seen[0] = '\0';
    date_calls   = 0;
}

static void feed(HttpXml *x, const char *body)
{
    http_xml_feed(x, (const unsigned char *)body, (long)strlen(body));
}

/* Feed `body` in pieces of `step` bytes. */
static void feed_pieces(HttpXml *x, const char *body, unsigned long step)
{
    unsigned long len = strlen(body);
    unsigned long at  = 0;

    while (at < len)
    {
        unsigned long take = (len - at < step) ? (len - at) : step;

        http_xml_feed(x, (const unsigned char *)&body[at], (long)take);
        at += take;
    }
}

/* What the 207 and the lock reply read out of a skim. */
static int same_outcome(const HttpXml *a, const HttpXml *b)
{
    unsigned char i;

    if (a->pf_mode != b->pf_mode || a->props != b->props ||
        a->props_cut != b->props_cut || a->nsdecls != b->nsdecls ||
        a->have_date != b->have_date)
        return 0;

    for (i = 0; i < a->props; i++)
    {
        if (strcmp(a->prop_name[i], b->prop_name[i]) != 0 ||
            a->prop_ok[i] != b->prop_ok[i])
            return 0;
    }

    for (i = 0; i < a->nsdecls; i++)
    {
        if (strcmp(a->nsdecl[i], b->nsdecl[i]) != 0)
            return 0;
    }

    return strcmp(a->owner, b->owner) == 0;
}

/* Every split of the body has to agree with the whole. */
static void check_splits(const char *body, int propfind)
{
    HttpXml       whole;
    HttpXml       part;
    unsigned long step;

    fresh(&whole, propfind);
    feed(&whole, body);

    for (step = 1; step <= 7; step++)
    {
        fresh(&part, propfind);
        feed_pieces(&part, body, step);
        CHECK(same_outcome(&whole, &part));
    }
}

static const char propfind_allprop[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
    "<D:propfind xmlns:D=\"DAV:\"><D:allprop/></D:propfind>";

static const char propfind_propname[] =
    "<?xml version=\"1.0\"?>"
    "<D:propfind xmlns:D=\"DAV:\">\n  <D:propname />\n</D:propfind>";

static const char propfind_named[] =
    "<?xml version=\"1.0\"?>"
    "<D:propfind xmlns:D=\"DAV:\" xmlns:Z=\"urn:schemas-microsoft-com:\">"
    "<D:prop>"
    "<D:getcontentlength/><D:resourcetype/>"
    "<Z:Win32FileAttributes/>"
    "</D:prop></D:propfind>";

static const char proppatch_date[] =
    "<?xml version=\"1.0\"?>"
    "<D:propertyupdate xmlns:D=\"DAV:\" xmlns:Z=\"urn:schemas-microsoft-com:\">"
    "<D:set><D:prop>"
    "<Z:Win32LastModifiedTime>Wed, 12 Jun 2024 10:00:00 GMT"
    "</Z:Win32LastModifiedTime>"
    "<Z:Win32FileAttributes>00000020</Z:Win32FileAttributes>"
    "</D:prop></D:set></D:propertyupdate>";

static const char lockinfo[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"
    "<D:lockinfo xmlns:D='DAV:'>"
    "<D:lockscope><D:exclusive/></D:lockscope>"
    "<D:locktype><D:write/></D:locktype>"
    "<D:owner>\r\n  <D:href>http://amiga.local/turo</D:href>\r\n</D:owner>"
    "</D:lockinfo>";

static void test_propfind(void)
{
    HttpXml x;

    printf("PROPFIND bodies\n");

    fresh(&x, 1);
    CHECK(x.pf_mode == HTTPD_PF_ALLPROP);       /* an empty body is allprop */

    fresh(&x, 1);
    feed(&x, propfind_allprop);
    CHECK(x.pf_mode == HTTPD_PF_ALLPROP);
    CHECK(x.props == 0);
    CHECK(x.nsdecls == 1);
    CHECK(strcmp(x.nsdecl[0], "xmlns:D=\"DAV:\"") == 0);

    fresh(&x, 1);
    feed(&x, propfind_propname);
    CHECK(x.pf_mode == HTTPD_PF_PROPNAME);
    CHECK(x.props == 0);

    fresh(&x, 1);
    feed(&x, propfind_named);
    CHECK(x.pf_mode == HTTPD_PF_NAMED);
    CHECK(x.props == 3);
    CHECK(strcmp(x.prop_name[0], "D:getcontentlength") == 0);
    CHECK(strcmp(x.prop_name[1], "D:resourcetype") == 0);
    CHECK(strcmp(x.prop_name[2], "Z:Win32FileAttributes") == 0);
    CHECK(x.prop_ok[0] == 0 && x.prop_ok[1] == 0 && x.prop_ok[2] == 0);
    CHECK(x.nsdecls == 2);
    CHECK(strcmp(x.nsdecl[0], "xmlns:D=\"DAV:\"") == 0);
    CHECK(strcmp(x.nsdecl[1], "xmlns:Z=\"urn:schemas-microsoft-com:\"") == 0);
    CHECK(x.in_prop == 0);
    CHECK(x.props_cut == 0);

    /* The same <prop> from PROPPATCH is values, not a request for names. */
    fresh(&x, 0);
    feed(&x, propfind_named);
    CHECK(x.pf_mode == HTTPD_PF_ALLPROP);
    CHECK(x.props == 3);

    /* Element names are matched on their local part, however prefixed. */
    fresh(&x, 1);
    feed(&x, "<propfind xmlns=\"DAV:\"><prop><displayname/></prop></propfind>");
    CHECK(x.pf_mode == HTTPD_PF_NAMED);
    CHECK(x.props == 1 && strcmp(x.prop_name[0], "displayname") == 0);
    CHECK(x.nsdecls == 1 && strcmp(x.nsdecl[0], "xmlns=\"DAV:\"") == 0);

    fresh(&x, 1);
    feed(&x, "<a:propfind xmlns:a=\"DAV:\"><a:PROP><a:x/></a:PROP></a:propfind>");
    CHECK(x.pf_mode == HTTPD_PF_NAMED && x.props == 1);

    /* More names than there is room to answer about. */
    fresh(&x, 1);
    feed(&x, "<D:propfind xmlns:D=\"DAV:\"><D:prop>"
             "<D:p1/><D:p2/><D:p3/><D:p4/><D:p5/><D:p6/><D:p7/><D:p8/>"
             "<D:p9/><D:p10/>"
             "</D:prop></D:propfind>");
    CHECK(x.props == HTTPD_PROPS_MAX);
    CHECK(x.props_cut == 1);
    CHECK(strcmp(x.prop_name[HTTPD_PROPS_MAX - 1], "D:p8") == 0);

    /* Names outside <prop> are structure, not properties. */
    fresh(&x, 1);
    feed(&x, "<D:propfind xmlns:D=\"DAV:\"><D:include><D:x/></D:include>"
             "<D:allprop/></D:propfind>");
    CHECK(x.props == 0 && x.pf_mode == HTTPD_PF_ALLPROP);

    check_splits(propfind_allprop, 1);
    check_splits(propfind_propname, 1);
    check_splits(propfind_named, 1);
}

static void test_namespaces(void)
{
    HttpXml x;

    printf("namespace declarations\n");

    /* Single quotes come out as double, which is what the reply writes. */
    fresh(&x, 1);
    feed(&x, "<D:propfind xmlns:D='DAV:'><D:allprop/></D:propfind>");
    CHECK(x.nsdecls == 1 && strcmp(x.nsdecl[0], "xmlns:D=\"DAV:\"") == 0);

    /* Declared twice is carried once. */
    fresh(&x, 1);
    feed(&x, "<D:propfind xmlns:D=\"DAV:\"><D:prop xmlns:D=\"DAV:\">"
             "<D:a/></D:prop></D:propfind>");
    CHECK(x.nsdecls == 1);

    /* HTTPD_NS_MAX and no more; the rest are not carried. */
    fresh(&x, 1);
    feed(&x, "<D:propfind xmlns:D=\"DAV:\" xmlns:A=\"a:\" xmlns:B=\"b:\" "
             "xmlns:C=\"c:\" xmlns:E=\"e:\"><D:allprop/></D:propfind>");
    CHECK(x.nsdecls == HTTPD_NS_MAX);
    CHECK(strcmp(x.nsdecl[2], "xmlns:B=\"b:\"") == 0);

    /* Attributes that are not declarations are not carried at all. */
    fresh(&x, 1);
    feed(&x, "<D:propfind version=\"1\" xmlns:D=\"DAV:\" lang='en'>"
             "<D:allprop/></D:propfind>");
    CHECK(x.nsdecls == 1 && strcmp(x.nsdecl[0], "xmlns:D=\"DAV:\"") == 0);

    /* Whitespace inside a tag, and a declaration on a self-closing one. */
    fresh(&x, 1);
    feed(&x, "<D:propfind\n\txmlns:D=\"DAV:\"\r\n>\n<D:allprop\txmlns:Z='z:' />"
             "</D:propfind>");
    CHECK(x.nsdecls == 2);
    CHECK(strcmp(x.nsdecl[0], "xmlns:D=\"DAV:\"") == 0);
    CHECK(strcmp(x.nsdecl[1], "xmlns:Z=\"z:\"") == 0);
    CHECK(x.pf_mode == HTTPD_PF_ALLPROP);

    /* A URI longer than the record is kept to what fits. */
    fresh(&x, 1);
    feed(&x, "<D:propfind xmlns:D=\"DAV:\" "
             "xmlns:L=\"urn:0123456789012345678901234567890123456789"
             "0123456789012345678901234567890123456789\">"
             "<D:allprop/></D:propfind>");
    CHECK(x.nsdecls == 2);
    CHECK(strlen(x.nsdecl[1]) == HTTPD_QNAME_MAX + HTTPD_NSURI_MAX - 1);
}

static void test_proppatch(void)
{
    HttpXml x;

    printf("PROPPATCH values\n");

    fresh(&x, 0);
    feed(&x, proppatch_date);
    CHECK(x.props == 2);
    CHECK(strcmp(x.prop_name[0], "Z:Win32LastModifiedTime") == 0);
    CHECK(strcmp(x.prop_name[1], "Z:Win32FileAttributes") == 0);
    CHECK(date_calls == 1);
    CHECK(strcmp(date_seen, "Wed, 12 Jun 2024 10:00:00 GMT") == 0);
    CHECK(x.have_date == 1);
    CHECK(x.prop_ok[0] == 1);       /* the date was set                  */
    CHECK(x.prop_ok[1] == 0);       /* the attributes are answered 403   */
    CHECK(x.pf_mode == HTTPD_PF_ALLPROP);

    /* The caller could not read it as a date: nothing is set. */
    fresh(&x, 0);
    date_answer = 0;
    feed(&x, proppatch_date);
    date_answer = 1;
    CHECK(date_calls == 1);
    CHECK(x.have_date == 0 && x.prop_ok[0] == 0 && x.prop_ok[1] == 0);

    /* No hook at all: no property is ever set. */
    fresh(&x, 0);
    x.date = 0;
    feed(&x, proppatch_date);
    CHECK(x.props == 2 && x.have_date == 0 && x.prop_ok[0] == 0);

    /* getlastmodified is the same date under DAV:'s own name. */
    fresh(&x, 0);
    feed(&x, "<D:propertyupdate xmlns:D=\"DAV:\"><D:set><D:prop>"
             "<D:getlastmodified>Mon, 01 Jan 2024 00:00:00 GMT"
             "</D:getlastmodified></D:prop></D:set></D:propertyupdate>");
    CHECK(date_calls == 1 && x.have_date == 1 && x.prop_ok[0] == 1);
    CHECK(strcmp(date_seen, "Mon, 01 Jan 2024 00:00:00 GMT") == 0);

    /* Any other value is not offered to the caller. */
    fresh(&x, 0);
    feed(&x, "<D:propertyupdate xmlns:D=\"DAV:\"><D:set><D:prop>"
             "<D:displayname>readme</D:displayname>"
             "</D:prop></D:set></D:propertyupdate>");
    CHECK(x.props == 1 && date_calls == 0 && x.have_date == 0);

    /* A value longer than HTTPD_TEXT_MAX is handed over cut. */
    fresh(&x, 0);
    feed(&x, "<D:propertyupdate xmlns:D=\"DAV:\"><D:set><D:prop>"
             "<D:getlastmodified>0123456789012345678901234567890123456789"
             "0123456789ABCDEF</D:getlastmodified>"
             "</D:prop></D:set></D:propertyupdate>");
    CHECK(date_calls == 1);
    CHECK(strlen(date_seen) == HTTPD_TEXT_MAX - 1);

    check_splits(proppatch_date, 0);
}

static void test_lock_owner(void)
{
    HttpXml x;

    printf("LOCK owner\n");

    /* Markup inside <owner> contributes nothing and its text does, which
       includes the indentation a pretty-printed body puts before <href>. */
    fresh(&x, 0);
    feed(&x, lockinfo);
    CHECK(strcmp(x.owner, "  http://amiga.local/turo") == 0);
    CHECK(x.in_owner == 0);
    CHECK(x.props == 0);
    CHECK(x.nsdecls == 1 && strcmp(x.nsdecl[0], "xmlns:D=\"DAV:\"") == 0);

    /* Plain text, with the control characters dropped and a UTF-8 owner
       kept byte for byte. */
    fresh(&x, 0);
    feed(&x, "<D:lockinfo xmlns:D=\"DAV:\"><D:owner>Turo\tR\x7f\xc3\xb6"
             "\n</D:owner></D:lockinfo>");
    CHECK(strcmp(x.owner, "TuroR\xc3\xb6") == 0);

    /* Bounded, and not cut inside a character. */
    {
        char body[HTTPD_OWNER_MAX * 2 + 64];
        unsigned long n;

        strcpy(body, "<D:lockinfo xmlns:D=\"DAV:\"><D:owner>");
        n = strlen(body);
        while (n < strlen("<D:lockinfo xmlns:D=\"DAV:\"><D:owner>") +
                   HTTPD_OWNER_MAX - 2)
            body[n++] = 'o';
        /* The 127th byte starts a two-byte character that cannot finish. */
        body[n++] = (char)0xc3;
        body[n++] = (char)0xb6;
        strcpy(&body[n], "</D:owner></D:lockinfo>");

        fresh(&x, 0);
        feed(&x, body);
        CHECK(strlen(x.owner) == HTTPD_OWNER_MAX - 2);
        CHECK(x.owner[HTTPD_OWNER_MAX - 3] == 'o');
    }

    /* No owner element: the owner stays empty. */
    fresh(&x, 0);
    feed(&x, "<D:lockinfo xmlns:D=\"DAV:\"><D:lockscope><D:exclusive/>"
             "</D:lockscope></D:lockinfo>");
    CHECK(x.owner[0] == '\0');

    check_splits(lockinfo, 0);
}

static void test_malformed(void)
{
    HttpXml x;
    char    body[512];

    printf("malformed and cut bodies\n");

    /* A tag that never closes names nothing. */
    fresh(&x, 1);
    feed(&x, "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:getcontentleng");
    CHECK(x.props == 0);
    CHECK(x.in_prop == 1);
    CHECK(x.state == XML_NAME);

    /* Empty and stray brackets. */
    fresh(&x, 1);
    feed(&x, "<><//>>>\"'<D:allprop/>");
    CHECK(x.pf_mode == HTTPD_PF_ALLPROP);

    /* Text before any element, and text alone. */
    fresh(&x, 1);
    feed(&x, "not xml at all");
    CHECK(x.props == 0 && x.pf_mode == HTTPD_PF_ALLPROP);

    /* An element name longer than the record is kept to what fits and is
       still one property. */
    strcpy(body, "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:");
    memset(&body[strlen(body)], 'n', 60);
    body[strlen("<D:propfind xmlns:D=\"DAV:\"><D:prop><D:") + 60] = '\0';
    strcat(body, "/></D:prop></D:propfind>");
    fresh(&x, 1);
    feed(&x, body);
    CHECK(x.props == 1);
    CHECK(strlen(x.prop_name[0]) == HTTPD_QNAME_MAX - 1);

    /* An attribute value that never closes swallows the rest. */
    fresh(&x, 1);
    feed(&x, "<D:propfind xmlns:D=\"DAV:><D:allprop/></D:propfind>");
    CHECK(x.state == XML_QUOTE);
    CHECK(x.nsdecls == 0);

    /* "<x a=1/>": a start and an end with nothing between. */
    fresh(&x, 1);
    feed(&x, "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:a b=1/><D:c/>"
             "</D:prop></D:propfind>");
    CHECK(x.props == 2);
    CHECK(strcmp(x.prop_name[0], "D:a") == 0);
    CHECK(strcmp(x.prop_name[1], "D:c") == 0);
}

static void test_want(void)
{
    HttpXml x;

    printf("what the 207 reports on\n");

    /* allprop and propname: everything is wanted and nothing is marked. */
    fresh(&x, 1);
    feed(&x, propfind_allprop);
    CHECK(http_xml_want(&x, "getcontentlength"));
    CHECK(http_xml_want(&x, "anything"));
    fresh(&x, 1);
    feed(&x, propfind_propname);
    CHECK(http_xml_want(&x, "displayname"));

    /* A named list: the local part is what is asked about, and each match
       is noted so the rest can go in the 404 propstat. */
    fresh(&x, 1);
    feed(&x, propfind_named);
    CHECK(http_xml_want(&x, "getcontentlength"));
    CHECK(x.prop_ok[0] == 1 && x.prop_ok[1] == 0 && x.prop_ok[2] == 0);
    CHECK(!http_xml_want(&x, "displayname"));
    CHECK(http_xml_want(&x, "Win32FileAttributes"));
    CHECK(x.prop_ok[2] == 1);
    CHECK(!http_xml_want(&x, "Z:Win32FileAttributes"));  /* not the qname */
    CHECK(http_xml_want(&x, "RESOURCETYPE"));            /* case-insensitive */
    CHECK(x.prop_ok[1] == 1);
    CHECK(x.prop_ok[0] == 1 && x.prop_ok[1] == 1 && x.prop_ok[2] == 1);

    /* An empty named list wants nothing. */
    fresh(&x, 1);
    feed(&x, "<D:propfind xmlns:D=\"DAV:\"><D:prop/></D:propfind>");
    CHECK(x.pf_mode == HTTPD_PF_NAMED && x.props == 0);
    CHECK(!http_xml_want(&x, "resourcetype"));
}

static void test_reset(void)
{
    HttpXml x;

    printf("reset between requests\n");

    fresh(&x, 1);
    feed(&x, propfind_named);
    feed(&x, "<D:owner>somebody</D:owner><D:prop><D:extra");
    CHECK(x.props == 3 && x.owner[0] != '\0');

    http_xml_reset(&x);
    CHECK(x.state == XML_TEXT);
    CHECK(x.pf_mode == HTTPD_PF_ALLPROP);
    CHECK(x.name_n == 0 && x.close == 0 && x.text_n == 0 && x.attr_n == 0);
    CHECK(x.in_prop == 0 && x.in_owner == 0 && x.propfind == 0);
    CHECK(x.props == 0 && x.props_cut == 0 && x.nsdecls == 0);
    CHECK(x.have_date == 0 && x.owner[0] == '\0');
    CHECK(x.date == date_hook && x.date_ctx == &date_calls);

    /* And the next body skims as though it were the first. */
    x.propfind = 1;
    feed(&x, propfind_allprop);
    CHECK(x.pf_mode == HTTPD_PF_ALLPROP && x.props == 0 && x.nsdecls == 1);
}

int main(void)
{
    test_propfind();
    test_namespaces();
    test_proppatch();
    test_lock_owner();
    test_malformed();
    test_want();
    test_reset();

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
