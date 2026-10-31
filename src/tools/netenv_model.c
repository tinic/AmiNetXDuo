/* NetEnv's projection and change-only publisher, shared by native tests.
 * SPDX-License-Identifier: MIT */
#include "netenv_model.h"
#include <string.h>

static char ne_buffer[NE_VALUE_MAX]; /* single owner, never on the 4 KB stack */

static int append(char *out, size_t cap, const char *text)
{
    size_t n = strlen(out), add = strlen(text);
    if (n + add >= cap) return 0;
    memcpy(out + n, text, add + 1);
    return 1;
}

static void number(unsigned value, char out[11])
{
    char reverse[10];
    unsigned n = 0, i;
    do { reverse[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    for (i = 0; i < n; i++) out[i] = reverse[n - i - 1];
    out[n] = 0;
}

static void ipv4(uint32_t address, char out[16])
{
    unsigned i;
    char part[11];
    out[0] = 0;
    for (i = 0; i < 4; i++) {
        number((unsigned)((address >> (24 - 8 * i)) & 255), part);
        if (i) (void)append(out, 16, ".");
        (void)append(out, 16, part);
    }
}

static int same_name(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x = (char)(x + ('a' - 'A'));
        if (y >= 'A' && y <= 'Z') y = (char)(y + ('a' - 'A'));
        if (x != y) return 0;
    }
    return *a == *b;
}

int ne_name_valid(const char *name)
{
    unsigned i;
    for (i = 0; name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        if (i == 15 || !((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.')) return 0;
    }
    return i != 0 && strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

const char *ne_value(const NeTable *table, const char *name)
{
    unsigned i;
    for (i = 0; i < table->count; i++)
        if (strcmp(table->entry[i].name, name) == 0)
            return table->text + table->entry[i].offset;
    return NULL;
}

static int add(NeTable *table, const char *name, const char *value)
{
    size_t len = strlen(value) + 1;
    NeEntry *entry;
    if (table->count >= NE_KEY_MAX || strlen(name) > 31 ||
        table->used + len > NE_TEXT_MAX || ne_value(table, name)) return 0;
    entry = &table->entry[table->count++];
    strcpy(entry->name, name);
    entry->offset = table->used;
    memcpy(table->text + table->used, value, len);
    table->used += (unsigned)len;
    return 1;
}

static const NeInterface *interface_at(const NeSnapshot *s, unsigned index)
{
    unsigned i;
    for (i = 0; i < s->interfaces; i++)
        if (s->iface[i].index == index) return &s->iface[i];
    return NULL;
}

static int field(NeTable *out, const char *name, const char *field_name,
                 const char *value)
{
    char key[32] = "ANXD/IF/";
    return append(key, sizeof(key), name) && append(key, sizeof(key), "/") &&
        append(key, sizeof(key), field_name) && add(out, key, value);
}

/* Sort the complete ready-address strings: list order must not cause writes. */
static int addresses6(const NeSnapshot *s, const NeInterface *iface)
{
    const NeAddress6 *order[NE_ADDR6_MAX];
    unsigned count = 0, i, j;
    char item[80], prefix[11], previous[80] = "";
    ne_buffer[0] = 0;
    for (i = 0; i < s->addresses6; i++) {
        const NeAddress6 *a = &s->addr6[i];
        if (!a->ready || a->index != iface->index) continue;
        if (a->prefix > 128 || !memchr(a->text, 0, sizeof(a->text))) return 0;
        j = count;
        while (j && strcmp(order[j-1]->text, a->text) > 0) {
            order[j] = order[j-1]; j--;
        }
        order[j] = a; count++;
    }
    for (i = 0; i < count; i++) {
        const NeAddress6 *a = order[i];
        item[0] = 0;
        if (!append(item, sizeof(item), a->text)) return 0;
        if (a->scoped && (!append(item, sizeof(item), "%") ||
                         !append(item, sizeof(item), iface->name))) return 0;
        number(a->prefix, prefix);
        if (!append(item, sizeof(item), "/") || !append(item, sizeof(item), prefix)) return 0;
        if (!strcmp(item, previous)) continue;
        if ((ne_buffer[0] && !append(ne_buffer, sizeof(ne_buffer), " ")) ||
            !append(ne_buffer, sizeof(ne_buffer), item)) return 0;
        strcpy(previous, item);
    }
    return 1;
}

int ne_build(const NeSnapshot *s, NeTable *out)
{
    const NeInterface *order[NE_IF_MAX];
    const NeRoute *default6 = NULL;
    unsigned i, j;
    int ambiguous6 = 0, have4 = 0;
    char list[NE_IF_MAX * 16] = "", address[16], mask[16], gateway[80];
    out->count = out->used = 0;
    if (!s->state || s->interfaces > NE_IF_MAX || s->addresses6 > NE_ADDR6_MAX ||
        s->routes > NE_ROUTE_MAX || !add(out, "ANXD/STATE", s->state)) return 0;
    if (strcmp(s->state, "UP") != 0) return add(out, "ANXD/INTERFACES", "");
    for (i = 0; i < s->interfaces; i++) {
        const NeInterface *f = &s->iface[i];
        if (!memchr(f->name, 0, sizeof(f->name)) || !ne_name_valid(f->name)) return 0;
        j = i;
        while (j && strcmp(order[j-1]->name, f->name) > 0) { order[j] = order[j-1]; j--; }
        order[j] = f;
    }
    for (i = 0; i < s->interfaces; i++) {
        const NeInterface *f = order[i];
        int has6;
        for (j = 0; j < i; j++)
            if (same_name(order[j]->name, f->name)) return 0;
        if ((i && !append(list, sizeof(list), " ")) || !append(list, sizeof(list), f->name)) return 0;
        if (!addresses6(s, f)) return 0;
        has6 = ne_buffer[0] != 0;
        if (f->up && has6 && !field(out, f->name, "ADDR6", ne_buffer)) return 0;
        if (!field(out, f->name, "STATE", !f->up ? "DOWN" :
                   (f->address || has6) ? "UP" : "CONFIGURING")) return 0;
        if (f->up && f->address) {
            ipv4(f->address, address); ipv4(f->mask, mask);
            if (!field(out, f->name, "ADDRESS", address) ||
                !field(out, f->name, "NETMASK", mask)) return 0;
        }
    }
    if (!add(out, "ANXD/INTERFACES", list)) return 0;
    for (i = 0; i < s->routes; i++) {
        const NeRoute *r = &s->route[i];
        const NeInterface *f = interface_at(s, r->index);
        if (!f || !f->up || !r->gateway[0]) continue;
        if (!memchr(r->gateway, 0, sizeof(r->gateway))) return 0;
        if (r->ipv6) {
            if (!addresses6(s, f)) return 0;
            if (!ne_buffer[0]) continue;
            if (default6 && (default6->index != r->index ||
                            strcmp(default6->gateway, r->gateway))) ambiguous6 = 1;
            default6 = r;
        } else if (!have4 && f->address) {
            ipv4(f->address, address); ipv4(f->mask, mask);
            if (!add(out, "ANXD/DEFAULT/INTERFACE", f->name) ||
                !add(out, "ANXD/DEFAULT/ADDRESS", address) ||
                !add(out, "ANXD/DEFAULT/NETMASK", mask) ||
                !add(out, "ANXD/DEFAULT/GATEWAY", r->gateway)) return 0;
            have4 = 1;
        }
    }
    if (default6 && !ambiguous6) {
        const NeInterface *f = interface_at(s, default6->index);
        strcpy(gateway, default6->gateway);
        if (default6->scoped && (!append(gateway, sizeof(gateway), "%") ||
                                !append(gateway, sizeof(gateway), f->name))) return 0;
        if (!add(out, "ANXD/DEFAULT6/INTERFACE", f->name) ||
            !add(out, "ANXD/DEFAULT6/GATEWAY", gateway)) return 0;
    }
    return 1;
}

int ne_own(NeOwned *owned, const char *name)
{
    unsigned i;
    for (i = 0; i < owned->count; i++)
        if (!strcmp(owned->name[i], name)) return 1;
    if (owned->count == NE_KEY_MAX || strlen(name) > 31) return 0;
    strcpy(owned->name[owned->count++], name);
    return 1;
}

int ne_publish(NeOwned *owned, const NeTable *desired,
               NeRead read_value, NeWrite write_value, void *ctx)
{
    unsigned i = 0;
    int ok = 1;
    /* Remove old values first, also after restart or partial write failure. */
    while (i < owned->count) {
        if (ne_value(desired, owned->name[i])) { i++; continue; }
        if (!write_value(ctx, owned->name[i], NULL)) { ok = 0; i++; continue; }
        owned->count--;
        if (i != owned->count) strcpy(owned->name[i], owned->name[owned->count]);
    }
    for (i = 0; i < desired->count; i++) {
        const char *name = desired->entry[i].name;
        const char *value = desired->text + desired->entry[i].offset;
        if (!ne_own(owned, name)) { ok = 0; continue; }
        ne_buffer[0] = 0;
        if (read_value(ctx, name, ne_buffer, sizeof(ne_buffer)) == 1 &&
            !strcmp(value, ne_buffer)) continue;
        if (!write_value(ctx, name, value)) ok = 0;
    }
    return ok;
}

int ne_remove(const char *name, NeDelete delete_path, void *ctx)
{
    char path[40] = "ENV:", *p;
    long error;
    if (!append(path, sizeof(path), name)) return 0;
    error = delete_path(ctx, path);
    if (error && error != NE_ERR_DIR_NOT_FOUND && error != NE_ERR_OBJECT_NOT_FOUND)
        return 0;
    /* A directory still holding something refuses; that ends the walk. */
    while ((p = strrchr(path, '/'))) {
        *p = 0;
        error = delete_path(ctx, path);
        if (error && error != NE_ERR_DIR_NOT_FOUND && error != NE_ERR_OBJECT_NOT_FOUND)
            break;
    }
    return 1;
}

void ne_refresh(NeSnapshot *s, const NeStack *k)
{
    void *base;
    memset(s, 0, sizeof(*s));
    s->state = "DOWN";
    if (k->pending(k->ctx, 0) || !(base = k->open(k->ctx))) return;
    s->state = "UP";
    if (!k->collect(k->ctx, base, s)) s->state = "ERROR";
    k->close(k->ctx, base);
    if (k->pending(k->ctx, 1)) {
        memset(s, 0, sizeof(*s));
        s->state = "DOWN";
    }
}

const char *ne_version(const char *verstag)
{
    static const char prefix[] = "$VER: ";
    return strncmp(verstag, prefix, sizeof(prefix) - 1) ? verstag
                                                        : verstag + sizeof(prefix) - 1;
}
