/*
 * httpxml, the XML skimmer behind PROPFIND, PROPPATCH and LOCK.
 *
 * Not a parser: it finds element names and the text between them, both
 * bounded, and has no opinion about anything else.  The namespace
 * declarations are carried through as they arrived rather than resolved.
 *
 * Split out of httpd.c so that what a body asked for can be asserted on a
 * host, one byte at a time if need be: the sink hands the skimmer whatever a
 * recv() returned, so a tag split across two reads has to come out the same
 * as one that arrived whole.  Everything it is in the middle of is in the
 * struct, not on the stack, for that reason.
 *
 * The one property value that maps to an AmigaDOS call, the modification
 * time, is parsed by the caller through `date`: this file does not know what
 * a DateStamp is, and the caller says whether the text was a date.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_HTTPXML_H
#define AMINETXDUO_HTTPXML_H

#include "httplock.h"

/* The XML the write methods send is skimmed, not parsed: element names and the
   text between them, both bounded, and no tree.  PROPPATCH names the properties
   it wants and LOCK names an owner, and nothing here needs more. */
#define HTTPD_PROPS_MAX        8    /* properties reported on in one 207    */

/* What a PROPFIND body asked for, RFC 4918 9.1. */
#define HTTPD_PF_ALLPROP       0
#define HTTPD_PF_PROPNAME      1
#define HTTPD_PF_NAMED         2
#define HTTPD_QNAME_MAX       32    /* "Z:Win32LastModifiedTime" is 23      */
#define HTTPD_NS_MAX           3    /* xmlns: bindings carried to the reply */
#define HTTPD_NSURI_MAX       48
#define HTTPD_TEXT_MAX        48    /* an element's character data          */

/* The skimmer's position. */
enum
{
    XML_TEXT = 0,       /* between elements                                */
    XML_NAME,           /* inside a tag, reading its name                  */
    XML_ATTRS,          /* inside a tag, past the name                     */
    XML_QUOTE           /* inside an attribute value                       */
};

typedef struct HttpXml HttpXml;

struct HttpXml
{
    unsigned char state;
    unsigned char name_n;
    char          name[HTTPD_QNAME_MAX];
    unsigned char close;            /* the tag being read is a </close>    */
    unsigned char text_n;
    char          text[HTTPD_TEXT_MAX];
    unsigned char attr_n;
    char          attr[HTTPD_QNAME_MAX + HTTPD_NSURI_MAX];
    unsigned char in_prop;          /* inside <prop>: children are names   */
    unsigned char in_owner;         /* inside <owner>: text is the owner   */
    unsigned char propfind;         /* <prop> is a list of names, not values */
    unsigned char props;
    unsigned char props_cut;        /* more named than there was room for  */
    unsigned char pf_mode;
    unsigned char prop_ok[HTTPD_PROPS_MAX];
    char          prop_name[HTTPD_PROPS_MAX][HTTPD_QNAME_MAX];
    unsigned char nsdecls;
    char          nsdecl[HTTPD_NS_MAX][HTTPD_QNAME_MAX + HTTPD_NSURI_MAX];
    unsigned char have_date;
    char          owner[HTTPD_OWNER_MAX];

    /* A Win32LastModifiedTime or getlastmodified value just closed.  Nonzero
       when `text` was a date the caller kept; the property is then reported
       as set.  Left NULL, no property is ever set. */
    int         (*date)(void *ctx, const char *text);
    void         *date_ctx;
};

/* Between requests.  `date` and `date_ctx` are left as they were. */
void http_xml_reset(HttpXml *x);

/* Feed the body through.  Must survive being handed one byte at a time. */
void http_xml_feed(HttpXml *x, const unsigned char *data, long len);

/* Whether this property belongs in the 207, and a note that it was asked for.
   Marking each one is what lets the caller put the rest in a 404 propstat,
   which RFC 4918 9.1.1 requires.  `name` is the local part. */
int http_xml_want(HttpXml *x, const char *name);

#endif /* AMINETXDUO_HTTPXML_H */
