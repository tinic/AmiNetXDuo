/*
 * NetWirelessPrefs' three windows at topaz 8, 11, a proportional 13 and a
 * wide 15: every gadget inside its window, none overlapping another or
 * another's label, captions inside their buttons; a font too large for 640
 * says it does not fit.  And the hidden passphrase's edit arithmetic.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netwireless_layout.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(int yes, const char *font, const char *what, int a, int b)
{
    if (!yes)
    {
        fprintf(stderr, "FAIL: %s: %s (%d, %d)\n", font, what, a, b);
        failures++;
    }
}

typedef struct Fake
{
    const char *name;
    int height, narrow, normal, wide;
    int max_w;
} Fake;

static int fake_measure(void *ctx, const char *text, int len)
{
    const Fake *f = (const Fake *)ctx;
    int w = 0, i;

    for (i = 0; i < len; i++)
    {
        char c = text[i];
        if (strchr("il.:1 ", c) != NULL) w += f->narrow;
        else if (strchr("MWmw&", c) != NULL) w += f->wide;
        else w += f->normal;
    }
    return w;
}

static int inside(int w, int h, const NwBox *b, int inset)
{
    return b->x >= inset && b->y >= inset &&
           b->x + b->w <= w - inset && b->y + b->h <= h - inset;
}

static int overlap(const NwBox *a, const NwBox *b)
{
    if (a->w == 0 || b->w == 0) return 0;
    return a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}

static void verify(const Fake *f, const NwLayout *l)
{
    int i, j;

    for (i = 0; i < NW_L_COUNT; i++)
    {
        const NwBox *b = &l->box[i], *t = &l->label[i];
        int win = nw_items[i].window;
        int w = l->inner_w[win], h = l->inner_h[win];

        check(b->w > 0 && b->h > 0, f->name, "empty box", i, 0);
        check(inside(w, h, b, 1), f->name, "box leaves its window", i, 0);
        if (t->w != 0) check(inside(w, h, t, 0), f->name, "label leaves its window", i, t->x);
        if (nw_items[i].place != NW_PLACE_RIGHT)
            check(b->h >= f->height + 6, f->name, "gadget shorter than text", i, b->h);
        if (nw_items[i].place == NW_PLACE_IN)
            check(t->w + 8 <= b->w, f->name, "caption touches its frame", i, b->w - t->w);
        else
            check(!overlap(t, b), f->name, "label crosses its own gadget", i, 0);

        for (j = i + 1; j < NW_L_COUNT; j++)
        {
            if (nw_items[j].window != win) continue;
            check(!overlap(b, &l->box[j]), f->name, "boxes overlap", i, j);
            check(!overlap(t, &l->box[j]) || nw_items[i].place == NW_PLACE_IN,
                  f->name, "label crosses a box", i, j);
            check(!overlap(&l->label[j], b) || nw_items[j].place == NW_PLACE_IN,
                  f->name, "label crosses a box", j, i);
            check(!overlap(t, &l->label[j]), f->name, "labels overlap", i, j);
        }
    }

    /* A list row's parts in order, inside the list. */
    check(l->bars_w > 0 && l->level_x > l->bars_w && l->lock_x > l->level_x &&
          l->lock_x + f->height + 16 + 8 <= l->box[NW_M_LIST].w,
          f->name, "list row columns", l->lock_x, l->box[NW_M_LIST].w);
}

static void test_secret(void)
{
    char real[8] = "";
    int  len = 0;

    /* "ab", cursor home, "X" in front, cursor end, delete back twice. */
    check(nw_secret_edit(real, &len, 8, NW_EDIT_INSERT, 1, 1, 'a') &&
          nw_secret_edit(real, &len, 8, NW_EDIT_INSERT, 2, 2, 'b') &&
          strcmp(real, "ab") == 0, "secret", "typed", len, 0);
    check(nw_secret_edit(real, &len, 8, NW_EDIT_OTHER, 0, 2, 0) &&
          nw_secret_edit(real, &len, 8, NW_EDIT_INSERT, 1, 3, 'X') &&
          strcmp(real, "Xab") == 0, "secret", "inserted in front", len, 0);
    check(nw_secret_edit(real, &len, 8, NW_EDIT_REPLACE, 2, 3, 'Y') &&
          strcmp(real, "XYb") == 0, "secret", "replace", len, 0);
    check(nw_secret_edit(real, &len, 8, NW_EDIT_OTHER, 2, 2, 0) &&
          strcmp(real, "XY") == 0, "secret", "delete back at end", len, 0);
    check(nw_secret_edit(real, &len, 8, NW_EDIT_OTHER, 0, 1, 0) &&
          strcmp(real, "Y") == 0, "secret", "delete forward at home", len, 0);
    check(!nw_secret_edit(real, &len, 8, NW_EDIT_OTHER, 0, 4, 0) &&
          strcmp(real, "Y") == 0, "secret", "a paste is refused", len, 0);
    check(nw_secret_edit(real, &len, 8, NW_EDIT_OTHER, 0, 0, 0) &&
          len == 0 && real[0] == '\0', "secret", "clear", len, 0);
    while (len < 7) nw_secret_edit(real, &len, 8, NW_EDIT_INSERT, len + 1, len + 1, 'z');
    check(!nw_secret_edit(real, &len, 8, NW_EDIT_INSERT, 8, 8, 'z') && len == 7,
          "secret", "full buffer refuses more", len, 0);
    check(!nw_secret_edit(real, &len, 8, NW_EDIT_OTHER, 6, 5, 0),
          "secret", "a deletion past the end is refused", len, 0);
}

int main(void)
{
    static const Fake fits[] =
    {
        { "topaz8",       8, 8, 8, 8, 632 },
        { "topaz11",     11, 8, 8, 8, 632 },
        { "helvetica13", 13, 4, 8, 12, 632 },
        { "courier15",   15, 9, 9, 9, 792 },
    };
    static const Fake too_big = { "emerald30", 30, 14, 20, 26, 632 };
    NwLayout l;
    size_t   i;

    for (i = 0; i < sizeof(fits) / sizeof(fits[0]); i++)
    {
        const Fake *f = &fits[i];
        NwFont font = { f->height, fake_measure, (void *)f };
        /* 640x200 NTSC for topaz 8, PAL 256 for the rest, less a title. */
        int max_h = (f->height == 8 ? 200 : 256) - (f->height + 5);

        memset(&l, 0, sizeof(l));
        check(nw_layout(&font, f->max_w, max_h, &l), f->name, "does not fit",
              l.inner_w[0], l.inner_h[0]);
        verify(f, &l);
    }
    {
        NwFont font = { too_big.height, fake_measure, (void *)&too_big };
        check(!nw_layout(&font, too_big.max_w, 256 - 35, &l), too_big.name,
              "a font too large says so", l.inner_w[0], l.inner_h[0]);
    }
    test_secret();
    if (failures == 0) printf("netwireless layout: all passed\n");
    return failures == 0 ? 0 : 1;
}
