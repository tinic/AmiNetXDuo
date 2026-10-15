#include "netswitch_layout.h"

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
    int height, baseline, narrow, normal, wide;
    int max_w;                      /* screen width less the borders */
    int max_h;                      /* screen height less a title bar */
} Fake;

/* Proportional when narrow/normal/wide differ, fixed when they do not. */
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

static int inside(const NsBox *outer, const NsBox *b, int inset)
{
    return b->x >= outer->x + inset && b->y >= outer->y + inset &&
           b->x + b->w <= outer->x + outer->w - inset &&
           b->y + b->h <= outer->y + outer->h - inset;
}

static int overlap(const NsBox *a, const NsBox *b)
{
    if (a->w <= 0 || b->w <= 0 || a->h <= 0 || b->h <= 0) return 0;
    return a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}

static int digit_w_of(const Fake *f)
{
    return (fake_measure((void *)f, "0123456789", 10) + 9) / 10;
}

static void verify(const Fake *f, const NsLayout *l, unsigned count)
{
    NsBox window = { 0, 0, l->inner_w, l->inner_h };
    const NsBox *list = &l->box[NS_L_LIST];
    int i, j, digit_w = digit_w_of(f);

    for (i = 0; i < NS_L_COUNT; i++)
    {
        check(l->box[i].w > 0 && l->box[i].h > 0, f->name, "box empty",
              l->box[i].w, l->box[i].h);
        check(inside(&window, &l->box[i], 0), f->name, "box leaves the window",
              i, l->box[i].x);
    }

    for (i = 0; i < NS_L_COUNT; i++)
        for (j = i + 1; j < NS_L_COUNT; j++)
            check(!overlap(&l->box[i], &l->box[j]), f->name, "boxes overlap",
                  i, j);

    /* The legend and the panel's lines must not run into the list, and the
       status line must be under everything. */
    check(l->box[NS_L_LEGEND].x + l->box[NS_L_LEGEND].w <= list->x ||
          l->box[NS_L_LEGEND].y + l->box[NS_L_LEGEND].h <= list->y,
          f->name, "legend crosses the list", 0, 0);
    check(l->box[NS_L_PANEL].x >= list->x + list->w, f->name,
          "panel over the list", l->box[NS_L_PANEL].x, list->x + list->w);
    check(l->box[NS_L_STATUS].y >= list->y + list->h, f->name,
          "status over the list", l->box[NS_L_STATUS].y, list->y + list->h);
    check(l->box[NS_L_ACTION].y >= l->box[NS_L_PANEL].y +
          l->box[NS_L_PANEL].h, f->name, "button over the panel", 0, 0);
    check(l->box[NS_L_ALL].y >= l->box[NS_L_ACTION].y + l->box[NS_L_ACTION].h,
          f->name, "the two buttons touch", l->box[NS_L_ACTION].h, 0);
    check(l->box[NS_L_HINT].y >= l->box[NS_L_ALL].y + l->box[NS_L_ALL].h,
          f->name, "hint over the buttons", 0, 0);

    /* A row has to hold its text and the glyph has to be a glyph. */
    check(l->row_h > f->height, f->name, "row shorter than its text",
          l->row_h, f->height);
    check(l->glyph_w >= f->height, f->name, "glyph smaller than the font",
          l->glyph_w, f->height);
    /* The two constants the renderer needs, not derived from the font, travel
       with the layout so netswitch.c cannot drift from this file. */
    check(l->cell_pad == l->glyph_x, f->name, "cell pad is not the glyph inset",
          l->cell_pad, l->glyph_x);
    check((l->scroll == 0) == (l->strip_w == 0), f->name,
          "strip width disagrees with scroll", l->strip_w, l->scroll);
    check(!l->scroll || l->strip_w >= 8, f->name,
          "strip cannot hold the arrows", l->strip_w, 8);
    check((unsigned)l->rows <= count && (count == 0 || l->rows >= 1), f->name,
          "row count", l->rows, (int)count);
    check(l->scroll == ((unsigned)l->rows < count), f->name,
          "scroll flag disagrees with the rows", l->scroll, l->rows);

    /* The columns run left to right inside the list, each as wide as it says
       it is, and the last one ends inside the frame. */
    check(l->glyph_x >= 0 && l->name_x >= l->glyph_x + l->glyph_w,
          f->name, "name column over the glyph", l->name_x,
          l->glyph_x + l->glyph_w);
    check(l->device_x >= l->name_x + l->name_chars * digit_w, f->name,
          "device column over the name", l->device_x,
          l->name_x + l->name_chars * digit_w);
    check(l->address_x >= l->device_x + l->device_chars * digit_w, f->name,
          "address column over the device", l->address_x,
          l->device_x + l->device_chars * digit_w);
    check(l->address_x + l->address_chars * digit_w <= list->w, f->name,
          "address column leaves the list", l->address_x +
          l->address_chars * digit_w, list->w);

    /* Only a scrolling list pays for the strip. */
    check(l->scroll || list->w == l->box[NS_L_LEGEND].w, f->name,
          "the legend and a still list disagree", list->w,
          l->box[NS_L_LEGEND].w);
}

int main(void)
{
    /* A stock Workbench at PAL: 640x256 less the window borders and a title
       bar, which is what NetPrefs is given too. */
    static const Fake fits[] =
    {
        { "topaz8",       8, 6, 8, 8, 8, 632, 256 - 13 },
        { "topaz11",     11, 8, 8, 8, 8, 632, 256 - 16 },
        { "helvetica13", 13, 10, 4, 8, 12, 632, 256 - 18 },
    };
    /* Wider than 640: NetSwitch falls back to topaz 8, as NetPrefs does. */
    static const Fake too_big[] =
    {
        { "courier15",   15, 11, 9, 9, 9, 632, 256 - 20 },
        { "emerald20",   20, 15, 10, 13, 17, 632, 256 - 25 },
    };
    NsLayout l;
    size_t i;

    for (i = 0; i < sizeof(fits) / sizeof(fits[0]); i++)
    {
        const Fake *f = &fits[i];
        NsFont font = { f->height, f->baseline, fake_measure, (void *)f };
        unsigned count;

        for (count = 0; count <= 12; count++)
        {
            memset(&l, 0, sizeof(l));
            check(ns_layout(&font, f->max_w, f->max_h, count, &l), f->name,
                  "does not fit", l.inner_w, l.inner_h);
            verify(f, &l, count);
        }
    }

    /* The panel has to show a link-local address whole; that is why the
       window is wider than the 480 it aims for. */
    {
        const Fake *f = &fits[0];
        NsFont font = { f->height, f->baseline, fake_measure, (void *)f };
        int link_local = fake_measure((void *)f, "fe80::211:22ff:fe33:4455", 25);

        memset(&l, 0, sizeof(l));
        check(ns_layout(&font, f->max_w, f->max_h, 3, &l), "topaz8",
              "does not fit three interfaces", l.inner_w, l.inner_h);
        check(l.panel_w >= link_local, "topaz8",
              "panel cannot show a link-local address", l.panel_w, link_local);
        check(l.inner_w <= f->max_w, "topaz8", "wider than the screen",
              l.inner_w, f->max_w);
        check(l.inner_w >= 480, "topaz8", "narrower than the target",
              l.inner_w, 480);
        check(l.scroll == 0, "topaz8", "scrolls three interfaces", l.scroll, 0);
        check(l.rows == 3, "topaz8", "not three rows", l.rows, 3);
    }

    /* NTSC is 640x200: the same window, fewer rows, and then it scrolls. */
    {
        const Fake *f = &fits[0];
        NsFont font = { f->height, f->baseline, fake_measure, (void *)f };
        int ntsc_h = 200 - 13;

        memset(&l, 0, sizeof(l));
        check(ns_layout(&font, 632, ntsc_h, 1, &l), "topaz8/ntsc",
              "does not fit one interface", l.inner_w, l.inner_h);
        check(l.rows == 1 && l.scroll == 0, "topaz8/ntsc",
              "one interface should not scroll", l.rows, l.scroll);

        check(ns_layout(&font, 632, ntsc_h, 8, &l), "topaz8/ntsc",
              "does not fit eight interfaces", l.inner_w, l.inner_h);
        check(l.scroll == 1 && l.rows >= 1, "topaz8/ntsc",
              "eight interfaces should scroll", l.rows, l.scroll);
        verify(f, &l, 8);
    }

    /* Too tall for anything: one row cannot be shown, so the caller is told
       to fall back rather than being handed a window off the screen. */
    {
        const Fake *f = &fits[0];
        NsFont font = { f->height, f->baseline, fake_measure, (void *)f };

        check(!ns_layout(&font, 632, 20, 3, &l), "topaz8", "fits in 20 rows",
              l.inner_h, 20);
        check(!ns_layout(&font, 100, 256, 3, &l), "topaz8", "fits in 100 wide",
              l.inner_w, 100);
    }

    for (i = 0; i < sizeof(too_big) / sizeof(too_big[0]); i++)
    {
        const Fake *f = &too_big[i];
        NsFont font = { f->height, f->baseline, fake_measure, (void *)f };

        check(!ns_layout(&font, f->max_w, f->max_h, 3, &l), f->name,
              "claims to fit 640", l.inner_w, l.inner_h);
    }

    /* Scrolling keeps the selected row on the screen and stops at the end. */
    {
        NsLayout s;

        memset(&s, 0, sizeof(s));
        s.rows = 4;
        check(ns_first_row(&s, 3, 0) == 0, "first-row", "short list scrolled",
              ns_first_row(&s, 3, 0), 0);
        check(ns_first_row(&s, 4, 3) == 0, "first-row", "exact list scrolled",
              ns_first_row(&s, 4, 3), 0);
        check(ns_first_row(&s, 9, 0) == 0, "first-row", "selection above",
              ns_first_row(&s, 9, 0), 0);
        check(ns_first_row(&s, 9, 4) == 1, "first-row", "selection below",
              ns_first_row(&s, 9, 4), 1);
        check(ns_first_row(&s, 9, 8) == 5, "first-row", "last row",
              ns_first_row(&s, 9, 8), 5);
        check(ns_first_row(&s, 9, 99) == 5, "first-row", "past the end",
              ns_first_row(&s, 9, 99), 5);
    }

    if (failures == 0) printf("netswitch layout: all checks passed\n");
    return failures != 0;
}
