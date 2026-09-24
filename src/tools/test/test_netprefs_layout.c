#include "netprefs_layout.h"

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

static int inside(const NpBox *outer, const NpBox *b, int inset)
{
    return b->x >= outer->x + inset && b->y >= outer->y + inset &&
           b->x + b->w <= outer->x + outer->w - inset &&
           b->y + b->h <= outer->y + outer->h - inset;
}

static int overlap(const NpBox *a, const NpBox *b)
{
    if (a->w == 0 || b->w == 0) return 0;
    return a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}

static int shown_together(int a, int b)
{
    int pa = np_items[a].page, pb = np_items[b].page;
    return pa == pb || pa == NP_PAGE_COMMON || pb == NP_PAGE_COMMON;
}

static void verify(const Fake *f, const NpLayout *l)
{
    NpBox window = { 0, 0, l->inner_w, l->inner_h };
    int i, j;

    for (i = 0; i < NP_L_COUNT; i++)
    {
        const NpBox *b = &l->box[i];
        const NpBox *t = &l->label[i];
        int page = np_items[i].page;

        check(b->w > 0 && b->h > 0, f->name, np_items[i].label ?
              np_items[i].label : "unlabelled box empty", b->w, b->h);
        check(inside(&window, b, 1), f->name, "box leaves the window", i, 0);
        if (t->w != 0)
            check(inside(&window, t, 0), f->name, "label leaves the window",
                  i, t->x);

        /* Text-bearing gadgets leave room for their frame. */
        if (i != NP_L_NOTE && np_items[i].place != NP_PLACE_RIGHT &&
            i != NP_L_INTERFACE)
            check(b->h >= f->height + 6, f->name, "gadget shorter than text",
                  i, b->h);
        if (np_items[i].place != NP_PLACE_IN)
            check(!overlap(t, b), f->name, "label crosses its own gadget",
                  i, t->x);
        if (np_items[i].place == NP_PLACE_IN && i != NP_L_NOTE)
            check(t->w + 8 <= b->w, f->name, "caption touches its frame",
                  i, b->w - t->w);

        if (page != NP_PAGE_COMMON)
            check(inside(&l->bevel, b, 2) &&
                  (t->w == 0 || inside(&l->bevel, t, 2)),
                  f->name, "page item outside its frame", i, 0);

        /* The separator runs between the columns. */
        if (b->x < l->sep_x + 2 && b->x + b->w > l->sep_x &&
            b->y <= l->sep_bottom && b->y + b->h >= l->sep_top)
            check(0, f->name, "box crosses the separator", i, 0);

        for (j = i + 1; j < NP_L_COUNT; j++)
        {
            if (!shown_together(i, j)) continue;
            check(!overlap(b, &l->box[j]), f->name, "boxes overlap", i, j);
            check(!overlap(t, &l->box[j]) || np_items[i].place == NP_PLACE_IN,
                  f->name, "label crosses a box", i, j);
            check(!overlap(&l->label[j], b) || np_items[j].place == NP_PLACE_IN,
                  f->name, "label crosses a box", j, i);
            check(!overlap(t, &l->label[j]), f->name, "labels overlap", i, j);
        }
        if (page != NP_PAGE_COMMON)
            check(!overlap(&l->bevel, &l->box[NP_L_STATUS]) &&
                  !overlap(&l->bevel, &l->box[NP_L_PANEL]),
                  f->name, "frame crosses a common gadget", i, 0);
    }

    check(l->box[NP_L_DEVICE].x + l->box[NP_L_DEVICE].w + 8 <=
          l->box[NP_L_DEVICE_BROWSE].x,
          f->name, "device and browse button need a gap", 0, 0);
    check(l->box[NP_L_DEVICE].y == l->box[NP_L_DEVICE_BROWSE].y,
          f->name, "device and browse button are not aligned", 0, 0);
}

int main(void)
{
    /* max_h: 640x200 NTSC and 640x256 PAL less a title bar in that font. */
    static const Fake fits[] =
    {
        { "topaz8",       8, 6, 8, 8, 8, 632 },
        { "topaz11",     11, 8, 8, 8, 8, 632 },
        { "helvetica13", 13, 10, 4, 8, 12, 632 },
        { "courier15",   15, 11, 9, 9, 9, 792 },
    };
    /* Too wide for 640; NetPrefs falls back to topaz 8. */
    static const Fake too_big[] =
    {
        { "courier15",   15, 11, 9, 9, 9, 632 },
        { "emerald20",   20, 15, 10, 13, 17, 632 },
    };
    NpLayout l;
    size_t i;

    for (i = 0; i < sizeof(fits) / sizeof(fits[0]); i++)
    {
        const Fake *f = &fits[i];
        NpFont font = { f->height, f->baseline, fake_measure, (void *)f };
        int max_h = 256 - (f->height + 5);

        memset(&l, 0, sizeof(l));
        check(np_layout(&font, f->max_w, max_h, &l), f->name, "does not fit",
              l.inner_w, l.inner_h);
        verify(f, &l);
    }

    {
        NpFont font = { 8, 6, fake_measure, (void *)&fits[0] };
        check(np_layout(&font, 632, 200 - 13, &l), "topaz8",
              "does not fit a stock NTSC Workbench", l.inner_w, l.inner_h);
        check(l.inner_w == 632, "topaz8", "not the stock 640-pixel width",
              l.inner_w, 632);
    }
    for (i = 0; i < sizeof(too_big) / sizeof(too_big[0]); i++)
    {
        const Fake *f = &too_big[i];
        NpFont font = { f->height, f->baseline, fake_measure, (void *)f };

        check(!np_layout(&font, f->max_w, 256 - (f->height + 5), &l),
              f->name, "claims to fit 640", l.inner_w, l.inner_h);
        verify(f, &l);
    }

    if (failures == 0) printf("netprefs layout: all checks passed\n");
    return failures != 0;
}
