/*
 * tls.library, checked host-name setup shared by TLSOpenA() and its host test.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tls_internal.h"


LONG tls_hostname_set(TLSConnection *conn, CONST_STRPTR hostname, ULONG length)
{
ULONG i;


    if ((conn == NULL) || ((hostname == NULL) && (length != 0UL)))
    {
        return(TLS_ERR_INTERNAL);
    }

    if (length > (ULONG) NX_SECURE_X509_DNS_NAME_MAX)
    {
        return(TLS_ERR_BADHOSTNAME);
    }

    for (i = 0UL; i < length; i++)
    {
        conn -> tc_HostName[i] =  (UCHAR) hostname[i];
        conn -> tc_Sni.nx_secure_x509_dns_name[i] =  (UCHAR) hostname[i];
    }

    conn -> tc_HostName[length] =  0;
    conn -> tc_HostNameLength =  (USHORT) length;
    conn -> tc_Sni.nx_secure_x509_dns_name_length =  (USHORT) length;
    conn -> tc_Sni.nx_secure_x509_dns_name_next =  NX_NULL;

    return(TLS_OK);
}
