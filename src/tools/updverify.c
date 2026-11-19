/*
 * updverify, the host build of UpdateAmiNetXDuo's manifest check, for the
 * release workflow: a signed update.manifest is published only once this
 * accepts it.
 *
 *   updverify <update.manifest> <update.manifest.sig> [release|prerelease]
 *
 * The signature is checked by upd_verify() against upd_keys[], the same
 * updsig.c and updkeys.c the Amiga tool links: built without UPD_TEST_KEY it
 * holds the release keys and nothing else.  The manifest is then parsed by
 * upd_manifest_parse() once per flavour, as a machine of that flavour on
 * 0.0.0 would parse it, so a manifest the tool would refuse is refused here.
 * The channel, when given, must be the manifest's.
 *
 * Output is key=value plus an exit code: 0 accepted, 1 refused, 2 cannot run.
 *
 * SPDX-License-Identifier: MIT
 */

#include "updkeys.h"
#include "updsig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UV_MAX_BYTES    (1024UL * 1024UL)
#define UV_MAX_FILES    1024

static UpdFile uv_files[UV_MAX_FILES];

static char *uv_read(const char *path, unsigned long *len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    size_t n;

    if (f == NULL)
        return NULL;
    buf = malloc(UV_MAX_BYTES + 1);
    if (buf == NULL)
    {
        fclose(f);
        return NULL;
    }
    n = fread(buf, 1, UV_MAX_BYTES + 1, f);
    fclose(f);
    if (n > UV_MAX_BYTES)
    {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    *len = (unsigned long)n;
    return buf;
}

int main(int argc, char **argv)
{
    unsigned long mlen = 0, slen = 0;
    char         *m, *s;
    UpdResult     r;
    UpdWant       want;
    UpdManifest   man;
    unsigned int  f, k;
    int           channel = -1;
    char          ver[UPD_VER_MAX];

    if (argc != 3 && argc != 4)
    {
        printf("update_verify=FAIL usage=manifest_sig_[channel]\n");
        return 2;
    }
    if (argc == 4)
    {
        if (strcmp(argv[3], "release") == 0)
            channel = UPD_CHANNEL_RELEASE;
        else if (strcmp(argv[3], "prerelease") == 0)
            channel = UPD_CHANNEL_PRERELEASE;
        else
        {
            printf("update_verify=FAIL channel=%s\n", argv[3]);
            return 2;
        }
    }

    m = uv_read(argv[1], &mlen);
    s = uv_read(argv[2], &slen);
    if (m == NULL || s == NULL)
    {
        printf("update_verify=FAIL unreadable=%s\n", m == NULL ? argv[1] : argv[2]);
        return 2;
    }

    if (upd_key_count == 0)
    {
        printf("update_verify=FAIL keys=0\n");
        return 1;
    }

    r = upd_verify((const unsigned char *)m, mlen, s, slen,
                   upd_keys, upd_key_count);
    if (r != UPD_OK)
    {
        printf("update_verify=FAIL step=signature result=%s\n",
               upd_result_text(r));
        return 1;
    }

    /* Every flavour, from 0.0.0, on the manifest's own channel unless one
       was asked for: the first parse finds it out. */
    memset(&want, 0, sizeof(want));
    (void)upd_ver_parse("0.0.0", 5, &want.installed);
    for (f = 0; f < UPD_FLAVOUR_COUNT; f++)
    {
        want.flavour = (unsigned char)f;
        want.channel = (unsigned char)(channel >= 0 ? channel : UPD_CHANNEL_RELEASE);
        r = upd_manifest_parse(m, mlen, &want, &man, uv_files, UV_MAX_FILES);
        if (r == UPD_E_CHANNEL && channel < 0)
        {
            want.channel = UPD_CHANNEL_PRERELEASE;
            r = upd_manifest_parse(m, mlen, &want, &man, uv_files,
                                   UV_MAX_FILES);
        }
        if (r != UPD_OK)
        {
            printf("update_verify=FAIL step=manifest flavour=%s result=%s\n",
                   upd_flavour_name(f), upd_result_text(r));
            return 1;
        }
        if (man.nfiles == 0)
        {
            printf("update_verify=FAIL step=manifest flavour=%s files=0\n",
                   upd_flavour_name(f));
            return 1;
        }
    }

    upd_ver_format(&man.version, ver);
    printf("update_verify=PASS version=%s channel=%s keys=", ver,
           upd_channel_name(man.channel));
    for (k = 0; k < upd_key_count; k++)
        printf("%s%s", k ? "," : "", upd_keys[k].id);
    printf(" flavours=%u\n", (unsigned)UPD_FLAVOUR_COUNT);

    free(m);
    free(s);
    return 0;
}
