/*
 * SSL3 Protocol
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is the Netscape security libraries.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1994-2000
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Dr Stephen Henson <stephen.henson@gemplus.com>
 *   Dr Vipul Gupta <vipul.gupta@sun.com> and
 *   Douglas Stebila <douglas@stebila.ca>, Sun Microsystems Laboratories
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */
/* $Id: ssl3con.c,v 1.76.2.21 2007/08/28 03:39:12 nelson%bolyard.com Exp $ */

#include "nssrenam.h"
#include "cert.h"
#include "ssl.h"
#include "cryptohi.h"	/* for DSAU_ stuff */
#include "keyhi.h"
#include "secder.h"
#include "secitem.h"

#include "sslimpl.h"
#include "sslproto.h"
#include "sslerr.h"
#include "prtime.h"
#include "prinrval.h"
#include "prerror.h"
#include "pratom.h"
#include "prthread.h"

#include "pk11func.h"
#include "secmod.h"
#include "nsslocks.h"
#include "ec.h"
#include "blapi.h"

#include <stdio.h>

#ifndef PK11_SETATTRS
#define PK11_SETATTRS(x,id,v,l) (x)->type = (id); \
		(x)->pValue=(v); (x)->ulValueLen = (l);
#endif

static void      ssl3_CleanupPeerCerts(sslSocket *ss);
static PK11SymKey *ssl3_GenerateRSAPMS(sslSocket *ss, ssl3CipherSpec *spec,
                                       PK11SlotInfo * serverKeySlot);
static SECStatus ssl3_DeriveMasterSecret(sslSocket *ss, PK11SymKey *pms);
static SECStatus ssl3_DeriveConnectionKeysPKCS11(sslSocket *ss);
static SECStatus ssl3_HandshakeFailure(      sslSocket *ss);
static SECStatus ssl3_InitState(             sslSocket *ss);
static sslSessionID *ssl3_NewSessionID(      sslSocket *ss, PRBool is_server);
static SECStatus ssl3_SendCertificate(       sslSocket *ss);
static SECStatus ssl3_SendEmptyCertificate(  sslSocket *ss);
static SECStatus ssl3_SendCertificateRequest(sslSocket *ss);
static SECStatus ssl3_SendFinished(          sslSocket *ss, PRInt32 flags);
static SECStatus ssl3_SendServerHello(       sslSocket *ss);
static SECStatus ssl3_SendServerHelloDone(   sslSocket *ss);
static SECStatus ssl3_SendServerKeyExchange( sslSocket *ss);
static SECStatus ssl3_NewHandshakeHashes(    sslSocket *ss);
static SECStatus ssl3_UpdateHandshakeHashes( sslSocket *ss, unsigned char *b, 
                                             unsigned int l);

static SECStatus Null_Cipher(void *ctx, unsigned char *output, int *outputLen,
			     int maxOutputLen, const unsigned char *input,
			     int inputLen);

#define MAX_SEND_BUF_LENGTH 32000 /* watch for 16-bit integer overflow */
#define MIN_SEND_BUF_LENGTH  4000

#define MAX_CIPHER_SUITES 20

/* This list of SSL3 cipher suites is sorted in descending order of
 * precedence (desirability).  It only includes cipher suites we implement.
 * This table is modified by SSL3_SetPolicy(). The ordering of cipher suites
 * in this table must match the ordering in SSL_ImplementedCiphers (sslenum.c)
 */
static ssl3CipherSuiteCfg cipherSuites[ssl_V3_SUITES_IMPLEMENTED] = {
   /*      cipher_suite                         policy      enabled is_present*/
#ifdef NSS_ENABLE_ECC
 { TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA,   SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
 { TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA,     SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
#endif /* NSS_ENABLE_ECC */
 { TLS_DHE_RSA_WITH_AES_256_CBC_SHA, 	   SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
 { TLS_DHE_DSS_WITH_AES_256_CBC_SHA, 	   SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
#ifdef NSS_ENABLE_ECC
 { TLS_ECDH_RSA_WITH_AES_256_CBC_SHA,      SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
 { TLS_ECDH_ECDSA_WITH_AES_256_CBC_SHA,    SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
#endif /* NSS_ENABLE_ECC */
 { TLS_RSA_WITH_AES_256_CBC_SHA,     	   SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},

#ifdef NSS_ENABLE_ECC
 { TLS_ECDHE_ECDSA_WITH_RC4_128_SHA,       SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
 { TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA,   SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
 { TLS_ECDHE_RSA_WITH_RC4_128_SHA,         SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
 { TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA,     SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
#endif /* NSS_ENABLE_ECC */
 { TLS_DHE_DSS_WITH_RC4_128_SHA,           SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
 { TLS_DHE_RSA_WITH_AES_128_CBC_SHA,       SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
 { TLS_DHE_DSS_WITH_AES_128_CBC_SHA, 	   SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
#ifdef NSS_ENABLE_ECC
 { TLS_ECDH_RSA_WITH_RC4_128_SHA,          SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
 { TLS_ECDH_RSA_WITH_AES_128_CBC_SHA,      SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
 { TLS_ECDH_ECDSA_WITH_RC4_128_SHA,        SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
 { TLS_ECDH_ECDSA_WITH_AES_128_CBC_SHA,    SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
#endif /* NSS_ENABLE_ECC */
 { SSL_RSA_WITH_RC4_128_MD5,               SSL_NOT_ALLOWED, PR_TRUE, PR_FALSE},
 { SSL_RSA_WITH_RC4_128_SHA,               SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
 { TLS_RSA_WITH_AES_128_CBC_SHA,     	   SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},

#ifdef NSS_ENABLE_ECC
 { TLS_ECDHE_ECDSA_WITH_3DES_EDE_CBC_SHA,  SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
 { TLS_ECDHE_RSA_WITH_3DES_EDE_CBC_SHA,    SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
#endif /* NSS_ENABLE_ECC */
 { SSL_DHE_RSA_WITH_3DES_EDE_CBC_SHA,      SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
 { SSL_DHE_DSS_WITH_3DES_EDE_CBC_SHA,      SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
#ifdef NSS_ENABLE_ECC
 { TLS_ECDH_RSA_WITH_3DES_EDE_CBC_SHA,     SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
 { TLS_ECDH_ECDSA_WITH_3DES_EDE_CBC_SHA,   SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
#endif /* NSS_ENABLE_ECC */
 { SSL_RSA_FIPS_WITH_3DES_EDE_CBC_SHA,     SSL_NOT_ALLOWED, PR_TRUE, PR_FALSE},
 { SSL_RSA_WITH_3DES_EDE_CBC_SHA,          SSL_NOT_ALLOWED, PR_TRUE, PR_FALSE},


 { SSL_DHE_RSA_WITH_DES_CBC_SHA,           SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
 { SSL_DHE_DSS_WITH_DES_CBC_SHA,           SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
 { SSL_RSA_FIPS_WITH_DES_CBC_SHA,          SSL_NOT_ALLOWED, PR_TRUE, PR_FALSE},
 { SSL_RSA_WITH_DES_CBC_SHA,               SSL_NOT_ALLOWED, PR_TRUE, PR_FALSE},
 { TLS_RSA_EXPORT1024_WITH_RC4_56_SHA,     SSL_NOT_ALLOWED, PR_TRUE, PR_FALSE},
 { TLS_RSA_EXPORT1024_WITH_DES_CBC_SHA,    SSL_NOT_ALLOWED, PR_TRUE, PR_FALSE},

 { SSL_RSA_EXPORT_WITH_RC4_40_MD5,         SSL_NOT_ALLOWED, PR_TRUE, PR_FALSE},
 { SSL_RSA_EXPORT_WITH_RC2_CBC_40_MD5,     SSL_NOT_ALLOWED, PR_TRUE, PR_FALSE},

#ifdef NSS_ENABLE_ECC
 { TLS_ECDHE_ECDSA_WITH_NULL_SHA,          SSL_NOT_ALLOWED, PR_FALSE, PR_FALSE},
 { TLS_ECDHE_RSA_WITH_NULL_SHA,            SSL_NOT_ALLOWED, PR_FALSE, PR_FALSE},
 { TLS_ECDH_RSA_WITH_NULL_SHA,             SSL_NOT_ALLOWED, PR_FALSE, PR_FALSE},
 { TLS_ECDH_ECDSA_WITH_NULL_SHA,           SSL_NOT_ALLOWED, PR_FALSE, PR_FALSE},
#endif /* NSS_ENABLE_ECC */
 { SSL_RSA_WITH_NULL_SHA,                  SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},
 { SSL_RSA_WITH_NULL_MD5,                  SSL_NOT_ALLOWED, PR_FALSE,PR_FALSE},

};

static const /*SSL3CompressionMethod*/ uint8 compressions [] = {
    compression_null
};

static const int compressionMethodsCount =
		sizeof(compressions) / sizeof(compressions[0]);

static const /*SSL3ClientCertificateType */ uint8 certificate_types [] = {
    ct_RSA_sign,
    ct_DSS_sign,
#ifdef NSS_ENABLE_ECC
    ct_ECDSA_sign,
#endif /* NSS_ENABLE_ECC */
};


/*
 * make sure there is room in the write buffer for padding and
 * other compression and cryptographic expansions
 */
#define SSL3_BUFFER_FUDGE     100

#define EXPORT_RSA_KEY_LENGTH 64	/* bytes */


/* This is a hack to make sure we don't do double handshakes for US policy */
PRBool ssl3_global_policy_some_restricted = PR_FALSE;

/* This global item is used only in servers.  It is is initialized by
** SSL_ConfigSecureServer(), and is used in ssl3_SendCertificateRequest().
*/
CERTDistNames *ssl3_server_ca_list = NULL;
static SSL3Statistics ssl3stats;

/* indexed by SSL3BulkCipher */
static const ssl3BulkCipherDef bulk_cipher_defs[] = {
    /* cipher          calg        keySz secretSz  type  ivSz BlkSz keygen */
    {cipher_null,      calg_null,      0,  0, type_stream,  0, 0, kg_null},
    {cipher_rc4,       calg_rc4,      16, 16, type_stream,  0, 0, kg_strong},
    {cipher_rc4_40,    calg_rc4,      16,  5, type_stream,  0, 0, kg_export},
    {cipher_rc4_56,    calg_rc4,      16,  7, type_stream,  0, 0, kg_export},
    {cipher_rc2,       calg_rc2,      16, 16, type_block,   8, 8, kg_strong},
    {cipher_rc2_40,    calg_rc2,      16,  5, type_block,   8, 8, kg_export},
    {cipher_des,       calg_des,       8,  8, type_block,   8, 8, kg_strong},
    {cipher_3des,      calg_3des,     24, 24, type_block,   8, 8, kg_strong},
    {cipher_des40,     calg_des,       8,  5, type_block,   8, 8, kg_export},
    {cipher_idea,      calg_idea,     16, 16, type_block,   8, 8, kg_strong},
    {cipher_aes_128,   calg_aes,      16, 16, type_block,  16,16, kg_strong},
    {cipher_aes_256,   calg_aes,      32, 32, type_block,  16,16, kg_strong},
    {cipher_missing,   calg_null,      0,  0, type_stream,  0, 0, kg_null},
};

static const ssl3KEADef kea_defs[] = 
{ /* indexed by SSL3KeyExchangeAlgorithm */
    /* kea              exchKeyType signKeyType is_limited limit  tls_keygen */
    {kea_null,           kt_null,     sign_null, PR_FALSE,   0, PR_FALSE},
    {kea_rsa,            kt_rsa,      sign_rsa,  PR_FALSE,   0, PR_FALSE},
    {kea_rsa_export,     kt_rsa,      sign_rsa,  PR_TRUE,  512, PR_FALSE},
    {kea_rsa_export_1024,kt_rsa,      sign_rsa,  PR_TRUE, 1024, PR_FALSE},
    {kea_dh_dss,         kt_dh,       sign_dsa,  PR_FALSE,   0, PR_FALSE},
    {kea_dh_dss_export,  kt_dh,       sign_dsa,  PR_TRUE,  512, PR_FALSE},
    {kea_dh_rsa,         kt_dh,       sign_rsa,  PR_FALSE,   0, PR_FALSE},
    {kea_dh_rsa_export,  kt_dh,       sign_rsa,  PR_TRUE,  512, PR_FALSE},
    {kea_dhe_dss,        kt_dh,       sign_dsa,  PR_FALSE,   0, PR_FALSE},
    {kea_dhe_dss_export, kt_dh,       sign_dsa,  PR_TRUE,  512, PR_FALSE},
    {kea_dhe_rsa,        kt_dh,       sign_rsa,  PR_FALSE,   0, PR_FALSE},
    {kea_dhe_rsa_export, kt_dh,       sign_rsa,  PR_TRUE,  512, PR_FALSE},
    {kea_dh_anon,        kt_dh,       sign_null, PR_FALSE,   0, PR_FALSE},
    {kea_dh_anon_export, kt_dh,       sign_null, PR_TRUE,  512, PR_FALSE},
    {kea_rsa_fips,       kt_rsa,      sign_rsa,  PR_FALSE,   0, PR_TRUE },
#ifdef NSS_ENABLE_ECC
    {kea_ecdh_ecdsa,     kt_ecdh,     sign_ecdsa,  PR_FALSE, 0, PR_FALSE},
    {kea_ecdhe_ecdsa,    kt_ecdh,     sign_ecdsa,  PR_FALSE,   0, PR_FALSE},
    {kea_ecdh_rsa,       kt_ecdh,     sign_rsa,  PR_FALSE,   0, PR_FALSE},
    {kea_ecdhe_rsa,      kt_ecdh,     sign_rsa,  PR_FALSE,   0, PR_FALSE},
    {kea_ecdh_anon,      kt_ecdh,     sign_null,  PR_FALSE,   0, PR_FALSE},
#endif /* NSS_ENABLE_ECC */
};

/* must use ssl_LookupCipherSuiteDef to access */
static const ssl3CipherSuiteDef cipher_suite_defs[] = 
{
/*  cipher_suite                    bulk_cipher_alg mac_alg key_exchange_alg */

    {SSL_NULL_WITH_NULL_NULL,       cipher_null,   mac_null, kea_null},
    {SSL_RSA_WITH_NULL_MD5,         cipher_null,   mac_md5, kea_rsa},
    {SSL_RSA_WITH_NULL_SHA,         cipher_null,   mac_sha, kea_rsa},
    {SSL_RSA_EXPORT_WITH_RC4_40_MD5,cipher_rc4_40, mac_md5, kea_rsa_export},
    {SSL_RSA_WITH_RC4_128_MD5,      cipher_rc4,    mac_md5, kea_rsa},
    {SSL_RSA_WITH_RC4_128_SHA,      cipher_rc4,    mac_sha, kea_rsa},
    {SSL_RSA_EXPORT_WITH_RC2_CBC_40_MD5,
                                    cipher_rc2_40, mac_md5, kea_rsa_export},
#if 0 /* not implemented */
    {SSL_RSA_WITH_IDEA_CBC_SHA,     cipher_idea,   mac_sha, kea_rsa},
    {SSL_RSA_EXPORT_WITH_DES40_CBC_SHA,
                                    cipher_des40,  mac_sha, kea_rsa_export},
#endif
    {SSL_RSA_WITH_DES_CBC_SHA,      cipher_des,    mac_sha, kea_rsa},
    {SSL_RSA_WITH_3DES_EDE_CBC_SHA, cipher_3des,   mac_sha, kea_rsa},
    {SSL_DHE_DSS_WITH_DES_CBC_SHA,  cipher_des,    mac_sha, kea_dhe_dss},
    {SSL_DHE_DSS_WITH_3DES_EDE_CBC_SHA,
                                    cipher_3des,   mac_sha, kea_dhe_dss},
    {TLS_DHE_DSS_WITH_RC4_128_SHA,  cipher_rc4,    mac_sha, kea_dhe_dss},
#if 0 /* not implemented */
    {SSL_DH_DSS_EXPORT_WITH_DES40_CBC_SHA,
                                    cipher_des40,  mac_sha, kea_dh_dss_export},
    {SSL_DH_DSS_DES_CBC_SHA,        cipher_des,    mac_sha, kea_dh_dss},
    {SSL_DH_DSS_3DES_CBC_SHA,       cipher_3des,   mac_sha, kea_dh_dss},
    {SSL_DH_RSA_EXPORT_WITH_DES40_CBC_SHA,
                                    cipher_des40,  mac_sha, kea_dh_rsa_export},
    {SSL_DH_RSA_DES_CBC_SHA,        cipher_des,    mac_sha, kea_dh_rsa},
    {SSL_DH_RSA_3DES_CBC_SHA,       cipher_3des,   mac_sha, kea_dh_rsa},
    {SSL_DHE_DSS_EXPORT_WITH_DES40_CBC_SHA,
                                    cipher_des40,  mac_sha, kea_dh_dss_export},
    {SSL_DHE_RSA_EXPORT_WITH_DES40_CBC_SHA,
                                    cipher_des40,  mac_sha, kea_dh_rsa_export},
#endif
    {SSL_DHE_RSA_WITH_DES_CBC_SHA,  cipher_des,    mac_sha, kea_dhe_rsa},
    {SSL_DHE_RSA_WITH_3DES_EDE_CBC_SHA,
                                    cipher_3des,   mac_sha, kea_dhe_rsa},
#if 0
    {SSL_DH_ANON_EXPORT_RC4_40_MD5, cipher_rc4_40, mac_md5, kea_dh_anon_export},
    {SSL_DH_ANON_EXPORT_RC4_40_MD5, cipher_rc4,    mac_md5, kea_dh_anon_export},
    {SSL_DH_ANON_EXPORT_WITH_DES40_CBC_SHA,
                                    cipher_des40,  mac_sha, kea_dh_anon_export},
    {SSL_DH_ANON_DES_CBC_SHA,       cipher_des,    mac_sha, kea_dh_anon},
    {SSL_DH_ANON_3DES_CBC_SHA,      cipher_3des,   mac_sha, kea_dh_anon},
#endif


/* New TLS cipher suites */
    {TLS_RSA_WITH_AES_128_CBC_SHA,     	cipher_aes_128, mac_sha, kea_rsa},
    {TLS_DHE_DSS_WITH_AES_128_CBC_SHA, 	cipher_aes_128, mac_sha, kea_dhe_dss},
    {TLS_DHE_RSA_WITH_AES_128_CBC_SHA, 	cipher_aes_128, mac_sha, kea_dhe_rsa},
    {TLS_RSA_WITH_AES_256_CBC_SHA,     	cipher_aes_256, mac_sha, kea_rsa},
    {TLS_DHE_DSS_WITH_AES_256_CBC_SHA, 	cipher_aes_256, mac_sha, kea_dhe_dss},
    {TLS_DHE_RSA_WITH_AES_256_CBC_SHA, 	cipher_aes_256, mac_sha, kea_dhe_rsa},
#if 0
    {TLS_DH_DSS_WITH_AES_128_CBC_SHA,  	cipher_aes_128, mac_sha, kea_dh_dss},
    {TLS_DH_RSA_WITH_AES_128_CBC_SHA,  	cipher_aes_128, mac_sha, kea_dh_rsa},
    {TLS_DH_ANON_WITH_AES_128_CBC_SHA, 	cipher_aes_128, mac_sha, kea_dh_anon},
    {TLS_DH_DSS_WITH_AES_256_CBC_SHA,  	cipher_aes_256, mac_sha, kea_dh_dss},
    {TLS_DH_RSA_WITH_AES_256_CBC_SHA,  	cipher_aes_256, mac_sha, kea_dh_rsa},
    {TLS_DH_ANON_WITH_AES_256_CBC_SHA, 	cipher_aes_256, mac_sha, kea_dh_anon},
#endif

    {TLS_RSA_EXPORT1024_WITH_DES_CBC_SHA,
                                    cipher_des,    mac_sha,kea_rsa_export_1024},
    {TLS_RSA_EXPORT1024_WITH_RC4_56_SHA,
                                    cipher_rc4_56, mac_sha,kea_rsa_export_1024},

    {SSL_RSA_FIPS_WITH_3DES_EDE_CBC_SHA, cipher_3des, mac_sha, kea_rsa_fips},
    {SSL_RSA_FIPS_WITH_DES_CBC_SHA, cipher_des,    mac_sha, kea_rsa_fips},

#ifdef NSS_ENABLE_ECC
    {TLS_ECDH_ECDSA_WITH_NULL_SHA,        cipher_null, mac_sha, kea_ecdh_ecdsa},
    {TLS_ECDH_ECDSA_WITH_RC4_128_SHA,      cipher_rc4, mac_sha, kea_ecdh_ecdsa},
    {TLS_ECDH_ECDSA_WITH_3DES_EDE_CBC_SHA, cipher_3des, mac_sha, kea_ecdh_ecdsa},
    {TLS_ECDH_ECDSA_WITH_AES_128_CBC_SHA, cipher_aes_128, mac_sha, kea_ecdh_ecdsa},
    {TLS_ECDH_ECDSA_WITH_AES_256_CBC_SHA, cipher_aes_256, mac_sha, kea_ecdh_ecdsa},

    {TLS_ECDHE_ECDSA_WITH_NULL_SHA,        cipher_null, mac_sha, kea_ecdhe_ecdsa},
    {TLS_ECDHE_ECDSA_WITH_RC4_128_SHA,      cipher_rc4, mac_sha, kea_ecdhe_ecdsa},
    {TLS_ECDHE_ECDSA_WITH_3DES_EDE_CBC_SHA, cipher_3des, mac_sha, kea_ecdhe_ecdsa},
    {TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA, cipher_aes_128, mac_sha, kea_ecdhe_ecdsa},
    {TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA, cipher_aes_256, mac_sha, kea_ecdhe_ecdsa},

    {TLS_ECDH_RSA_WITH_NULL_SHA,         cipher_null,    mac_sha, kea_ecdh_rsa},
    {TLS_ECDH_RSA_WITH_RC4_128_SHA,      cipher_rc4,     mac_sha, kea_ecdh_rsa},
    {TLS_ECDH_RSA_WITH_3DES_EDE_CBC_SHA, cipher_3des,    mac_sha, kea_ecdh_rsa},
    {TLS_ECDH_RSA_WITH_AES_128_CBC_SHA,  cipher_aes_128, mac_sha, kea_ecdh_rsa},
    {TLS_ECDH_RSA_WITH_AES_256_CBC_SHA,  cipher_aes_256, mac_sha, kea_ecdh_rsa},

    {TLS_ECDHE_RSA_WITH_NULL_SHA,         cipher_null,    mac_sha, kea_ecdhe_rsa},
    {TLS_ECDHE_RSA_WITH_RC4_128_SHA,      cipher_rc4,     mac_sha, kea_ecdhe_rsa},
    {TLS_ECDHE_RSA_WITH_3DES_EDE_CBC_SHA, cipher_3des,    mac_sha, kea_ecdhe_rsa},
    {TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA,  cipher_aes_128, mac_sha, kea_ecdhe_rsa},
    {TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA,  cipher_aes_256, mac_sha, kea_ecdhe_rsa},

#if 0
    {TLS_ECDH_anon_WITH_NULL_SHA,         cipher_null,    mac_sha, kea_ecdh_anon},
    {TLS_ECDH_anon_WITH_RC4_128_SHA,      cipher_rc4,     mac_sha, kea_ecdh_anon},
    {TLS_ECDH_anon_WITH_3DES_EDE_CBC_SHA, cipher_3des,    mac_sha, kea_ecdh_anon},
    {TLS_ECDH_anon_WITH_AES_128_CBC_SHA,  cipher_aes_128, mac_sha, kea_ecdh_anon},
    {TLS_ECDH_anon_WITH_AES_256_CBC_SHA,  cipher_aes_256, mac_sha, kea_ecdh_anon},
#endif
#endif /* NSS_ENABLE_ECC */
};

static const CK_MECHANISM_TYPE kea_alg_defs[] = {
    0x80000000L,
    CKM_RSA_PKCS,
    CKM_DH_PKCS_DERIVE,
    CKM_KEA_KEY_DERIVE,
    CKM_ECDH1_DERIVE
};

typedef struct SSLCipher2MechStr {
    SSLCipherAlgorithm  calg;
    CK_MECHANISM_TYPE   cmech;
} SSLCipher2Mech;

/* indexed by type SSLCipherAlgorithm */
static const SSLCipher2Mech alg2Mech[] = {
    /* calg,          cmech  */
    { calg_null     , (CK_MECHANISM_TYPE)0x80000000L	},
    { calg_rc4      , CKM_RC4				},
    { calg_rc2      , CKM_RC2_CBC			},
    { calg_des      , CKM_DES_CBC			},
    { calg_3des     , CKM_DES3_CBC			},
    { calg_idea     , CKM_IDEA_CBC			},
    { calg_fortezza , CKM_SKIPJACK_CBC64                },
    { calg_aes      , CKM_AES_CBC			},
/*  { calg_init     , (CK_MECHANISM_TYPE)0x7fffffffL    }  */
};

#define mmech_null     (CK_MECHANISM_TYPE)0x80000000L
#define mmech_md5      CKM_SSL3_MD5_MAC
#define mmech_sha      CKM_SSL3_SHA1_MAC
#define mmech_md5_hmac CKM_MD5_HMAC
#define mmech_sha_hmac CKM_SHA_1_HMAC

static const ssl3MACDef mac_defs[] = { /* indexed by SSL3MACAlgorithm */
    /* mac      mmech       pad_size  mac_size                       */
    { mac_null, mmech_null,       0,  0          },
    { mac_md5,  mmech_md5,       48,  MD5_LENGTH },
    { mac_sha,  mmech_sha,       40,  SHA1_LENGTH},
    {hmac_md5,  mmech_md5_hmac,  48,  MD5_LENGTH },
    {hmac_sha,  mmech_sha_hmac,  40,  SHA1_LENGTH},
};

/* indexed by SSL3BulkCipher */
const char * const ssl3_cipherName[] = {
    "NULL",
    "RC4",
    "RC4-40",
    "RC4-56",
    "RC2-CBC",
    "RC2-CBC-40",
    "DES-CBC",
    "3DES-EDE-CBC",
    "DES-CBC-40",
    "IDEA-CBC",
    "AES-128",
    "AES-256",
    "missing"
};

#ifdef NSS_ENABLE_ECC
/* The ECCWrappedKeyInfo structure defines how various pieces of 
 * information are laid out within wrappedSymmetricWrappingkey 
 * for ECDH key exchange. Since wrappedSymmetricWrappingkey is 
 * a 512-byte buffer (see sslimpl.h), the variable length field 
 * in ECCWrappedKeyInfo can be at most (512 - 8) = 504 bytes.
 *
 * XXX For now, NSS only supports named elliptic curves of size 571 bits 
 * or smaller. The public value will fit within 145 bytes and EC params
 * will fit within 12 bytes. We'll need to revisit this when NSS
 * supports arbitrary curves.
 */
#define MAX_EC_WRAPPED_KEY_BUFLEN  504

typedef struct ECCWrappedKeyInfoStr {
    PRUint16 size;            /* EC public key size in bits */
    PRUint16 encodedParamLen; /* length (in bytes) of DER encoded EC params */
    PRUint16 pubValueLen;     /* length (in bytes) of EC public value */
    PRUint16 wrappedKeyLen;   /* length (in bytes) of the wrapped key */
    PRUint8 var[MAX_EC_WRAPPED_KEY_BUFLEN]; /* this buffer contains the */
    /* EC public-key params, the EC public value and the wrapped key  */
} ECCWrappedKeyInfo;
#endif /* NSS_ENABLE_ECC */

#if defined(TRACE)

static char *
ssl3_DecodeHandshakeType(int msgType)
{
    char * rv;
    static char line[40];

    switch(msgType) {
    case hello_request:	        rv = "hello_request (0)";               break;
    case client_hello:	        rv = "client_hello  (1)";               break;
    case server_hello:	        rv = "server_hello  (2)";               break;
    case certificate:	        rv = "certificate  (11)";               break;
    case server_key_exchange:	rv = "server_key_exchange (12)";        break;
    case certificate_request:	rv = "certificate_request (13)";        break;
    case server_hello_done:	rv = "server_hello_done   (14)";        break;
    case certificate_verify:	rv = "certificate_verify  (15)";        break;
    case client_key_exchange:	rv = "client_key_exchange (16)";        break;
    case finished:	        rv = "finished     (20)";               break;
    default:
        sprintf(line, "*UNKNOWN* handshake type! (%d)", msgType);
	rv = line;
    }
    return rv;
}

static char *
ssl3_DecodeContentType(int msgType)
{
    char * rv;
    static char line[40];

    switch(msgType) {
    case content_change_cipher_spec:
                                rv = "change_cipher_spec (20)";         break;
    case content_alert:	        rv = "alert      (21)";                 break;
    case content_handshake:	rv = "handshake  (22)";                 break;
    case content_application_data:
                                rv = "application_data (23)";           break;
    default:
        sprintf(line, "*UNKNOWN* record type! (%d)", msgType);
	rv = line;
    }
    return rv;
}

#endif

SSL3Statistics * 
SSL_GetStatistics(void)
{
    return &ssl3stats;
}

typedef struct tooLongStr {
#if defined(IS_LITTLE_ENDIAN)
    PRInt32 low;
    PRInt32 high;
#else
    PRInt32 high;
    PRInt32 low;
#endif
} tooLong;

static void SSL_AtomicIncrementLong(long * x)
{
    if ((sizeof *x) == sizeof(PRInt32)) {
        PR_AtomicIncrement((PRInt32 *)x);
    } else {
    	tooLong * tl = (tooLong *)x;
	PR_AtomicIncrement(&tl->low) || PR_AtomicIncrement(&tl->high);
    }
}

/* return pointer to ssl3CipherSuiteDef for suite, or NULL */
/* XXX This does a linear search.  A binary search would be better. */
static const ssl3CipherSuiteDef *
ssl_LookupCipherSuiteDef(ssl3CipherSuite suite)
{
    int cipher_suite_def_len =
	sizeof(cipher_suite_defs) / sizeof(cipher_suite_defs[0]);
    int i;

    for (i = 0; i < cipher_suite_def_len; i++) {
	if (cipher_suite_defs[i].cipher_suite == suite)
	    return &cipher_suite_defs[i];
    }
    PORT_Assert(PR_FALSE);  /* We should never get here. */
    PORT_SetError(SSL_ERROR_UNKNOWN_CIPHER_SUITE);
    return NULL;
}

/* Find the cipher configuration struct associate with suite */
/* XXX This does a linear search.  A binary search would be better. */
static ssl3CipherSuiteCfg *
ssl_LookupCipherSuiteCfg(ssl3CipherSuite suite, ssl3CipherSuiteCfg *suites)
{
    int i;

    for (i = 0; i < ssl_V3_SUITES_IMPLEMENTED; i++) {
	if (suites[i].cipher_suite == suite)
	    return &suites[i];
    }
    /* return NULL and let the caller handle it.  */
    PORT_SetError(SSL_ERROR_UNKNOWN_CIPHER_SUITE);
    return NULL;
}


/* Initialize the suite->isPresent value for config_match
 * Returns count of enabled ciphers supported by extant tokens,
 * regardless of policy or user preference.
 * If this returns zero, the user cannot do SSL v3.
 */
int
ssl3_config_match_init(sslSocket *ss)
{
    ssl3CipherSuiteCfg *      suite;
    const ssl3CipherSuiteDef *cipher_def;
    SSLCipherAlgorithm        cipher_alg;
    CK_MECHANISM_TYPE         cipher_mech;
    SSL3KEAType               exchKeyType;
    int                       i;
    int                       numPresent		= 0;
    int                       numEnabled		= 0;
    PRBool                    isServer;
    sslServerCerts           *svrAuth;

    PORT_Assert(ss);
    if (!ss) {
    	PORT_SetError(SEC_ERROR_INVALID_ARGS);
	return 0;
    }
    if (!ss->opt.enableSSL3 && !ss->opt.enableTLS) {
    	return 0;
    }
    isServer = (PRBool)(ss->sec.isServer != 0);

    for (i = 0; i < ssl_V3_SUITES_IMPLEMENTED; i++) {
	suite = &ss->cipherSuites[i];
	if (suite->enabled) {
	    ++numEnabled;
	    /* We need the cipher defs to see if we have a token that can handle
	     * this cipher.  It isn't part of the static definition.
	     */
	    cipher_def = ssl_LookupCipherSuiteDef(suite->cipher_suite);
	    if (!cipher_def) {
	    	suite->isPresent = PR_FALSE;
		continue;
	    }
	    cipher_alg=bulk_cipher_defs[cipher_def->bulk_cipher_alg ].calg;
	    PORT_Assert(  alg2Mech[cipher_alg].calg == cipher_alg);
	    cipher_mech = alg2Mech[cipher_alg].cmech;
	    exchKeyType =
	    	    kea_defs[cipher_def->key_exchange_alg].exchKeyType;
#ifndef NSS_ENABLE_ECC
	    svrAuth = ss->serverCerts + exchKeyType;
#else
	    /* XXX SSLKEAType isn't really a good choice for 
	     * indexing certificates. It doesn't work for
	     * (EC)DHE-* ciphers. Here we use a hack to ensure
	     * that the server uses an RSA cert for (EC)DHE-RSA.
	     */
	    switch (cipher_def->key_exchange_alg) {
	    case kea_ecdhe_rsa:
#if NSS_SERVER_DHE_IMPLEMENTED
	    /* XXX NSS does not yet implement the server side of _DHE_
	     * cipher suites.  Correcting the computation for svrAuth,
	     * as the case below does, causes NSS SSL servers to begin to
	     * negotiate cipher suites they do not implement.  So, until
	     * server side _DHE_ is implemented, keep this disabled.
	     */
	    case kea_dhe_rsa:
#endif
		svrAuth = ss->serverCerts + kt_rsa;
		break;
	    case kea_ecdh_ecdsa:
	    case kea_ecdh_rsa:
	        /* 
		 * XXX We ought to have different indices for 
		 * ECDSA- and RSA-signed EC certificates so
		 * we could support both key exchange mechanisms
		 * simultaneously. For now, both of them use
		 * whatever is in the certificate slot for kt_ecdh
		 */
	    default:
		svrAuth = ss->serverCerts + exchKeyType;
		break;
	    }
#endif /* NSS_ENABLE_ECC */

	    /* Mark the suites that are backed by real tokens, certs and keys */
	    suite->isPresent = (PRBool)
		(((exchKeyType == kt_null) ||
		   ((!isServer || (svrAuth->serverKeyPair &&
		                   svrAuth->SERVERKEY &&
				   svrAuth->serverCertChain)) &&
		    PK11_TokenExists(kea_alg_defs[exchKeyType]))) &&
		((cipher_alg == calg_null) || PK11_TokenExists(cipher_mech)));
	    if (suite->isPresent)
	    	++numPresent;
	}
    }
    PORT_Assert(numPresent > 0 || numEnabled == 0);
    if (numPresent <= 0) {
	PORT_SetError(SSL_ERROR_NO_CIPHERS_SUPPORTED);
    }
    return numPresent;
}


/* return PR_TRUE if suite matches policy and enabled state */
/* It would be a REALLY BAD THING (tm) if we ever permitted the use
** of a cipher that was NOT_ALLOWED.  So, if this is ever called with
** policy == SSL_NOT_ALLOWED, report no match.
*/
/* adjust suite enabled to the availability of a token that can do the
 * cipher suite. */
static PRBool
config_match(ssl3CipherSuiteCfg *suite, int policy, PRBool enabled)
{
    PORT_Assert(policy != SSL_NOT_ALLOWED && enabled != PR_FALSE);
    if (policy == SSL_NOT_ALLOWED || !enabled)
    	return PR_FALSE;
    return (PRBool)(suite->enabled &&
                    suite->isPresent &&
	            suite->policy != SSL_NOT_ALLOWED &&
		    suite->policy <= policy);
}

/* return number of cipher suites that match policy and enabled state */
/* called from ssl3_SendClientHello and ssl3_ConstructV2CipherSpecsHack */
static int
count_cipher_suites(sslSocket *ss, int policy, PRBool enabled)
{
    int i, count = 0;

    if (!ss->opt.enableSSL3 && !ss->opt.enableTLS) {
    	return 0;
    }
    for (i = 0; i < ssl_V3_SUITES_IMPLEMENTED; i++) {
	if (config_match(&ss->cipherSuites[i], policy, enabled))
	    count++;
    }
    if (count <= 0) {
	PORT_SetError(SSL_ERROR_SSL_DISABLED);
    }
    return count;
}

static PRBool
anyRestrictedEnabled(sslSocket *ss)
{
    int i;

    if (!ss->opt.enableSSL3 && !ss->opt.enableTLS) {
    	return PR_FALSE;
    }
    for (i = 0; i < ssl_V3_SUITES_IMPLEMENTED; i++) {
	ssl3CipherSuiteCfg *suite = &ss->cipherSuites[i];
	if (suite->policy == SSL_RESTRICTED &&
	    suite->enabled &&
	    suite->isPresent)
	    return PR_TRUE;
    }
    return PR_FALSE;
}

/*
 * Null compression, mac and encryption functions
 */

static SECStatus
Null_Cipher(void *ctx, unsigned char *output, int *outputLen, int maxOutputLen,
	    const unsigned char *input, int inputLen)
{
    *outputLen = inputLen;
    if (input != output)
	PORT_Memcpy(output, input, inputLen);
    return SECSuccess;
}

/*
 * SSL3 Utility functions
 */

SECStatus
ssl3_NegotiateVersion(sslSocket *ss, SSL3ProtocolVersion peerVersion)
{
    SSL3ProtocolVersion version;
    SSL3ProtocolVersion maxVersion;

    if (ss->opt.enableTLS) {
	maxVersion = SSL_LIBRARY_VERSION_3_1_TLS;
    } else if (ss->opt.enableSSL3) {
	maxVersion = SSL_LIBRARY_VERSION_3_0;
    } else {
    	/* what are we doing here? */
	PORT_Assert(ss->opt.enableSSL3 || ss->opt.enableTLS);
	PORT_SetError(SSL_ERROR_SSL_DISABLED);
	return SECFailure;
    }

    ss->version = version = PR_MIN(maxVersion, peerVersion);

    if ((version == SSL_LIBRARY_VERSION_3_1_TLS && ss->opt.enableTLS) ||
    	(version == SSL_LIBRARY_VERSION_3_0     && ss->opt.enableSSL3)) {
	return SECSuccess;
    }

    PORT_SetError(SSL_ERROR_NO_CYPHER_OVERLAP);
    return SECFailure;

}

static SECStatus
ssl3_GetNewRandom(SSL3Random *random)
{
    PRIntervalTime gmt = PR_IntervalToSeconds(PR_IntervalNow());
    SECStatus rv;

    random->rand[0] = (unsigned char)(gmt >> 24);
    random->rand[1] = (unsigned char)(gmt >> 16);
    random->rand[2] = (unsigned char)(gmt >>  8);
    random->rand[3] = (unsigned char)(gmt);

    /* first 4 bytes are reserverd for time */
    rv = PK11_GenerateRandom(&random->rand[4], SSL3_RANDOM_LENGTH - 4);
    if (rv != SECSuccess) {
	ssl_MapLowLevelError(SSL_ERROR_GENERATE_RANDOM_FAILURE);
    }
    return rv;
}

/* Called by ssl3_SendServerKeyExchange and ssl3_SendCertificateVerify */
SECStatus
ssl3_SignHashes(SSL3Hashes *hash, SECKEYPrivateKey *key, SECItem *buf, 
                PRBool isTLS)
{
    SECStatus rv		= SECFailure;
    PRBool    doDerEncode       = PR_FALSE;
    int       signatureLen;
    SECItem   hashItem;

    buf->data    = NULL;
    signatureLen = PK11_SignatureLen(key);
    if (signatureLen <= 0) {
	PORT_SetError(SEC_ERROR_INVALID_KEY);
        goto done;
    }

    buf->len  = (unsigned)signatureLen;
    buf->data = (unsigned char *)PORT_Alloc(signatureLen);
    if (!buf->data)
        goto done;	/* error code was set. */

    switch (key->keyType) {
    case rsaKey:
    	hashItem.data = hash->md5;
    	hashItem.len = sizeof(SSL3Hashes);
	break;
    case dsaKey:
	doDerEncode = isTLS;
	hashItem.data = hash->sha;
	hashItem.len = sizeof(hash->sha);
	break;
#ifdef NSS_ENABLE_ECC
    case ecKey:
	doDerEncode = PR_TRUE;
	hashItem.data = hash->sha;
	hashItem.len = sizeof(hash->sha);
	break;
#endif /* NSS_ENABLE_ECC */
    default:
	PORT_SetError(SEC_ERROR_INVALID_KEY);
	goto done;
    }
    PRINT_BUF(60, (NULL, "hash(es) to be signed", hashItem.data, hashItem.len));

    rv = PK11_Sign(key, buf, &hashItem);
    if (rv != SECSuccess) {
	ssl_MapLowLevelError(SSL_ERROR_SIGN_HASHES_FAILURE);
    } else if (doDerEncode) {
	SECItem   derSig	= {siBuffer, NULL, 0};

	/* This also works for an ECDSA signature */
	rv = DSAU_EncodeDerSigWithLen(&derSig, buf, buf->len);
	if (rv == SECSuccess) {
	    PORT_Free(buf->data);	/* discard unencoded signature. */
	    *buf = derSig;		/* give caller encoded signature. */
	} else if (derSig.data) {
	    PORT_Free(derSig.data);
	}
    }

    PRINT_BUF(60, (NULL, "signed hashes", (unsigned char*)buf->data, buf->len));
done:
    if (rv != SECSuccess && buf->data) {
	PORT_Free(buf->data);
	buf->data = NULL;
    }
    return rv;
}

/* Called from ssl3_HandleServerKeyExchange, ssl3_HandleCertificateVerify */
SECStatus
ssl3_VerifySignedHashes(SSL3Hashes *hash, CERTCertificate *cert, 
                        SECItem *buf, PRBool isTLS, void *pwArg)
{
    SECKEYPublicKey * key;
    SECItem *         signature	= NULL;
    SECStatus         rv;
    SECItem           hashItem;
#ifdef NSS_ENABLE_ECC
    unsigned int      len;
#endif /* NSS_ENABLE_ECC */


    PRINT_BUF(60, (NULL, "check signed hashes",
                  buf->data, buf->len));

    key = CERT_ExtractPublicKey(cert);
    if (key == NULL) {
	/* CERT_ExtractPublicKey doesn't set error code */
	PORT_SetError(SSL_ERROR_EXTRACT_PUBLIC_KEY_FAILURE);
    	return SECFailure;
    }

    switch (key->keyType) {
    case rsaKey:
    	hashItem.data = hash->md5;
    	hashItem.len = sizeof(SSL3Hashes);
	break;
    case dsaKey:
	hashItem.data = hash->sha;
	hashItem.len = sizeof(hash->sha);
	if (isTLS) {
	    signature = DSAU_DecodeDerSig(buf);
	    if (!signature) {
	    	PORT_SetError(SSL_ERROR_BAD_HANDSHAKE_HASH_VALUE);
		return SECFailure;
	    }
	    buf = signature;
	}
	break;

#ifdef NSS_ENABLE_ECC
    case ecKey:
	hashItem.data = hash->sha;
	hashItem.len = sizeof(hash->sha);
	/*
	 * ECDSA signatures always encode the integers r and s 
	 * using ASN (unlike DSA where ASN encoding is used
	 * with TLS but not with SSL3)
	 */
	len = SECKEY_SignatureLen(key);
	if (len == 0) {
	    SECKEY_DestroyPublicKey(key);
	    PORT_SetError(SEC_ERROR_UNSUPPORTED_ELLIPTIC_CURVE);
	    return SECFailure;
	}
	signature = DSAU_DecodeDerSigToLen(buf, len);
	if (!signature) {
	    PORT_SetError(SSL_ERROR_BAD_HANDSHAKE_HASH_VALUE);
	    return SECFailure;
	}
	buf = signature;
	break;
#endif /* NSS_ENABLE_ECC */

    default:
    	SECKEY_DestroyPublicKey(key);
	PORT_SetError(SEC_ERROR_UNSUPPORTED_KEYALG);
	return SECFailure;
    }

    PRINT_BUF(60, (NULL, "hash(es) to be verified",
                  hashItem.data, hashItem.len));

    rv = PK11_Verify(key, buf, &hashItem, pwArg);
    SECKEY_DestroyPublicKey(key);
    if (signature) {
    	SECITEM_FreeItem(signature, PR_TRUE);
    }
    if (rv != SECSuccess) {
	ssl_MapLowLevelError(SSL_ERROR_BAD_HANDSHAKE_HASH_VALUE);
    }
    return rv;
}


/* Caller must set hiLevel error code. */
/* Called from ssl3_ComputeExportRSAKeyHash
 *             ssl3_ComputeDHKeyHash
 * which are called from ssl3_HandleServerKeyExchange. 
 */
SECStatus
ssl3_ComputeCommonKeyHash(PRUint8 * hashBuf, unsigned int bufLen,
			     SSL3Hashes *hashes, PRBool bypassPKCS11)
{
    SECStatus     rv 		= SECSuccess;

    if (bypassPKCS11) {
	MD5_HashBuf (hashes->md5, hashBuf, bufLen);
	SHA1_HashBuf(hashes->sha, hashBuf, bufLen);
    } else {
	rv = PK11_HashBuf(SEC_OID_MD5, hashes->md5, hashBuf, bufLen);
	if (rv != SECSuccess) {
	    ssl_MapLowLevelError(SSL_ERROR_MD5_DIGEST_FAILURE);
	    rv = SECFailure;
	    goto done;
	}

	rv = PK11_HashBuf(SEC_OID_SHA1, hashes->sha, hashBuf, bufLen);
	if (rv != SECSuccess) {
	    ssl_MapLowLevelError(SSL_ERROR_SHA_DIGEST_FAILURE);
	    rv = SECFailure;
	}
    }
done:
    return rv;
}

/* Caller must set hiLevel error code. 
** Called from ssl3_SendServerKeyExchange and 
**             ssl3_HandleServerKeyExchange.
*/
static SECStatus
ssl3_ComputeExportRSAKeyHash(SECItem modulus, SECItem publicExponent,
			     SSL3Random *client_rand, SSL3Random *server_rand,
			     SSL3Hashes *hashes, PRBool bypassPKCS11)
{
    PRUint8     * hashBuf;
    PRUint8     * pBuf;
    SECStatus     rv 		= SECSuccess;
    unsigned int  bufLen;
    PRUint8       buf[2*SSL3_RANDOM_LENGTH + 2 + 4096/8 + 2 + 4096/8];

    bufLen = 2*SSL3_RANDOM_LENGTH + 2 + modulus.len + 2 + publicExponent.len;
    if (bufLen <= sizeof buf) {
    	hashBuf = buf;
    } else {
    	hashBuf = PORT_Alloc(bufLen);
	if (!hashBuf) {
	    return SECFailure;
	}
    }

    memcpy(hashBuf, client_rand, SSL3_RANDOM_LENGTH); 
    	pBuf = hashBuf + SSL3_RANDOM_LENGTH;
    memcpy(pBuf, server_rand, SSL3_RANDOM_LENGTH);
    	pBuf += SSL3_RANDOM_LENGTH;
    pBuf[0]  = (PRUint8)(modulus.len >> 8);
    pBuf[1]  = (PRUint8)(modulus.len);
    	pBuf += 2;
    memcpy(pBuf, modulus.data, modulus.len);
    	pBuf += modulus.len;
    pBuf[0] = (PRUint8)(publicExponent.len >> 8);
    pBuf[1] = (PRUint8)(publicExponent.len);
    	pBuf += 2;
    memcpy(pBuf, publicExponent.data, publicExponent.len);
    	pBuf += publicExponent.len;
    PORT_Assert((unsigned int)(pBuf - hashBuf) == bufLen);

    rv = ssl3_ComputeCommonKeyHash(hashBuf, bufLen, hashes, bypassPKCS11);

    PRINT_BUF(95, (NULL, "RSAkey hash: ", hashBuf, bufLen));
    PRINT_BUF(95, (NULL, "RSAkey hash: MD5 result", hashes->md5, MD5_LENGTH));
    PRINT_BUF(95, (NULL, "RSAkey hash: SHA1 result", hashes->sha, SHA1_LENGTH));

    if (hashBuf != buf && hashBuf != NULL)
    	PORT_Free(hashBuf);
    return rv;
}

/* Caller must set hiLevel error code. */
/* Called from ssl3_HandleServerKeyExchange. */
static SECStatus
ssl3_ComputeDHKeyHash(SECItem dh_p, SECItem dh_g, SECItem dh_Ys,
			     SSL3Random *client_rand, SSL3Random *server_rand,
			     SSL3Hashes *hashes, PRBool bypassPKCS11)
{
    PRUint8     * hashBuf;
    PRUint8     * pBuf;
    SECStatus     rv 		= SECSuccess;
    unsigned int  bufLen;
    PRUint8       buf[2*SSL3_RANDOM_LENGTH + 2 + 4096/8 + 2 + 4096/8];

    bufLen = 2*SSL3_RANDOM_LENGTH + 2 + dh_p.len + 2 + dh_g.len + 2 + dh_Ys.len;
    if (bufLen <= sizeof buf) {
    	hashBuf = buf;
    } else {
    	hashBuf = PORT_Alloc(bufLen);
	if (!hashBuf) {
	    return SECFailure;
	}
    }

    memcpy(hashBuf, client_rand, SSL3_RANDOM_LENGTH); 
    	pBuf = hashBuf + SSL3_RANDOM_LENGTH;
    memcpy(pBuf, server_rand, SSL3_RANDOM_LENGTH);
    	pBuf += SSL3_RANDOM_LENGTH;
    pBuf[0]  = (PRUint8)(dh_p.len >> 8);
    pBuf[1]  = (PRUint8)(dh_p.len);
    	pBuf += 2;
    memcpy(pBuf, dh_p.data, dh_p.len);
    	pBuf += dh_p.len;
    pBuf[0] = (PRUint8)(dh_g.len >> 8);
    pBuf[1] = (PRUint8)(dh_g.len);
    	pBuf += 2;
    memcpy(pBuf, dh_g.data, dh_g.len);
    	pBuf += dh_g.len;
    pBuf[0] = (PRUint8)(dh_Ys.len >> 8);
    pBuf[1] = (PRUint8)(dh_Ys.len);
    	pBuf += 2;
    memcpy(pBuf, dh_Ys.data, dh_Ys.len);
    	pBuf += dh_Ys.len;
    PORT_Assert((unsigned int)(pBuf - hashBuf) == bufLen);

    rv = ssl3_ComputeCommonKeyHash(hashBuf, bufLen, hashes, bypassPKCS11);

    PRINT_BUF(95, (NULL, "DHkey hash: ", hashBuf, bufLen));
    PRINT_BUF(95, (NULL, "DHkey hash: MD5 result", hashes->md5, MD5_LENGTH));
    PRINT_BUF(95, (NULL, "DHkey hash: SHA1 result", hashes->sha, SHA1_LENGTH));

    if (hashBuf != buf && hashBuf != NULL)
    	PORT_Free(hashBuf);
    return rv;
}

static void
ssl3_BumpSequenceNumber(SSL3SequenceNumber *num)
{
    num->low++;
    if (num->low == 0)
	num->high++;
}

/* Called twice, only from ssl3_DestroyCipherSpec (immediately below). */
static void
ssl3_CleanupKeyMaterial(ssl3KeyMaterial *mat)
{
    if (mat->write_key != NULL) {
	PK11_FreeSymKey(mat->write_key);
	mat->write_key = NULL;
    }
    if (mat->write_mac_key != NULL) {
	PK11_FreeSymKey(mat->write_mac_key);
	mat->write_mac_key = NULL;
    }
    if (mat->write_mac_context != NULL) {
	PK11_DestroyContext(mat->write_mac_context, PR_TRUE);
	mat->write_mac_context = NULL;
    }
}

/* Called from ssl3_SendChangeCipherSpecs() and 
**	       ssl3_HandleChangeCipherSpecs()
**             ssl3_DestroySSL3Info
** Caller must hold SpecWriteLock.
*/
static void
ssl3_DestroyCipherSpec(ssl3CipherSpec *spec)
{
    PRBool freeit = (PRBool)(!spec->bypassCiphers);
/*  PORT_Assert( ss->opt.noLocks || ssl_HaveSpecWriteLock(ss)); Don't have ss! */
    if (spec->destroy) {
	spec->destroy(spec->encodeContext, freeit);
	spec->destroy(spec->decodeContext, freeit);
	spec->encodeContext = NULL; /* paranoia */
	spec->decodeContext = NULL;
    }
    if (spec->master_secret != NULL) {
	PK11_FreeSymKey(spec->master_secret);
	spec->master_secret = NULL;
    }
    spec->msItem.data = NULL;
    spec->msItem.len  = 0;
    ssl3_CleanupKeyMaterial(&spec->client);
    ssl3_CleanupKeyMaterial(&spec->server);
    spec->bypassCiphers = PR_FALSE;
    spec->destroy=NULL;
}

/* Fill in the pending cipher spec with info from the selected ciphersuite.
** This is as much initialization as we can do without having key material.
** Called from ssl3_HandleServerHello(), ssl3_SendServerHello()
** Caller must hold the ssl3 handshake lock.
** Acquires & releases SpecWriteLock.
*/
static SECStatus
ssl3_SetupPendingCipherSpec(sslSocket *ss)
{
    ssl3CipherSpec *          pwSpec;
    ssl3CipherSpec *          cwSpec;
    ssl3CipherSuite           suite     = ss->ssl3.hs.cipher_suite;
    SSL3MACAlgorithm          mac;
    SSL3BulkCipher            cipher;
    SSL3KeyExchangeAlgorithm  kea;
    const ssl3CipherSuiteDef *suite_def;
    PRBool                    isTLS;

    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss));

    ssl_GetSpecWriteLock(ss);  /*******************************/

    pwSpec = ss->ssl3.pwSpec;
    PORT_Assert(pwSpec == ss->ssl3.prSpec);

    /* This hack provides maximal interoperability with SSL 3 servers. */
    cwSpec = ss->ssl3.cwSpec;
    if (cwSpec->mac_def->mac == mac_null) {
	/* SSL records are not being MACed. */
	cwSpec->version = ss->version;
    }

    pwSpec->version  = ss->version;
    isTLS  = (PRBool)(pwSpec->version > SSL_LIBRARY_VERSION_3_0);

    SSL_TRC(3, ("%d: SSL3[%d]: Set XXX Pending Cipher Suite to 0x%04x",
		SSL_GETPID(), ss->fd, suite));

    suite_def = ssl_LookupCipherSuiteDef(suite);
    if (suite_def == NULL) {
	ssl_ReleaseSpecWriteLock(ss);
	return SECFailure;	/* error code set by ssl_LookupCipherSuiteDef */
    }


    cipher = suite_def->bulk_cipher_alg;
    kea    = suite_def->key_exchange_alg;
    mac    = suite_def->mac_alg;
    if (isTLS)
	mac += 2;

    ss->ssl3.hs.suite_def = suite_def;
    ss->ssl3.hs.kea_def   = &kea_defs[kea];
    PORT_Assert(ss->ssl3.hs.kea_def->kea == kea);

    pwSpec->cipher_def   = &bulk_cipher_defs[cipher];
    PORT_Assert(pwSpec->cipher_def->cipher == cipher);

    pwSpec->mac_def = &mac_defs[mac];
    PORT_Assert(pwSpec->mac_def->mac == mac);

    ss->sec.keyBits       = pwSpec->cipher_def->key_size        * BPB;
    ss->sec.secretKeyBits = pwSpec->cipher_def->secret_key_size * BPB;
    ss->sec.cipherType    = cipher;

    pwSpec->encodeContext = NULL;
    pwSpec->decodeContext = NULL;

    pwSpec->mac_size = pwSpec->mac_def->mac_size;

    ssl_ReleaseSpecWriteLock(ss);  /*******************************/
    return SECSuccess;
}

/* Initialize encryption and MAC contexts for pending spec.
 * Master Secret already is derived in spec->msItem
 * Caller holds Spec write lock.
 */
static SECStatus
ssl3_InitPendingContextsBypass(sslSocket *ss)
{
      ssl3CipherSpec  *  pwSpec;
const ssl3BulkCipherDef *cipher_def;
      void *             serverContext = NULL;
      void *             clientContext = NULL;
      BLapiInitContextFunc initFn = (BLapiInitContextFunc)NULL;
      int                mode     = 0;
      unsigned int       optArg1  = 0;
      unsigned int       optArg2  = 0;
      PRBool             server_encrypts = ss->sec.isServer;
      CK_ULONG           macLength;
      SSLCipherAlgorithm calg;
      SECStatus          rv;

    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss));

    PORT_Assert(ss->ssl3.prSpec == ss->ssl3.pwSpec);

    pwSpec        = ss->ssl3.pwSpec;
    cipher_def    = pwSpec->cipher_def;
    macLength     = pwSpec->mac_size;

    /* MAC setup is done when computing the mac, not here.
     * Now setup the crypto contexts.
     */

    calg = cipher_def->calg;

    serverContext = pwSpec->server.cipher_context;
    clientContext = pwSpec->client.cipher_context;

    switch (calg) {
    case ssl_calg_null:
	pwSpec->encode  = Null_Cipher;
	pwSpec->decode  = Null_Cipher;
        pwSpec->destroy = NULL;
	goto success;

    case ssl_calg_rc4:
      	initFn = (BLapiInitContextFunc)RC4_InitContext;
	pwSpec->encode  = (SSLCipher) RC4_Encrypt;
	pwSpec->decode  = (SSLCipher) RC4_Decrypt;
	pwSpec->destroy = (SSLDestroy) RC4_DestroyContext;
	break;
    case ssl_calg_rc2:
      	initFn = (BLapiInitContextFunc)RC2_InitContext;
	mode = NSS_RC2_CBC;
	optArg1 = cipher_def->key_size;
	pwSpec->encode  = (SSLCipher) RC2_Encrypt;
	pwSpec->decode  = (SSLCipher) RC2_Decrypt;
	pwSpec->destroy = (SSLDestroy) RC2_DestroyContext;
	break;
    case ssl_calg_des:
      	initFn = (BLapiInitContextFunc)DES_InitContext;
	mode = NSS_DES_CBC;
	optArg1 = server_encrypts;
	pwSpec->encode  = (SSLCipher) DES_Encrypt;
	pwSpec->decode  = (SSLCipher) DES_Decrypt;
	pwSpec->destroy = (SSLDestroy) DES_DestroyContext;
	break;
    case ssl_calg_3des:
      	initFn = (BLapiInitContextFunc)DES_InitContext;
	mode = NSS_DES_EDE3_CBC;
	optArg1 = server_encrypts;
	pwSpec->encode  = (SSLCipher) DES_Encrypt;
	pwSpec->decode  = (SSLCipher) DES_Decrypt;
	pwSpec->destroy = (SSLDestroy) DES_DestroyContext;
	break;
    case ssl_calg_aes:
      	initFn = (BLapiInitContextFunc)AES_InitContext;
	mode = NSS_AES_CBC;
	optArg1 = server_encrypts;
	optArg2 = AES_BLOCK_SIZE;
	pwSpec->encode  = (SSLCipher) AES_Encrypt;
	pwSpec->decode  = (SSLCipher) AES_Decrypt;
	pwSpec->destroy = (SSLDestroy) AES_DestroyContext;
	break;

    case ssl_calg_idea:
    case ssl_calg_fortezza :
    default:
	PORT_Assert(0);
	PORT_SetError(SEC_ERROR_LIBRARY_FAILURE);
	goto bail_out;
    }
    rv = (*initFn)(serverContext,
		   pwSpec->server.write_key_item.data,
		   pwSpec->server.write_key_item.len,
		   pwSpec->server.write_iv_item.data,
		   mode, optArg1, optArg2);
    if (rv != SECSuccess) {
	PORT_Assert(0);
	PORT_SetError(SEC_ERROR_LIBRARY_FAILURE);
	goto bail_out;
    }

    if (calg == ssl_calg_des || calg == ssl_calg_3des || calg == ssl_calg_aes) {
	/* For block ciphers, if the server is encrypting, then the client
	 * is decrypting, and vice versa.
	 */
	optArg1 = !optArg1;
    }

    rv = (*initFn)(clientContext,
		   pwSpec->client.write_key_item.data,
		   pwSpec->client.write_key_item.len,
		   pwSpec->client.write_iv_item.data,
		   mode, optArg1, optArg2);
    if (rv != SECSuccess) {
	PORT_Assert(0);
	PORT_SetError(SEC_ERROR_LIBRARY_FAILURE);
	goto bail_out;
    }

    pwSpec->encodeContext = (ss->sec.isServer) ? serverContext : clientContext;
    pwSpec->decodeContext = (ss->sec.isServer) ? clientContext : serverContext;

success:
    return SECSuccess;

bail_out:
    return SECFailure;
}

/* This function should probably be moved to pk11wrap and be named 
 * PK11_ParamFromIVAndEffectiveKeyBits
 */
static SECItem *
ssl3_ParamFromIV(CK_MECHANISM_TYPE mtype, SECItem *iv, CK_ULONG ulEffectiveBits)
{
    SECItem * param = PK11_ParamFromIV(mtype, iv);
    if (param && param->data  && param->len >= sizeof(CK_RC2_PARAMS)) {
	switch (mtype) {
	case CKM_RC2_KEY_GEN:
	case CKM_RC2_ECB:
	case CKM_RC2_CBC:
	case CKM_RC2_MAC:
	case CKM_RC2_MAC_GENERAL:
	case CKM_RC2_CBC_PAD:
	    *(CK_RC2_PARAMS *)param->data = ulEffectiveBits;
	default: break;
	}
    }
    return param;
}

/* Initialize encryption and MAC contexts for pending spec.
 * Master Secret already is derived.
 * Caller holds Spec write lock.
 */
static SECStatus
ssl3_InitPendingContextsPKCS11(sslSocket *ss)
{
      ssl3CipherSpec  *  pwSpec;
const ssl3BulkCipherDef *cipher_def;
      PK11Context *      serverContext = NULL;
      PK11Context *      clientContext = NULL;
      SECItem *          param;
      CK_MECHANISM_TYPE  mechanism;
      CK_MECHANISM_TYPE  mac_mech;
      CK_ULONG           macLength;
      CK_ULONG           effKeyBits;
      SECItem            iv;
      SECItem            mac_param;
      SSLCipherAlgorithm calg;

    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss));

    PORT_Assert(ss->ssl3.prSpec == ss->ssl3.pwSpec);

    pwSpec        = ss->ssl3.pwSpec;
    cipher_def    = pwSpec->cipher_def;
    macLength     = pwSpec->mac_size;

    /* 
    ** Now setup the MAC contexts, 
    **   crypto contexts are setup below.
    */

    pwSpec->client.write_mac_context = NULL;
    pwSpec->server.write_mac_context = NULL;
    mac_mech       = pwSpec->mac_def->mmech;
    mac_param.data = (unsigned char *)&macLength;
    mac_param.len  = sizeof(macLength);
    mac_param.type = 0;

    pwSpec->client.write_mac_context = PK11_CreateContextBySymKey(
	    mac_mech, CKA_SIGN, pwSpec->client.write_mac_key, &mac_param);
    if (pwSpec->client.write_mac_context == NULL)  {
	ssl_MapLowLevelError(SSL_ERROR_SYM_KEY_CONTEXT_FAILURE);
	goto fail;
    }
    pwSpec->server.write_mac_context = PK11_CreateContextBySymKey(
	    mac_mech, CKA_SIGN, pwSpec->server.write_mac_key, &mac_param);
    if (pwSpec->server.write_mac_context == NULL) {
	ssl_MapLowLevelError(SSL_ERROR_SYM_KEY_CONTEXT_FAILURE);
	goto fail;
    }

    /* 
    ** Now setup the crypto contexts.
    */

    calg = cipher_def->calg;
    PORT_Assert(alg2Mech[calg].calg == calg);

    if (calg == calg_null) {
	pwSpec->encode  = Null_Cipher;
	pwSpec->decode  = Null_Cipher;
	pwSpec->destroy = NULL;
	return SECSuccess;
    }
    mechanism = alg2Mech[calg].cmech;
    effKeyBits = cipher_def->key_size * BPB;

    /*
     * build the server context
     */
    iv.data = pwSpec->server.write_iv;
    iv.len  = cipher_def->iv_size;
    param = ssl3_ParamFromIV(mechanism, &iv, effKeyBits);
    if (param == NULL) {
	ssl_MapLowLevelError(SSL_ERROR_IV_PARAM_FAILURE);
    	goto fail;
    }
    serverContext = PK11_CreateContextBySymKey(mechanism,
				(ss->sec.isServer ? CKA_ENCRYPT : CKA_DECRYPT),
				pwSpec->server.write_key, param);
    iv.data = PK11_IVFromParam(mechanism, param, (int *)&iv.len);
    if (iv.data)
    	PORT_Memcpy(pwSpec->server.write_iv, iv.data, iv.len);
    SECITEM_FreeItem(param, PR_TRUE);
    if (serverContext == NULL) {
	ssl_MapLowLevelError(SSL_ERROR_SYM_KEY_CONTEXT_FAILURE);
    	goto fail;
    }

    /*
     * build the client context
     */
    iv.data = pwSpec->client.write_iv;
    iv.len  = cipher_def->iv_size;

    param = ssl3_ParamFromIV(mechanism, &iv, effKeyBits);
    if (param == NULL) {
	ssl_MapLowLevelError(SSL_ERROR_IV_PARAM_FAILURE);
    	goto fail;
    }
    clientContext = PK11_CreateContextBySymKey(mechanism,
				(ss->sec.isServer ? CKA_DECRYPT : CKA_ENCRYPT),
				pwSpec->client.write_key, param);
    iv.data = PK11_IVFromParam(mechanism, param, (int *)&iv.len);
    if (iv.data)
    	PORT_Memcpy(pwSpec->client.write_iv, iv.data, iv.len);
    SECITEM_FreeItem(param,PR_TRUE);
    if (clientContext == NULL) {
	ssl_MapLowLevelError(SSL_ERROR_SYM_KEY_CONTEXT_FAILURE);
    	goto fail;
    }
    pwSpec->encode  = (SSLCipher) PK11_CipherOp;
    pwSpec->decode  = (SSLCipher) PK11_CipherOp;
    pwSpec->destroy = (SSLDestroy) PK11_DestroyContext;

    pwSpec->encodeContext = (ss->sec.isServer) ? serverContext : clientContext;
    pwSpec->decodeContext = (ss->sec.isServer) ? clientContext : serverContext;

    serverContext = NULL;
    clientContext = NULL;

    return SECSuccess;

fail:
    if (serverContext != NULL) PK11_DestroyContext(serverContext, PR_TRUE);
    if (clientContext != NULL) PK11_DestroyContext(clientContext, PR_TRUE);
    if (pwSpec->client.write_mac_context != NULL) {
    	PK11_DestroyContext(pwSpec->client.write_mac_context,PR_TRUE);
	pwSpec->client.write_mac_context = NULL;
    }
    if (pwSpec->server.write_mac_context != NULL) {
    	PK11_DestroyContext(pwSpec->server.write_mac_context,PR_TRUE);
	pwSpec->server.write_mac_context = NULL;
    }

    return SECFailure;
}

/* Complete the initialization of all keys, ciphers, MACs and their contexts
 * for the pending Cipher Spec.
 * Called from: ssl3_SendClientKeyExchange 	(for Full handshake)
 *              ssl3_HandleRSAClientKeyExchange	(for Full handshake)
 *              ssl3_HandleServerHello		(for session restart)
 *              ssl3_HandleClientHello		(for session restart)
 * Sets error code, but caller probably should override to disambiguate.
 * NULL pms means re-use old master_secret.
 *
 * This code is common to the bypass and PKCS11 execution paths.
 * For the bypass case,  pms is NULL.
 */
SECStatus
ssl3_InitPendingCipherSpec(sslSocket *ss, PK11SymKey *pms)
{
    ssl3CipherSpec  *  pwSpec;
    SECStatus          rv;

    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss));

    ssl_GetSpecWriteLock(ss);	/**************************************/

    PORT_Assert(ss->ssl3.prSpec == ss->ssl3.pwSpec);

    pwSpec        = ss->ssl3.pwSpec;

    if (pms || (!pwSpec->msItem.len && !pwSpec->master_secret)) {
	rv = ssl3_DeriveMasterSecret(ss, pms);
	if (rv != SECSuccess) {
	    goto done;  /* err code set by ssl3_DeriveMasterSecret */
	}
    }
    if (ss->opt.bypassPKCS11 && pwSpec->msItem.len && pwSpec->msItem.data) {
	/* Double Bypass succeeded in extracting the master_secret */
	const ssl3KEADef * kea_def = ss->ssl3.hs.kea_def;
	PRBool             isTLS   = (PRBool)(kea_def->tls_keygen ||
                                (pwSpec->version > SSL_LIBRARY_VERSION_3_0));
	pwSpec->bypassCiphers = PR_TRUE;
	rv = ssl3_KeyAndMacDeriveBypass( pwSpec, 
			     (const unsigned char *)&ss->ssl3.hs.client_random,
			     (const unsigned char *)&ss->ssl3.hs.server_random,
			     isTLS, 
			     (PRBool)(kea_def->is_limited));
	if (rv == SECSuccess) {
	    rv = ssl3_InitPendingContextsBypass(ss);
	}
    } else if (pwSpec->master_secret) {
	rv = ssl3_DeriveConnectionKeysPKCS11(ss);
	if (rv == SECSuccess) {
	    rv = ssl3_InitPendingContextsPKCS11(ss);
	}
    } else {
	PORT_Assert(pwSpec->master_secret);
	PORT_SetError(SEC_ERROR_LIBRARY_FAILURE);
	rv = SECFailure;
    }

done:
    ssl_ReleaseSpecWriteLock(ss);	/******************************/
    if (rv != SECSuccess)
	ssl_MapLowLevelError(SSL_ERROR_SESSION_KEY_GEN_FAILURE);
    return rv;
}

/*
 * 60 bytes is 3 times the maximum length MAC size that is supported.
 */
static const unsigned char mac_pad_1 [60] = {
    0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
    0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
    0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
    0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
    0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
    0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
    0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
    0x36, 0x36, 0x36, 0x36
};
static const unsigned char mac_pad_2 [60] = {
    0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c,
    0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c,
    0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c,
    0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c,
    0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c,
    0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c,
    0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c,
    0x5c, 0x5c, 0x5c, 0x5c
};

/* Called from: ssl3_SendRecord()
**		ssl3_HandleRecord()
** Caller must already hold the SpecReadLock. (wish we could assert that!)
*/
static SECStatus
ssl3_ComputeRecordMAC(
    ssl3CipherSpec *   spec,
    PRBool             useServerMacKey,
    SSL3ContentType    type,
    SSL3ProtocolVersion version,
    SSL3SequenceNumber seq_num,
    const SSL3Opaque * input,
    int                inputLength,
    unsigned char *    outbuf,
    unsigned int *     outLength)
{
    const ssl3MACDef * mac_def;
    SECStatus          rv;
    PRBool             isTLS;
    unsigned int       tempLen;
    unsigned char      temp[MAX_MAC_LENGTH];

    temp[0] = (unsigned char)(seq_num.high >> 24);
    temp[1] = (unsigned char)(seq_num.high >> 16);
    temp[2] = (unsigned char)(seq_num.high >>  8);
    temp[3] = (unsigned char)(seq_num.high >>  0);
    temp[4] = (unsigned char)(seq_num.low  >> 24);
    temp[5] = (unsigned char)(seq_num.low  >> 16);
    temp[6] = (unsigned char)(seq_num.low  >>  8);
    temp[7] = (unsigned char)(seq_num.low  >>  0);
    temp[8] = type;

    /* TLS MAC includes the record's version field, SSL's doesn't.
    ** We decide which MAC defintiion to use based on the version of 
    ** the protocol that was negotiated when the spec became current,
    ** NOT based on the version value in the record itself.
    ** But, we use the record'v version value in the computation.
    */
    if (spec->version <= SSL_LIBRARY_VERSION_3_0) {
	temp[9]  = MSB(inputLength);
	temp[10] = LSB(inputLength);
	tempLen  = 11;
	isTLS    = PR_FALSE;
    } else {
    	/* New TLS hash includes version. */
	temp[9]  = MSB(version);
	temp[10] = LSB(version);
	temp[11] = MSB(inputLength);
	temp[12] = LSB(inputLength);
	tempLen  = 13;
	isTLS    = PR_TRUE;
    }

    PRINT_BUF(95, (NULL, "frag hash1: temp", temp, tempLen));
    PRINT_BUF(95, (NULL, "frag hash1: input", input, inputLength));

    mac_def = spec->mac_def;
    if (mac_def->mac == mac_null) {
	*outLength = 0;
	return SECSuccess;
    }
    if (! spec->bypassCiphers) {
	PK11Context *mac_context = 
	    (useServerMacKey ? spec->server.write_mac_context
	                     : spec->client.write_mac_context);
	rv  = PK11_DigestBegin(mac_context);
	rv |= PK11_DigestOp(mac_context, temp, tempLen);
	rv |= PK11_DigestOp(mac_context, input, inputLength);
	rv |= PK11_DigestFinal(mac_context, outbuf, outLength, spec->mac_size);
    } else {
	/* bypass version */
	const SECHashObject *hashObj = NULL;
	unsigned int       pad_bytes;
	PRUint64           write_mac_context[MAX_MAC_CONTEXT_LLONGS];

	switch (mac_def->mac) {
	case ssl_mac_null:
	    *outLength = 0;
	    return SECSuccess;
	case ssl_mac_md5:
	    pad_bytes = 48;
	    hashObj = HASH_GetRawHashObject(HASH_AlgMD5);
	    break;
	case ssl_mac_sha:
	    pad_bytes = 40;
	    hashObj = HASH_GetRawHashObject(HASH_AlgSHA1);
	    break;
	case ssl_hmac_md5: /* used with TLS */
	    hashObj = HASH_GetRawHashObject(HASH_AlgMD5);
	    break;
	case ssl_hmac_sha: /* used with TLS */
	    hashObj = HASH_GetRawHashObject(HASH_AlgSHA1);
	    break;
	default:
	    break;
	}
	if (!hashObj) {
	    PORT_Assert(0);
	    PORT_SetError(SEC_ERROR_LIBRARY_FAILURE);
	    return SECFailure;
	}

	if (!isTLS) {
	    /* compute "inner" part of SSL3 MAC */
	    hashObj->begin(write_mac_context);
	    if (useServerMacKey)
		hashObj->update(write_mac_context, 
				spec->server.write_mac_key_item.data,
				spec->server.write_mac_key_item.len);
	    else
		hashObj->update(write_mac_context, 
				spec->client.write_mac_key_item.data,
				spec->client.write_mac_key_item.len);
	    hashObj->update(write_mac_context, mac_pad_1, pad_bytes);
	    hashObj->update(write_mac_context, temp,  tempLen);
	    hashObj->update(write_mac_context, input, inputLength);
	    hashObj->end(write_mac_context,    temp, &tempLen, sizeof temp);

	    /* compute "outer" part of SSL3 MAC */
	    hashObj->begin(write_mac_context);
	    if (useServerMacKey)
		hashObj->update(write_mac_context, 
				spec->server.write_mac_key_item.data,
				spec->server.write_mac_key_item.len);
	    else
		hashObj->update(write_mac_context, 
				spec->client.write_mac_key_item.data,
				spec->client.write_mac_key_item.len);
	    hashObj->update(write_mac_context, mac_pad_2, pad_bytes);
	    hashObj->update(write_mac_context, temp, tempLen);
	    hashObj->end(write_mac_context, outbuf, outLength, spec->mac_size);
	    rv = SECSuccess;
	} else { /* is TLS */
#define cx ((HMACContext *)write_mac_context)
	    if (useServerMacKey) {
		rv = HMAC_Init(cx, hashObj, 
			       spec->server.write_mac_key_item.data,
			       spec->server.write_mac_key_item.len, PR_FALSE);
	    } else {
		rv = HMAC_Init(cx, hashObj, 
			       spec->client.write_mac_key_item.data,
			       spec->client.write_mac_key_item.len, PR_FALSE);
	    }
	    if (rv == SECSuccess) {
		HMAC_Begin(cx);
		HMAC_Update(cx, temp, tempLen);
		HMAC_Update(cx, input, inputLength);
		rv = HMAC_Finish(cx, outbuf, outLength, spec->mac_size);
		HMAC_Destroy(cx, PR_FALSE);
	    }
#undef cx
	}
    }

    PORT_Assert(rv != SECSuccess || *outLength == (unsigned)spec->mac_size);

    PRINT_BUF(95, (NULL, "frag hash2: result", outbuf, *outLength));

    if (rv != SECSuccess) {
    	rv = SECFailure;
	ssl_MapLowLevelError(SSL_ERROR_MAC_COMPUTATION_FAILURE);
    }
    return rv;
}

static PRBool
ssl3_ClientAuthTokenPresent(sslSessionID *sid) {
    PK11SlotInfo *slot = NULL;
    PRBool isPresent = PR_TRUE;

    /* we only care if we are doing client auth */
    if (!sid || !sid->u.ssl3.clAuthValid) {
	return PR_TRUE;
    }

    /* get the slot */
    slot = SECMOD_LookupSlot(sid->u.ssl3.clAuthModuleID,
	                     sid->u.ssl3.clAuthSlotID);
    if (slot == NULL ||
	!PK11_IsPresent(slot) ||
	sid->u.ssl3.clAuthSeries     != PK11_GetSlotSeries(slot) ||
	sid->u.ssl3.clAuthSlotID     != PK11_GetSlotID(slot)     ||
	sid->u.ssl3.clAuthModuleID   != PK11_GetModuleID(slot)   ||
	(PK11_NeedLogin(slot) && !PK11_IsLoggedIn(slot, NULL))) {
	isPresent = PR_FALSE;
    } 
    if (slot) {
	PK11_FreeSlot(slot);
    }
    return isPresent;
}

static SECStatus
ssl3_CompressMACEncryptRecord(sslSocket *        ss,
                              SSL3ContentType    type,
		              const SSL3Opaque * pIn,
		              PRUint32           contentLen)
{
    ssl3CipherSpec *          cwSpec;
    const ssl3BulkCipherDef * cipher_def;
    sslBuffer      *          wrBuf 	  = &ss->sec.writeBuf;
    SECStatus                 rv;
    PRUint32                  macLen      = 0;
    PRUint32                  fragLen;
    PRUint32  p1Len, p2Len, oddLen = 0;
    PRInt32   cipherBytes =  0;

    /*
     * null compression is easy to do
    PORT_Memcpy(wrBuf->buf + SSL3_RECORD_HEADER_LENGTH, pIn, contentLen);
     */

    ssl_GetSpecReadLock(ss);	/********************************/

    cwSpec = ss->ssl3.cwSpec;
    cipher_def = cwSpec->cipher_def;
    /*
     * Add the MAC
     */
    rv = ssl3_ComputeRecordMAC( cwSpec, (PRBool)(ss->sec.isServer),
	type, cwSpec->version, cwSpec->write_seq_num, pIn, contentLen,
	wrBuf->buf + contentLen + SSL3_RECORD_HEADER_LENGTH, &macLen);
    if (rv != SECSuccess) {
	ssl_MapLowLevelError(SSL_ERROR_MAC_COMPUTATION_FAILURE);
	goto spec_locked_loser;
    }
    p1Len   = contentLen;
    p2Len   = macLen;
    fragLen = contentLen + macLen;	/* needs to be encrypted */
    PORT_Assert(fragLen <= MAX_FRAGMENT_LENGTH + 1024);

    /*
     * Pad the text (if we're doing a block cipher)
     * then Encrypt it
     */
    if (cipher_def->type == type_block) {
	unsigned char * pBuf;
	int             padding_length;
	int             i;

	oddLen = contentLen % cipher_def->block_size;
	/* Assume blockSize is a power of two */
	padding_length = cipher_def->block_size - 1 -
			((fragLen) & (cipher_def->block_size - 1));
	fragLen += padding_length + 1;
	PORT_Assert((fragLen % cipher_def->block_size) == 0);

	/* Pad according to TLS rules (also acceptable to SSL3). */
	pBuf = &wrBuf->buf[fragLen + SSL3_RECORD_HEADER_LENGTH - 1];
	for (i = padding_length + 1; i > 0; --i) {
	    *pBuf-- = padding_length;
	}
	/* now, if contentLen is not a multiple of block size, fix it */
	p2Len = fragLen - p1Len;
    }
    if (p1Len < 256) {
	oddLen = p1Len;
	p1Len = 0;
    } else {
	p1Len -= oddLen;
    }
    if (oddLen) {
	p2Len += oddLen;
	PORT_Assert( (cipher_def->block_size < 2) || \
		     (p2Len % cipher_def->block_size) == 0);
	memcpy(wrBuf->buf + SSL3_RECORD_HEADER_LENGTH + p1Len,
	       pIn + p1Len, oddLen);
    }
    if (p1Len > 0) {
	rv = cwSpec->encode( cwSpec->encodeContext, 
	    wrBuf->buf + SSL3_RECORD_HEADER_LENGTH, /* output */
	    &cipherBytes,                           /* actual outlen */
	    p1Len,                                  /* max outlen */
	    pIn, p1Len);                      /* input, and inputlen */
	PORT_Assert(rv == SECSuccess && cipherBytes == p1Len);
	if (rv != SECSuccess || cipherBytes != p1Len) {
	    PORT_SetError(SSL_ERROR_ENCRYPTION_FAILURE);
	    goto spec_locked_loser;
	}
    }
    if (p2Len > 0) {
	PRInt32 cipherBytesPart2 = -1;
	rv = cwSpec->encode( cwSpec->encodeContext, 
	    wrBuf->buf + SSL3_RECORD_HEADER_LENGTH + p1Len,
	    &cipherBytesPart2,          /* output and actual outLen */
	    p2Len,                             /* max outlen */
	    wrBuf->buf + SSL3_RECORD_HEADER_LENGTH + p1Len,
	    p2Len);                            /* input and inputLen*/
	PORT_Assert(rv == SECSuccess && cipherBytesPart2 == p2Len);
	if (rv != SECSuccess || cipherBytesPart2 != p2Len) {
	    PORT_SetError(SSL_ERROR_ENCRYPTION_FAILURE);
	    goto spec_locked_loser;
	}
	cipherBytes += cipherBytesPart2;
    }	
    PORT_Assert(cipherBytes <= MAX_FRAGMENT_LENGTH + 1024);

    ssl3_BumpSequenceNumber(&cwSpec->write_seq_num);

    wrBuf->len    = cipherBytes + SSL3_RECORD_HEADER_LENGTH;
    wrBuf->buf[0] = type;
    wrBuf->buf[1] = MSB(cwSpec->version);
    wrBuf->buf[2] = LSB(cwSpec->version);
    wrBuf->buf[3] = MSB(cipherBytes);
    wrBuf->buf[4] = LSB(cipherBytes);

    ssl_ReleaseSpecReadLock(ss); /************************************/

    return SECSuccess;

spec_locked_loser:
    ssl_ReleaseSpecReadLock(ss);
    return SECFailure;
}

/* Process the plain text before sending it.
 * Returns the number of bytes of plaintext that were succesfully sent
 * 	plus the number of bytes of plaintext that were copied into the
 *	output (write) buffer.
 * Returns SECFailure on a hard IO error, memory error, or crypto error.
 * Does NOT return SECWouldBlock.
 *
 * Notes on the use of the private ssl flags:
 * (no private SSL flags)
 *    Attempt to make and send SSL records for all plaintext
 *    If non-blocking and a send gets WOULD_BLOCK,
 *    or if the pending (ciphertext) buffer is not empty,
 *    then buffer remaining bytes of ciphertext into pending buf,
 *    and continue to do that for all succssive records until all
 *    bytes are used.
 * ssl_SEND_FLAG_FORCE_INTO_BUFFER
 *    As above, except this suppresses all write attempts, and forces
 *    all ciphertext into the pending ciphertext buffer.
 *
 */
static PRInt32
ssl3_SendRecord(   sslSocket *        ss,
                   SSL3ContentType    type,
		   const SSL3Opaque * pIn,   /* input buffer */
		   PRInt32            nIn,   /* bytes of input */
		   PRInt32            flags)
{
    sslBuffer      *          wrBuf 	  = &ss->sec.writeBuf;
    SECStatus                 rv;
    PRInt32                   totalSent   = 0;

    SSL_TRC(3, ("%d: SSL3[%d] SendRecord type: %s nIn=%d",
		SSL_GETPID(), ss->fd, ssl3_DecodeContentType(type),
		nIn));
    PRINT_BUF(3, (ss, "Send record (plain text)", pIn, nIn));

    PORT_Assert( ss->opt.noLocks || ssl_HaveXmitBufLock(ss) );

    if (ss->ssl3.initialized == PR_FALSE) {
	/* This can happen on a server if the very first incoming record
	** looks like a defective ssl3 record (e.g. too long), and we're
	** trying to send an alert.
	*/
	PR_ASSERT(type == content_alert);
	rv = ssl3_InitState(ss);
	if (rv != SECSuccess) {
	    return SECFailure;	/* ssl3_InitState has set the error code. */
    	}
    }

    /* check for Token Presence */
    if (!ssl3_ClientAuthTokenPresent(ss->sec.ci.sid)) {
	PORT_SetError(SSL_ERROR_TOKEN_INSERTION_REMOVAL);
	return SECFailure;
    }

    while (nIn > 0) {
	PRUint32  contentLen = PR_MIN(nIn, MAX_FRAGMENT_LENGTH);

	if (wrBuf->space < contentLen + SSL3_BUFFER_FUDGE) {
	    PRInt32 newSpace = PR_MAX(wrBuf->space * 2, contentLen);
	    newSpace = PR_MIN(newSpace, MAX_FRAGMENT_LENGTH);
	    newSpace += SSL3_BUFFER_FUDGE;
	    rv = sslBuffer_Grow(wrBuf, newSpace);
	    if (rv != SECSuccess) {
		SSL_DBG(("%d: SSL3[%d]: SendRecord, tried to get %d bytes",
			 SSL_GETPID(), ss->fd, newSpace));
		return SECFailure; /* sslBuffer_Grow set a memory error code. */
	    }
	}

	rv = ssl3_CompressMACEncryptRecord( ss, type, pIn, contentLen);
	if (rv != SECSuccess)
	    return SECFailure;

	pIn += contentLen;
	nIn -= contentLen;
	PORT_Assert( nIn >= 0 );

	PRINT_BUF(50, (ss, "send (encrypted) record data:", wrBuf->buf, wrBuf->len));

	/* If there's still some previously saved ciphertext,
	 * or the caller doesn't want us to send the data yet,
	 * then add all our new ciphertext to the amount previously saved.
	 */
	if ((ss->pendingBuf.len > 0) ||
	    (flags & ssl_SEND_FLAG_FORCE_INTO_BUFFER)) {

	    rv = ssl_SaveWriteData(ss, wrBuf->buf, wrBuf->len);
	    if (rv != SECSuccess) {
		/* presumably a memory error, SEC_ERROR_NO_MEMORY */
		return SECFailure;
	    }
	    wrBuf->len = 0;	/* All cipher text is saved away. */

	    if (!(flags & ssl_SEND_FLAG_FORCE_INTO_BUFFER)) {
		PRInt32   sent;
		ss->handshakeBegun = 1;
		sent = ssl_SendSavedWriteData(ss);
		if (sent < 0 && PR_GetError() != PR_WOULD_BLOCK_ERROR) {
		    ssl_MapLowLevelError(SSL_ERROR_SOCKET_WRITE_FAILURE);
		    return SECFailure;
		}
		if (ss->pendingBuf.len) {
		    flags |= ssl_SEND_FLAG_FORCE_INTO_BUFFER;
		}
	    }
	} else if (wrBuf->len > 0) {
	    PRInt32   sent;
	    ss->handshakeBegun = 1;
	    sent = ssl_DefSend(ss, wrBuf->buf, wrBuf->len,
			       flags & ~ssl_SEND_FLAG_MASK);
	    if (sent < 0) {
		if (PR_GetError() != PR_WOULD_BLOCK_ERROR) {
		    ssl_MapLowLevelError(SSL_ERROR_SOCKET_WRITE_FAILURE);
		    return SECFailure;
		}
		/* we got PR_WOULD_BLOCK_ERROR, which means none was sent. */
		sent = 0;
	    }
	    wrBuf->len -= sent;
	    if (wrBuf->len) {
		/* now take all the remaining unsent new ciphertext and 
		 * append it to the buffer of previously unsent ciphertext.
		 */
		rv = ssl_SaveWriteData(ss, wrBuf->buf + sent, wrBuf->len);
		if (rv != SECSuccess) {
		    /* presumably a memory error, SEC_ERROR_NO_MEMORY */
		    return SECFailure;
		}
	    }
	}
	totalSent += contentLen;
    }
    return totalSent;
}

#define SSL3_PENDING_HIGH_WATER 1024

/* Attempt to send the content of "in" in an SSL application_data record.
 * Returns "len" or SECFailure,   never SECWouldBlock, nor SECSuccess.
 */
int
ssl3_SendApplicationData(sslSocket *ss, const unsigned char *in,
			 PRInt32 len, PRInt32 flags)
{
    PRInt32   totalSent	= 0;
    PRInt32   discarded = 0;

    PORT_Assert( ss->opt.noLocks || ssl_HaveXmitBufLock(ss) );
    if (len < 0 || !in) {
	PORT_SetError(PR_INVALID_ARGUMENT_ERROR);
	return SECFailure;
    }

    if (ss->pendingBuf.len > SSL3_PENDING_HIGH_WATER &&
        !ssl_SocketIsBlocking(ss)) {
	PORT_Assert(!ssl_SocketIsBlocking(ss));
	PORT_SetError(PR_WOULD_BLOCK_ERROR);
	return SECFailure;
    }

    if (ss->appDataBuffered && len) {
	PORT_Assert (in[0] == (unsigned char)(ss->appDataBuffered));
	if (in[0] != (unsigned char)(ss->appDataBuffered)) {
	    PORT_SetError(PR_INVALID_ARGUMENT_ERROR);
	    return SECFailure;
	}
    	in++;
	len--;
	discarded = 1;
    }
    while (len > totalSent) {
	PRInt32   sent, toSend;

	if (totalSent > 0) {
	    /*
	     * The thread yield is intended to give the reader thread a
	     * chance to get some cycles while the writer thread is in
	     * the middle of a large application data write.  (See
	     * Bugzilla bug 127740, comment #1.)
	     */
	    ssl_ReleaseXmitBufLock(ss);
	    PR_Sleep(PR_INTERVAL_NO_WAIT);	/* PR_Yield(); */
	    ssl_GetXmitBufLock(ss);
	}
	toSend = PR_MIN(len - totalSent, MAX_FRAGMENT_LENGTH);
	sent = ssl3_SendRecord(ss, content_application_data, 
	                       in + totalSent, toSend, flags);
	if (sent < 0) {
	    if (totalSent > 0 && PR_GetError() == PR_WOULD_BLOCK_ERROR) {
		PORT_Assert(ss->lastWriteBlocked);
	    	break;
	    }
	    return SECFailure; /* error code set by ssl3_SendRecord */
	}
	totalSent += sent;
	if (ss->pendingBuf.len) {
	    /* must be a non-blocking socket */
	    PORT_Assert(!ssl_SocketIsBlocking(ss));
	    PORT_Assert(ss->lastWriteBlocked);
	    break;	
	}
    }
    if (ss->pendingBuf.len) {
	/* Must be non-blocking. */
	PORT_Assert(!ssl_SocketIsBlocking(ss));
	if (totalSent > 0) {
	    ss->appDataBuffered = 0x100 | in[totalSent - 1];
	}

	totalSent = totalSent + discarded - 1;
	if (totalSent <= 0) {
	    PORT_SetError(PR_WOULD_BLOCK_ERROR);
	    totalSent = SECFailure;
	}
	return totalSent;
    } 
    ss->appDataBuffered = 0;
    return totalSent + discarded;
}

/* Attempt to send the content of sendBuf buffer in an SSL handshake record.
 * This function returns SECSuccess or SECFailure, never SECWouldBlock.
 * Always set sendBuf.len to 0, even when returning SECFailure.
 *
 * Called from SSL3_SendAlert(), ssl3_SendChangeCipherSpecs(),
 *             ssl3_AppendHandshake(), ssl3_SendClientHello(),
 *             ssl3_SendHelloRequest(), ssl3_SendServerHelloDone(),
 *             ssl3_SendFinished(),
 */
static SECStatus
ssl3_FlushHandshake(sslSocket *ss, PRInt32 flags)
{
    PRInt32 rv = SECSuccess;

    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss));
    PORT_Assert( ss->opt.noLocks || ssl_HaveXmitBufLock(ss) );

    if (!ss->sec.ci.sendBuf.buf || !ss->sec.ci.sendBuf.len)
	return rv;

    /* only this flag is allowed */
    PORT_Assert(!(flags & ~ssl_SEND_FLAG_FORCE_INTO_BUFFER));
    if ((flags & ~ssl_SEND_FLAG_FORCE_INTO_BUFFER) != 0) {
	PORT_SetError(SEC_ERROR_INVALID_ARGS);
	rv = SECFailure;
    } else {
	rv = ssl3_SendRecord(ss, content_handshake, ss->sec.ci.sendBuf.buf,
			     ss->sec.ci.sendBuf.len, flags);
    }
    if (rv < 0) { 
    	int err = PORT_GetError();
	PORT_Assert(err != PR_WOULD_BLOCK_ERROR);
	if (err == PR_WOULD_BLOCK_ERROR) {
	    PORT_SetError(SEC_ERROR_LIBRARY_FAILURE);
	}
    } else if (rv < ss->sec.ci.sendBuf.len) {
    	/* short write should never happen */
	PORT_Assert(rv >= ss->sec.ci.sendBuf.len);
	PORT_SetError(SEC_ERROR_LIBRARY_FAILURE);
	rv = SECFailure;
    } else {
	rv = SECSuccess;
    }

    /* Whether we succeeded or failed, toss the old handshake data. */
    ss->sec.ci.sendBuf.len = 0;
    return rv;
}

/*
 * Called from ssl3_HandleAlert and from ssl3_HandleCertificate when
 * the remote client sends a negative response to our certificate request.
 * Returns SECFailure if the application has required client auth.
 *         SECSuccess otherwise.
 */
static SECStatus
ssl3_HandleNoCertificate(sslSocket *ss)
{
    if (ss->sec.peerCert != NULL) {
	if (ss->sec.peerKey != NULL) {
	    SECKEY_DestroyPublicKey(ss->sec.peerKey);
	    ss->sec.peerKey = NULL;
	}
	CERT_DestroyCertificate(ss->sec.peerCert);
	ss->sec.peerCert = NULL;
    }
    ssl3_CleanupPeerCerts(ss);

    /* If the server has required client-auth blindly but doesn't
     * actually look at the certificate it won't know that no
     * certificate was presented so we shutdown the socket to ensure
     * an error.  We only do this if we haven't already completed the
     * first handshake because if we're redoing the handshake we 
     * know the server is paying attention to the certificate.
     */
    if ((ss->opt.requireCertificate == SSL_REQUIRE_ALWAYS) ||
	(!ss->firstHsDone && 
	 (ss->opt.requireCertificate == SSL_REQUIRE_FIRST_HANDSHAKE))) {
	PRFileDesc * lower;

	ss->sec.uncache(ss->sec.ci.sid);
	SSL3_SendAlert(ss, alert_fatal, bad_certificate);

	lower = ss->fd->lower;
#ifdef _WIN32
	lower->methods->shutdown(lower, PR_SHUTDOWN_SEND);
#else
	lower->methods->shutdown(lower, PR_SHUTDOWN_BOTH);
#endif
	PORT_SetError(SSL_ERROR_NO_CERTIFICATE);
	return SECFailure;
    }
    return SECSuccess;
}

/************************************************************************
 * Alerts
 */

/*
** Acquires both handshake and XmitBuf locks.
** Called from: ssl3_IllegalParameter	<-
**              ssl3_HandshakeFailure	<-
**              ssl3_HandleAlert	<- ssl3_HandleRecord.
**              ssl3_HandleChangeCipherSpecs <- ssl3_HandleRecord
**              ssl3_ConsumeHandshakeVariable <-
**              ssl3_HandleHelloRequest	<-
**              ssl3_HandleServerHello	<-
**              ssl3_HandleServerKeyExchange <-
**              ssl3_HandleCertificateRequest <-
**              ssl3_HandleServerHelloDone <-
**              ssl3_HandleClientHello	<-
**              ssl3_HandleV2ClientHello <-
**              ssl3_HandleCertificateVerify <-
**              ssl3_HandleClientKeyExchange <-
**              ssl3_HandleCertificate	<-
**              ssl3_HandleFinished	<-
**              ssl3_HandleHandshakeMessage <-
**              ssl3_HandleRecord	<-
**
*/
SECStatus
SSL3_SendAlert(sslSocket *ss, SSL3AlertLevel level, SSL3AlertDescription desc)
{
    uint8 	bytes[2];
    SECStatus	rv;

    SSL_TRC(3, ("%d: SSL3[%d]: send alert record, level=%d desc=%d",
		SSL_GETPID(), ss->fd, level, desc));

    bytes[0] = level;
    bytes[1] = desc;

    ssl_GetSSL3HandshakeLock(ss);
    if (level == alert_fatal) {
	if (ss->sec.ci.sid) {
	    ss->sec.uncache(ss->sec.ci.sid);
	}
    }
    ssl_GetXmitBufLock(ss);
    rv = ssl3_FlushHandshake(ss, ssl_SEND_FLAG_FORCE_INTO_BUFFER);
    if (rv == SECSuccess) {
	PRInt32 sent;
	sent = ssl3_SendRecord(ss, content_alert, bytes, 2, 
			       desc == no_certificate 
			       ? ssl_SEND_FLAG_FORCE_INTO_BUFFER : 0);
	rv = (sent >= 0) ? SECSuccess : (SECStatus)sent;
    }
    ssl_ReleaseXmitBufLock(ss);
    ssl_ReleaseSSL3HandshakeLock(ss);
    return rv;	/* error set by ssl3_FlushHandshake or ssl3_SendRecord */
}

/*
 * Send illegal_parameter alert.  Set generic error number.
 */
static SECStatus
ssl3_IllegalParameter(sslSocket *ss)
{
    PRBool isTLS;

    isTLS = (PRBool)(ss->ssl3.pwSpec->version > SSL_LIBRARY_VERSION_3_0);
    (void)SSL3_SendAlert(ss, alert_fatal, illegal_parameter);
    PORT_SetError(ss->sec.isServer ? SSL_ERROR_BAD_CLIENT
                                   : SSL_ERROR_BAD_SERVER );
    return SECFailure;
}

/*
 * Send handshake_Failure alert.  Set generic error number.
 */
static SECStatus
ssl3_HandshakeFailure(sslSocket *ss)
{
    (void)SSL3_SendAlert(ss, alert_fatal, handshake_failure);
    PORT_SetError( ss->sec.isServer ? SSL_ERROR_BAD_CLIENT
                                    : SSL_ERROR_BAD_SERVER );
    return SECFailure;
}

/*
 * Send handshake_Failure alert.  Set generic error number.
 */
static SECStatus
ssl3_DecodeError(sslSocket *ss)
{
    (void)SSL3_SendAlert(ss, alert_fatal, 
		  ss->version > SSL_LIBRARY_VERSION_3_0 ? decode_error 
							: illegal_parameter);
    PORT_SetError( ss->sec.isServer ? SSL_ERROR_BAD_CLIENT
                                    : SSL_ERROR_BAD_SERVER );
    return SECFailure;
}

/* Called from ssl3_HandleRecord.
** Caller must hold both RecvBuf and Handshake locks.
*/
static SECStatus
ssl3_HandleAlert(sslSocket *ss, sslBuffer *buf)
{
    SSL3AlertLevel       level;
    SSL3AlertDescription desc;
    int                  error;

    PORT_Assert( ss->opt.noLocks || ssl_HaveRecvBufLock(ss) );
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) );

    SSL_TRC(3, ("%d: SSL3[%d]: handle alert record", SSL_GETPID(), ss->fd));

    if (buf->len != 2) {
	(void)ssl3_DecodeError(ss);
	PORT_SetError(SSL_ERROR_RX_MALFORMED_ALERT);
	return SECFailure;
    }
    level = (SSL3AlertLevel)buf->buf[0];
    desc  = (SSL3AlertDescription)buf->buf[1];
    buf->len = 0;
    SSL_TRC(5, ("%d: SSL3[%d] received alert, level = %d, description = %d",
        SSL_GETPID(), ss->fd, level, desc));

    switch (desc) {
    case close_notify:		ss->recvdCloseNotify = 1;
		        	error = SSL_ERROR_CLOSE_NOTIFY_ALERT;     break;
    case unexpected_message: 	error = SSL_ERROR_HANDSHAKE_UNEXPECTED_ALERT;
									  break;
    case bad_record_mac: 	error = SSL_ERROR_BAD_MAC_ALERT; 	  break;
    case decryption_failed: 	error = SSL_ERROR_DECRYPTION_FAILED_ALERT; 
    									  break;
    case record_overflow: 	error = SSL_ERROR_RECORD_OVERFLOW_ALERT;  break;
    case decompression_failure: error = SSL_ERROR_DECOMPRESSION_FAILURE_ALERT;
									  break;
    case handshake_failure: 	error = SSL_ERROR_HANDSHAKE_FAILURE_ALERT;
			        					  break;
    case no_certificate: 	error = SSL_ERROR_NO_CERTIFICATE;	  break;
    case bad_certificate: 	error = SSL_ERROR_BAD_CERT_ALERT; 	  break;
    case unsupported_certificate:error = SSL_ERROR_UNSUPPORTED_CERT_ALERT;break;
    case certificate_revoked: 	error = SSL_ERROR_REVOKED_CERT_ALERT; 	  break;
    case certificate_expired: 	error = SSL_ERROR_EXPIRED_CERT_ALERT; 	  break;
    case certificate_unknown: 	error = SSL_ERROR_CERTIFICATE_UNKNOWN_ALERT;
			        					  break;
    case illegal_parameter: 	error = SSL_ERROR_ILLEGAL_PARAMETER_ALERT;break;

    /* All alerts below are TLS only. */
    case unknown_ca: 		error = SSL_ERROR_UNKNOWN_CA_ALERT;       break;
    case access_denied: 	error = SSL_ERROR_ACCESS_DENIED_ALERT;    break;
    case decode_error: 		error = SSL_ERROR_DECODE_ERROR_ALERT;     break;
    case decrypt_error: 	error = SSL_ERROR_DECRYPT_ERROR_ALERT;    break;
    case export_restriction: 	error = SSL_ERROR_EXPORT_RESTRICTION_ALERT; 
    									  break;
    case protocol_version: 	error = SSL_ERROR_PROTOCOL_VERSION_ALERT; break;
    case insufficient_security: error = SSL_ERROR_INSUFFICIENT_SECURITY_ALERT; 
    									  break;
    case internal_error: 	error = SSL_ERROR_INTERNAL_ERROR_ALERT;   break;
    case user_canceled: 	error = SSL_ERROR_USER_CANCELED_ALERT;    break;
    case no_renegotiation: 	error = SSL_ERROR_NO_RENEGOTIATION_ALERT; break;

    /* Alerts for TLS client hello extensions */
    case unsupported_extension: 
			error = SSL_ERROR_UNSUPPORTED_EXTENSION_ALERT;    break;
    case certificate_unobtainable: 
			error = SSL_ERROR_CERTIFICATE_UNOBTAINABLE_ALERT; break;
    case unrecognized_name: 
			error = SSL_ERROR_UNRECOGNIZED_NAME_ALERT;        break;
    case bad_certificate_status_response: 
			error = SSL_ERROR_BAD_CERT_STATUS_RESPONSE_ALERT; break;
    case bad_certificate_hash_value: 
			error = SSL_ERROR_BAD_CERT_HASH_VALUE_ALERT;      break;
    default: 		error = SSL_ERROR_RX_UNKNOWN_ALERT;               break;
    }
    if (level == alert_fatal) {
	ss->sec.uncache(ss->sec.ci.sid);
	if ((ss->ssl3.hs.ws == wait_server_hello) &&
	    (desc == handshake_failure)) {
	    /* XXX This is a hack.  We're assuming that any handshake failure
	     * XXX on the client hello is a failure to match ciphers.
	     */
	    error = SSL_ERROR_NO_CYPHER_OVERLAP;
	}
	PORT_SetError(error);
	return SECFailure;
    }
    if ((desc == no_certificate) && (ss->ssl3.hs.ws == wait_client_cert)) {
    	/* I'm a server. I've requested a client cert. He hasn't got one. */
	SECStatus rv;

	PORT_Assert(ss->sec.isServer);
	ss->ssl3.hs.ws = wait_client_key;
	rv = ssl3_HandleNoCertificate(ss);
	return rv;
    }
    return SECSuccess;
}

/*
 * Change Cipher Specs
 * Called from ssl3_HandleServerHelloDone,
 *             ssl3_HandleClientHello,
 * and         ssl3_HandleFinished
 *
 * Acquires and releases spec write lock, to protect switching the current
 * and pending write spec pointers.
 */

static SECStatus
ssl3_SendChangeCipherSpecs(sslSocket *ss)
{
    uint8             change = change_cipher_spec_choice;
    ssl3CipherSpec *  pwSpec;
    SECStatus         rv;
    PRInt32           sent;

    SSL_TRC(3, ("%d: SSL3[%d]: send change_cipher_spec record",
		SSL_GETPID(), ss->fd));

    PORT_Assert( ss->opt.noLocks || ssl_HaveXmitBufLock(ss) );
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss));

    rv = ssl3_FlushHandshake(ss, ssl_SEND_FLAG_FORCE_INTO_BUFFER);
    if (rv != SECSuccess) {
	return rv;	/* error code set by ssl3_FlushHandshake */
    }
    sent = ssl3_SendRecord(ss, content_change_cipher_spec, &change, 1,
                              ssl_SEND_FLAG_FORCE_INTO_BUFFER);
    if (sent < 0) {
	return (SECStatus)sent;	/* error code set by ssl3_SendRecord */
    }

    /* swap the pending and current write specs. */
    ssl_GetSpecWriteLock(ss);	/**************************************/
    pwSpec                     = ss->ssl3.pwSpec;
    pwSpec->write_seq_num.high = 0;
    pwSpec->write_seq_num.low  = 0;

    ss->ssl3.pwSpec = ss->ssl3.cwSpec;
    ss->ssl3.cwSpec = pwSpec;

    SSL_TRC(3, ("%d: SSL3[%d] Set Current Write Cipher Suite to Pending",
		SSL_GETPID(), ss->fd ));

    /* We need to free up the contexts, keys and certs ! */
    /* If we are really through with the old cipher spec
     * (Both the read and write sides have changed) destroy it.
     */
    if (ss->ssl3.prSpec == ss->ssl3.pwSpec) {
    	ssl3_DestroyCipherSpec(ss->ssl3.pwSpec);
    }
    ssl_ReleaseSpecWriteLock(ss); /**************************************/

    return SECSuccess;
}

/* Called from ssl3_HandleRecord.
** Caller must hold both RecvBuf and Handshake locks.
 *
 * Acquires and releases spec write lock, to protect switching the current
 * and pending write spec pointers.
*/
static SECStatus
ssl3_HandleChangeCipherSpecs(sslSocket *ss, sslBuffer *buf)
{
    ssl3CipherSpec *           prSpec;
    SSL3WaitState              ws      = ss->ssl3.hs.ws;
    SSL3ChangeCipherSpecChoice change;

    PORT_Assert( ss->opt.noLocks || ssl_HaveRecvBufLock(ss) );
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) );

    SSL_TRC(3, ("%d: SSL3[%d]: handle change_cipher_spec record",
		SSL_GETPID(), ss->fd));

    if (ws != wait_change_cipher) {
	(void)SSL3_SendAlert(ss, alert_fatal, unexpected_message);
	PORT_SetError(SSL_ERROR_RX_UNEXPECTED_CHANGE_CIPHER);
	return SECFailure;
    }

    if(buf->len != 1) {
	(void)ssl3_DecodeError(ss);
	PORT_SetError(SSL_ERROR_RX_MALFORMED_CHANGE_CIPHER);
	return SECFailure;
    }
    change = (SSL3ChangeCipherSpecChoice)buf->buf[0];
    if (change != change_cipher_spec_choice) {
	/* illegal_parameter is correct here for both SSL3 and TLS. */
	(void)ssl3_IllegalParameter(ss);
	PORT_SetError(SSL_ERROR_RX_MALFORMED_CHANGE_CIPHER);
	return SECFailure;
    }
    buf->len = 0;

    /* Swap the pending and current read specs. */
    ssl_GetSpecWriteLock(ss);   /*************************************/
    prSpec                    = ss->ssl3.prSpec;
    prSpec->read_seq_num.high = prSpec->read_seq_num.low = 0;

    ss->ssl3.prSpec  = ss->ssl3.crSpec;
    ss->ssl3.crSpec  = prSpec;
    ss->ssl3.hs.ws   = wait_finished;

    SSL_TRC(3, ("%d: SSL3[%d] Set Current Read Cipher Suite to Pending",
		SSL_GETPID(), ss->fd ));

    /* If we are really through with the old cipher prSpec
     * (Both the read and write sides have changed) destroy it.
     */
    if (ss->ssl3.prSpec == ss->ssl3.pwSpec) {
    	ssl3_DestroyCipherSpec(ss->ssl3.prSpec);
    }
    ssl_ReleaseSpecWriteLock(ss);   /*************************************/
    return SECSuccess;
}

/* This method uses PKCS11 to derive the MS from the PMS, where PMS 
** is a PKCS11 symkey. This is used in all cases except the 
** "triple bypass" with RSA key exchange.
** Called from ssl3_InitPendingCipherSpec.   prSpec is pwSpec.
*/
static SECStatus
ssl3_DeriveMasterSecret(sslSocket *ss, PK11SymKey *pms)
{
    ssl3CipherSpec *  pwSpec = ss->ssl3.pwSpec;
    const ssl3KEADef *kea_def= ss->ssl3.hs.kea_def;
    unsigned char *   cr     = (unsigned char *)&ss->ssl3.hs.client_random;
    unsigned char *   sr     = (unsigned char *)&ss->ssl3.hs.server_random;
    PRBool            isTLS  = (PRBool)(kea_def->tls_keygen ||
                                (pwSpec->version > SSL_LIBRARY_VERSION_3_0));
    /* 
     * Whenever isDH is true, we need to use CKM_TLS_MASTER_KEY_DERIVE_DH
     * which, unlike CKM_TLS_MASTER_KEY_DERIVE, converts arbitrary size
     * data into a 48-byte value. 
     */
    PRBool    isDH = (PRBool) ((ss->ssl3.hs.kea_def->exchKeyType == kt_dh) ||
	                       (ss->ssl3.hs.kea_def->exchKeyType == kt_ecdh));
    SECStatus         rv = SECFailure;
    CK_MECHANISM_TYPE master_derive;
    CK_MECHANISM_TYPE key_derive;
    SECItem           params;
    CK_FLAGS          keyFlags;
    CK_VERSION        pms_version;
    CK_SSL3_MASTER_KEY_DERIVE_PARAMS master_params;

    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss));
    PORT_Assert( ss->opt.noLocks || ssl_HaveSpecWriteLock(ss));
    PORT_Assert(ss->ssl3.prSpec == ss->ssl3.pwSpec);
    if (isTLS) {
	if(isDH) master_derive = CKM_TLS_MASTER_KEY_DERIVE_DH;
	else master_derive = CKM_TLS_MASTER_KEY_DERIVE;
	key_derive    = CKM_TLS_KEY_AND_MAC_DERIVE;
	keyFlags      = CKF_SIGN | CKF_VERIFY;
    } else {
	if (isDH) master_derive = CKM_SSL3_MASTER_KEY_DERIVE_DH;
	else master_derive = CKM_SSL3_MASTER_KEY_DERIVE;
	key_derive    = CKM_SSL3_KEY_AND_MAC_DERIVE;
	keyFlags      = 0;
    }

    if (pms || !pwSpec->master_secret) {
	master_params.pVersion                     = &pms_version;
	master_params.RandomInfo.pClientRandom     = cr;
	master_params.RandomInfo.ulClientRandomLen = SSL3_RANDOM_LENGTH;
	master_params.RandomInfo.pServerRandom     = sr;
	master_params.RandomInfo.ulServerRandomLen = SSL3_RANDOM_LENGTH;

	params.data = (unsigned char *) &master_params;
	params.len  = sizeof master_params;
    }

    if (pms != NULL) {
#if defined(TRACE)
	if (ssl_trace >= 100) {
	    SECStatus extractRV = PK11_ExtractKeyValue(pms);
	    if (extractRV == SECSuccess) {
		SECItem * keyData = PK11_GetKeyData(pms);
		if (keyData && keyData->data && keyData->len) {
		    ssl_PrintBuf(ss, "Pre-Master Secret", 
				 keyData->data, keyData->len);
		}
	    }
	}
#endif
	pwSpec->master_secret = PK11_DeriveWithFlags(pms, master_derive, 
				&params, key_derive, CKA_DERIVE, 0, keyFlags);
	if (!isDH && pwSpec->master_secret && ss->opt.detectRollBack) {
	    SSL3ProtocolVersion client_version;
	    client_version = pms_version.major << 8 | pms_version.minor;
	    if (client_version != ss->clientHelloVersion) {
		/* Destroy it.  Version roll-back detected. */
		PK11_FreeSymKey(pwSpec->master_secret);
	    	pwSpec->master_secret = NULL;
	    }
	}
	if (pwSpec->master_secret == NULL) {
	    /* Generate a faux master secret in the same slot as the old one. */
	    PK11SlotInfo * slot = PK11_GetSlotFromKey((PK11SymKey *)pms);
	    PK11SymKey *   fpms = ssl3_GenerateRSAPMS(ss, pwSpec, slot);

	    PK11_FreeSlot(slot);
	    if (fpms != NULL) {
		pwSpec->master_secret = PK11_DeriveWithFlags(fpms, 
					master_derive, &params, key_derive, 
					CKA_DERIVE, 0, keyFlags);
		PK11_FreeSymKey(fpms);
	    }
	}
    }
    if (pwSpec->master_secret == NULL) {
	/* Generate a faux master secret from the internal slot. */
	PK11SlotInfo *  slot = PK11_GetInternalSlot();
	PK11SymKey *    fpms = ssl3_GenerateRSAPMS(ss, pwSpec, slot);

	PK11_FreeSlot(slot);
	if (fpms != NULL) {
	    pwSpec->master_secret = PK11_DeriveWithFlags(fpms, 
					master_derive, &params, key_derive, 
					CKA_DERIVE, 0, keyFlags);
	    if (pwSpec->master_secret == NULL) {
	    	pwSpec->master_secret = fpms; /* use the fpms as the master. */
		fpms = NULL;
	    }
	}
	if (fpms) {
	    PK11_FreeSymKey(fpms);
    	}
    }
    if (pwSpec->master_secret == NULL) {
	ssl_MapLowLevelError(SSL_ERROR_SESSION_KEY_GEN_FAILURE);
	return rv;
    }
    if (ss->opt.bypassPKCS11) {
	SECItem * keydata;
	/* In hope of doing a "double bypass", 
	 * need to extract the master secret's value from the key object 
	 * and store it raw in the sslSocket struct.
	 */
	rv = PK11_ExtractKeyValue(pwSpec->master_secret);
	if (rv != SECSuccess) {
#if defined(NSS_SURVIVE_DOUBLE_BYPASS_FAILURE)
	    /* The double bypass failed.  
	     * Attempt to revert to an all PKCS#11, non-bypass method.
	     * Do we need any unacquired locks here?
	     */
	    ss->opt.bypassPKCS11 = 0;
	    rv = ssl3_NewHandshakeHashes(ss);
	    if (rv == SECSuccess) {
		rv = ssl3_UpdateHandshakeHashes(ss, ss->ssl3.hs.messages.buf, 
		                                    ss->ssl3.hs.messages.len);
	    }
#endif
	    return rv;
	} 
	/* This returns the address of the secItem inside the key struct,
	 * not a copy or a reference.  So, there's no need to free it.
	 */
	keydata = PK11_GetKeyData(pwSpec->master_secret);
	if (keydata && keydata->len <= sizeof pwSpec->raw_master_secret) {
	    memcpy(pwSpec->raw_master_secret, keydata->data, keydata->len);
	    pwSpec->msItem.data = pwSpec->raw_master_secret;
	    pwSpec->msItem.len  = keydata->len;
	} else {
	    PORT_SetError(SEC_ERROR_LIBRARY_FAILURE);
	    return SECFailure;
	}
    }
    return SECSuccess;
}


/* 
 * Derive encryption and MAC Keys (and IVs) from master secret
 * Sets a useful error code when returning SECFailure.
 *
 * Called only from ssl3_InitPendingCipherSpec(),
 * which in turn is called from
 *              sendRSAClientKeyExchange        (for Full handshake)
 *              sendDHClientKeyExchange         (for Full handshake)
 *              ssl3_HandleClientKeyExchange    (for Full handshake)
 *              ssl3_HandleServerHello          (for session restart)
 *              ssl3_HandleClientHello          (for session restart)
 * Caller MUST hold the specWriteLock, and SSL3HandshakeLock.
 * ssl3_InitPendingCipherSpec does that.
 *
 */
static SECStatus
ssl3_DeriveConnectionKeysPKCS11(sslSocket *ss)
{
    ssl3CipherSpec *         pwSpec     = ss->ssl3.pwSpec;
    const ssl3KEADef *       kea_def    = ss->ssl3.hs.kea_def;
    unsigned char *   cr     = (unsigned char *)&ss->ssl3.hs.client_random;
    unsigned char *   sr     = (unsigned char *)&ss->ssl3.hs.server_random;
    PRBool            isTLS  = (PRBool)(kea_def->tls_keygen ||
                                (pwSpec->version > SSL_LIBRARY_VERSION_3_0));
    /* following variables used in PKCS11 path */
    const ssl3BulkCipherDef *cipher_def = pwSpec->cipher_def;
    PK11SlotInfo *         slot   = NULL;
    PK11SymKey *           symKey = NULL;
    void *                 pwArg  = ss->pkcs11PinArg;
    int                    keySize;
    CK_SSL3_KEY_MAT_PARAMS key_material_params;
    CK_SSL3_KEY_MAT_OUT    returnedKeys;
    CK_MECHANISM_TYPE      key_derive;
    CK_MECHANISM_TYPE      bulk_mechanism;
    SSLCipherAlgorithm     calg;
    SECItem                params;
    PRBool         skipKeysAndIVs = (PRBool)(cipher_def->calg == calg_null);

    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss));
    PORT_Assert( ss->opt.noLocks || ssl_HaveSpecWriteLock(ss));
    PORT_Assert(ss->ssl3.prSpec == ss->ssl3.pwSpec);

    if (!pwSpec->master_secret) {
	PORT_SetError(SSL_ERROR_SESSION_KEY_GEN_FAILURE);
	return SECFailure;
    }
    /*
     * generate the key material
     */
    key_material_params.ulMacSizeInBits = pwSpec->mac_size           * BPB;
    key_material_params.ulKeySizeInBits = cipher_def->secret_key_size* BPB;
    key_material_params.ulIVSizeInBits  = cipher_def->iv_size        * BPB;

    key_material_params.bIsExport = (CK_BBOOL)(kea_def->is_limited);
    /* was:	(CK_BBOOL)(cipher_def->keygen_mode != kg_strong); */

    key_material_params.RandomInfo.pClientRandom     = cr;
    key_material_params.RandomInfo.ulClientRandomLen = SSL3_RANDOM_LENGTH;
    key_material_params.RandomInfo.pServerRandom     = sr;
    key_material_params.RandomInfo.ulServerRandomLen = SSL3_RANDOM_LENGTH;
    key_material_params.pReturnedKeyMaterial         = &returnedKeys;

    returnedKeys.pIVClient = pwSpec->client.write_iv;
    returnedKeys.pIVServer = pwSpec->server.write_iv;
    keySize                = cipher_def->key_size;

    if (skipKeysAndIVs) {
	keySize                             = 0;
        key_material_params.ulKeySizeInBits = 0;
        key_material_params.ulIVSizeInBits  = 0;
    	returnedKeys.pIVClient              = NULL;
    	returnedKeys.pIVServer              = NULL;
    }

    calg = cipher_def->calg;
    PORT_Assert(     alg2Mech[calg].calg == calg);
    bulk_mechanism = alg2Mech[calg].cmech;

    params.data    = (unsigned char *)&key_material_params;
    params.len     = sizeof(key_material_params);

    if (isTLS) {
	key_derive    = CKM_TLS_KEY_AND_MAC_DERIVE;
    } else {
	key_derive    = CKM_SSL3_KEY_AND_MAC_DERIVE;
    }

    /* CKM_SSL3_KEY_AND_MAC_DERIVE is defined to set ENCRYPT, DECRYPT, and
     * DERIVE by DEFAULT */
    symKey = PK11_Derive(pwSpec->master_secret, key_derive, &params,
                         bulk_mechanism, CKA_ENCRYPT, keySize);
    if (!symKey) {
	ssl_MapLowLevelError(SSL_ERROR_SESSION_KEY_GEN_FAILURE);
	return SECFailure;
    }
    /* we really should use the actual mac'ing mechanism here, but we
     * don't because these types are used to map keytype anyway and both
     * mac's map to the same keytype.
     */
    slot  = PK11_GetSlotFromKey(symKey);

    PK11_FreeSlot(slot); /* slot is held until the key is freed */
    pwSpec->client.write_mac_key =
    	PK11_SymKeyFromHandle(slot, symKey, PK11_OriginDerive,
	    CKM_SSL3_SHA1_MAC, returnedKeys.hClientMacSecret, PR_TRUE, pwArg);
    if (pwSpec->client.write_mac_key == NULL ) {
	goto loser;	/* loser sets err */
    }
    pwSpec->server.write_mac_key =
    	PK11_SymKeyFromHandle(slot, symKey, PK11_OriginDerive,
	    CKM_SSL3_SHA1_MAC, returnedKeys.hServerMacSecret, PR_TRUE, pwArg);
    if (pwSpec->server.write_mac_key == NULL ) {
	goto loser;	/* loser sets err */
    }
    if (!skipKeysAndIVs) {
	pwSpec->client.write_key =
		PK11_SymKeyFromHandle(slot, symKey, PK11_OriginDerive,
		     bulk_mechanism, returnedKeys.hClientKey, PR_TRUE, pwArg);
	if (pwSpec->client.write_key == NULL ) {
	    goto loser;	/* loser sets err */
	}
	pwSpec->server.write_key =
		PK11_SymKeyFromHandle(slot, symKey, PK11_OriginDerive,
		     bulk_mechanism, returnedKeys.hServerKey, PR_TRUE, pwArg);
	if (pwSpec->server.write_key == NULL ) {
	    goto loser;	/* loser sets err */
	}
    }
    PK11_FreeSymKey(symKey);
    return SECSuccess;


loser:
    if (symKey) PK11_FreeSymKey(symKey);
    ssl_MapLowLevelError(SSL_ERROR_SESSION_KEY_GEN_FAILURE);
    return SECFailure;
}

static SECStatus 
ssl3_RestartHandshakeHashes(sslSocket *ss)
{
    SECStatus rv = SECSuccess;

    if (ss->opt.bypassPKCS11) {
	ss->ssl3.hs.messages.len = 0;
	MD5_Begin((MD5Context *)ss->ssl3.hs.md5_cx);
	SHA1_Begin((SHA1Context *)ss->ssl3.hs.sha_cx);
    } else {
	rv = PK11_DigestBegin(ss->ssl3.hs.md5);
	if (rv != SECSuccess) {
	    ssl_MapLowLevelError(SSL_ERROR_MD5_DIGEST_FAILURE);
	    return rv;
	}
	rv = PK11_DigestBegin(ss->ssl3.hs.sha);
	if (rv != SECSuccess) {
	    ssl_MapLowLevelError(SSL_ERROR_SHA_DIGEST_FAILURE);
	    return rv;
	}
    }
    return rv;
}

static SECStatus
ssl3_NewHandshakeHashes(sslSocket *ss)
{
    PK11Context *md5  = NULL;
    PK11Context *sha  = NULL;

    /*
     * note: We should probably lookup an SSL3 slot for these
     * handshake hashes in hopes that we wind up with the same slots
     * that the master secret will wind up in ...
     */
    SSL_TRC(30,("%d: SSL3[%d]: start handshake hashes", SSL_GETPID(), ss->fd));
    if (ss->opt.bypassPKCS11) {
	PORT_Assert(!ss->ssl3.hs.messages.buf && !ss->ssl3.hs.messages.space);
	ss->ssl3.hs.messages.buf = NULL;
	ss->ssl3.hs.messages.space = 0;
    } else {
	ss->ssl3.hs.md5 = md5 = PK11_CreateDigestContext(SEC_OID_MD5);
	ss->ssl3.hs.sha = sha = PK11_CreateDigestContext(SEC_OID_SHA1);
	if (md5 == NULL) {
	    ssl_MapLowLevelError(SSL_ERROR_MD5_DIGEST_FAILURE);
	    goto loser;
	}
	if (sha == NULL) {
	    ssl_MapLowLevelError(SSL_ERROR_SHA_DIGEST_FAILURE);
	    goto loser;
	}
    }
    if (SECSuccess == ssl3_RestartHandshakeHashes(ss)) {
	return SECSuccess;
    }

loser:
    if (md5 != NULL) {
    	PK11_DestroyContext(md5, PR_TRUE);
	ss->ssl3.hs.md5 = NULL;
    }
    if (sha != NULL) {
    	PK11_DestroyContext(sha, PR_TRUE);
	ss->ssl3.hs.sha = NULL;
    }
    return SECFailure;

}

/*
 * Handshake messages
 */
/* Called from	ssl3_AppendHandshake()
**		ssl3_StartHandshakeHash()
**		ssl3_HandleV2ClientHello()
**		ssl3_HandleHandshakeMessage()
** Caller must hold the ssl3Handshake lock.
*/
static SECStatus
ssl3_UpdateHandshakeHashes(sslSocket *ss, unsigned char *b, unsigned int l)
{
    SECStatus  rv = SECSuccess;

    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) );

    PRINT_BUF(90, (NULL, "MD5 & SHA handshake hash input:", b, l));

    if (ss->opt.bypassPKCS11) {
	MD5_Update((MD5Context *)ss->ssl3.hs.md5_cx, b, l);
	SHA1_Update((SHA1Context *)ss->ssl3.hs.sha_cx, b, l);
#if defined(NSS_SURVIVE_DOUBLE_BYPASS_FAILURE)
	rv = sslBuffer_Append(&ss->ssl3.hs.messages, b, l);
#endif
	return rv;
    }
    rv = PK11_DigestOp(ss->ssl3.hs.md5, b, l);
    if (rv != SECSuccess) {
	ssl_MapLowLevelError(SSL_ERROR_MD5_DIGEST_FAILURE);
	return rv;
    }
    rv = PK11_DigestOp(ss->ssl3.hs.sha, b, l);
    if (rv != SECSuccess) {
	ssl_MapLowLevelError(SSL_ERROR_SHA_DIGEST_FAILURE);
	return rv;
    }
    return rv;
}

/**************************************************************************
 * Append Handshake functions.
 * All these functions set appropriate error codes.
 * Most rely on ssl3_AppendHandshake to set the error code.
 **************************************************************************/
SECStatus
ssl3_AppendHandshake(sslSocket *ss, const void *void_src, PRInt32 bytes)
{
    unsigned char *  src  = (unsigned char *)void_src;
    int              room = ss->sec.ci.sendBuf.space - ss->sec.ci.sendBuf.len;
    SECStatus        rv;

    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) ); /* protects sendBuf. */

    if (ss->sec.ci.sendBuf.space < MAX_SEND_BUF_LENGTH && room < bytes) {
	rv = sslBuffer_Grow(&ss->sec.ci.sendBuf, PR_MAX(MIN_SEND_BUF_LENGTH,
		 PR_MIN(MAX_SEND_BUF_LENGTH, ss->sec.ci.sendBuf.len + bytes)));
	if (rv != SECSuccess)
	    return rv;	/* sslBuffer_Grow has set a memory error code. */
	room = ss->sec.ci.sendBuf.space - ss->sec.ci.sendBuf.len;
    }

    PRINT_BUF(60, (ss, "Append to Handshake", (unsigned char*)void_src, bytes));
    rv = ssl3_UpdateHandshakeHashes(ss, src, bytes);
    if (rv != SECSuccess)
	return rv;	/* error code set by ssl3_UpdateHandshakeHashes */

    while (bytes > room) {
	if (room > 0)
	    PORT_Memcpy(ss->sec.ci.sendBuf.buf + ss->sec.ci.sendBuf.len, src, 
	                room);
	ss->sec.ci.sendBuf.len += room;
	rv = ssl3_FlushHandshake(ss, ssl_SEND_FLAG_FORCE_INTO_BUFFER);
	if (rv != SECSuccess) {
	    return rv;	/* error code set by ssl3_FlushHandshake */
	}
	bytes -= room;
	src += room;
	room = ss->sec.ci.sendBuf.space;
	PORT_Assert(ss->sec.ci.sendBuf.len == 0);
    }
    PORT_Memcpy(ss->sec.ci.sendBuf.buf + ss->sec.ci.sendBuf.len, src, bytes);
    ss->sec.ci.sendBuf.len += bytes;
    return SECSuccess;
}

SECStatus
ssl3_AppendHandshakeNumber(sslSocket *ss, PRInt32 num, PRInt32 lenSize)
{
    SECStatus rv;
    uint8     b[4];
    uint8 *   p = b;

    switch (lenSize) {
      case 4:
	*p++ = (num >> 24) & 0xff;
      case 3:
	*p++ = (num >> 16) & 0xff;
      case 2:
	*p++ = (num >> 8) & 0xff;
      case 1:
	*p = num & 0xff;
    }
    SSL_TRC(60, ("%d: number:", SSL_GETPID()));
    rv = ssl3_AppendHandshake(ss, &b[0], lenSize);
    return rv;	/* error code set by AppendHandshake, if applicable. */
}

SECStatus
ssl3_AppendHandshakeVariable(
    sslSocket *ss, const SSL3Opaque *src, PRInt32 bytes, PRInt32 lenSize)
{
    SECStatus rv;

    PORT_Assert((bytes < (1<<8) && lenSize == 1) ||
	      (bytes < (1L<<16) && lenSize == 2) ||
	      (bytes < (1L<<24) && lenSize == 3));

    SSL_TRC(60,("%d: append variable:", SSL_GETPID()));
    rv = ssl3_AppendHandshakeNumber(ss, bytes, lenSize);
    if (rv != SECSuccess) {
	return rv;	/* error code set by AppendHandshake, if applicable. */
    }
    SSL_TRC(60, ("data:"));
    rv = ssl3_AppendHandshake(ss, src, bytes);
    return rv;	/* error code set by AppendHandshake, if applicable. */
}

SECStatus
ssl3_AppendHandshakeHeader(sslSocket *ss, SSL3HandshakeType t, PRUint32 length)
{
    SECStatus rv;

    SSL_TRC(30,("%d: SSL3[%d]: append handshake header: type %s",
    	SSL_GETPID(), ss->fd, ssl3_DecodeHandshakeType(t)));
    PRINT_BUF(60, (ss, "MD5 handshake hash:",
    	          (unsigned char*)ss->ssl3.hs.md5_cx, MD5_LENGTH));
    PRINT_BUF(95, (ss, "SHA handshake hash:",
    	          (unsigned char*)ss->ssl3.hs.sha_cx, SHA1_LENGTH));

    rv = ssl3_AppendHandshakeNumber(ss, t, 1);
    if (rv != SECSuccess) {
    	return rv;	/* error code set by AppendHandshake, if applicable. */
    }
    rv = ssl3_AppendHandshakeNumber(ss, length, 3);
    return rv;		/* error code set by AppendHandshake, if applicable. */
}

/**************************************************************************
 * Consume Handshake functions.
 *
 * All data used in these functions is protected by two locks,
 * the RecvBufLock and the SSL3HandshakeLock
 **************************************************************************/

/* Read up the next "bytes" number of bytes from the (decrypted) input
 * stream "b" (which is *length bytes long). Copy them into buffer "v".
 * Reduces *length by bytes.  Advances *b by bytes.
 *
 * If this function returns SECFailure, it has already sent an alert,
 * and has set a generic error code.  The caller should probably
 * override the generic error code by setting another.
 */
SECStatus
ssl3_ConsumeHandshake(sslSocket *ss, void *v, PRInt32 bytes, SSL3Opaque **b,
		      PRUint32 *length)
{
    PORT_Assert( ss->opt.noLocks || ssl_HaveRecvBufLock(ss) );
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) );

    if ((PRUint32)bytes > *length) {
	return ssl3_DecodeError(ss);
    }
    PORT_Memcpy(v, *b, bytes);
    PRINT_BUF(60, (ss, "consume bytes:", *b, bytes));
    *b      += bytes;
    *length -= bytes;
    return SECSuccess;
}

/* Read up the next "bytes" number of bytes from the (decrypted) input
 * stream "b" (which is *length bytes long), and interpret them as an
 * integer in network byte order.  Returns the received value.
 * Reduces *length by bytes.  Advances *b by bytes.
 *
 * Returns SECFailure (-1) on failure.
 * This value is indistinguishable from the equivalent received value.
 * Only positive numbers are to be received this way.
 * Thus, the largest value that may be sent this way is 0x7fffffff.
 * On error, an alert has been sent, and a generic error code has been set.
 */
PRInt32
ssl3_ConsumeHandshakeNumber(sslSocket *ss, PRInt32 bytes, SSL3Opaque **b,
			    PRUint32 *length)
{
    uint8     *buf = *b;
    int       i;
    PRInt32   num = 0;

    PORT_Assert( ss->opt.noLocks || ssl_HaveRecvBufLock(ss) );
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) );
    PORT_Assert( bytes <= sizeof num);

    if ((PRUint32)bytes > *length) {
	return ssl3_DecodeError(ss);
    }
    PRINT_BUF(60, (ss, "consume bytes:", *b, bytes));

    for (i = 0; i < bytes; i++)
	num = (num << 8) + buf[i];
    *b      += bytes;
    *length -= bytes;
    return num;
}

/* Read in two values from the incoming decrypted byte stream "b", which is
 * *length bytes long.  The first value is a number whose size is "bytes"
 * bytes long.  The second value is a byte-string whose size is the value
 * of the first number received.  The latter byte-string, and its length,
 * is returned in the SECItem i.
 *
 * Returns SECFailure (-1) on failure.
 * On error, an alert has been sent, and a generic error code has been set.
 *
 * RADICAL CHANGE for NSS 3.11.  All callers of this function make copies 
 * of the data returned in the SECItem *i, so making a copy of it here
 * is simply wasteful.  So, This function now just sets SECItem *i to 
 * point to the values in the buffer **b.
 */
SECStatus
ssl3_ConsumeHandshakeVariable(sslSocket *ss, SECItem *i, PRInt32 bytes,
			      SSL3Opaque **b, PRUint32 *length)
{
    PRInt32   count;

    PORT_Assert(bytes <= 3);
    i->len  = 0;
    i->data = NULL;
    count = ssl3_ConsumeHandshakeNumber(ss, bytes, b, length);
    if (count < 0) { 		/* Can't test for SECSuccess here. */
    	return SECFailure;
    }
    if (count > 0) {
	if ((PRUint32)count > *length) {
	    return ssl3_DecodeError(ss);
	}
	i->data = *b;
	i->len  = count;
	*b      += count;
	*length -= count;
    }
    return SECSuccess;
}

/**************************************************************************
 * end of Consume Handshake functions.
 **************************************************************************/

/* Extract the hashes of handshake messages to this point.
 * Called from ssl3_SendCertificateVerify
 *             ssl3_SendFinished
 *             ssl3_HandleHandshakeMessage
 *
 * Caller must hold the SSL3HandshakeLock.
 * Caller must hold a read or write lock on the Spec R/W lock.
 *	(There is presently no way to assert on a Read lock.)
 */
static SECStatus
ssl3_ComputeHandshakeHashes(sslSocket *     ss,
                            ssl3CipherSpec *spec,   /* uses ->master_secret */
			    SSL3Hashes *    hashes, /* output goes here. */
			    uint32          sender)
{
    SECStatus     rv        = SECSuccess;
    PRBool        isTLS     = (PRBool)(spec->version > SSL_LIBRARY_VERSION_3_0);
    unsigned int  outLength;
    SSL3Opaque    md5_inner[MAX_MAC_LENGTH];
    SSL3Opaque    sha_inner[MAX_MAC_LENGTH];

    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) );

    if (ss->opt.bypassPKCS11) {
	/* compute them without PKCS11 */
	PRUint64      md5_cx[MAX_MAC_CONTEXT_LLONGS];
	PRUint64      sha_cx[MAX_MAC_CONTEXT_LLONGS];

#define md5cx ((MD5Context *)md5_cx)
#define shacx ((SHA1Context *)sha_cx)

	if (!spec->msItem.data) {
	    PORT_SetError(SSL_ERROR_RX_UNEXPECTED_HANDSHAKE);
	    return SECFailure;
	}

	MD5_Clone (md5cx,  (MD5Context *)ss->ssl3.hs.md5_cx);
	SHA1_Clone(shacx, (SHA1Context *)ss->ssl3.hs.sha_cx);

	if (!isTLS) {
	    /* compute hashes for SSL3. */
	    unsigned char s[4];

	    s[0] = (unsigned char)(sender >> 24);
	    s[1] = (unsigned char)(sender >> 16);
	    s[2] = (unsigned char)(sender >> 8);
	    s[3] = (unsigned char)sender;

	    if (sender != 0) {
		MD5_Update(md5cx, s, 4);
		PRINT_BUF(95, (NULL, "MD5 inner: sender", s, 4));
	    }

	    PRINT_BUF(95, (NULL, "MD5 inner: MAC Pad 1", mac_pad_1, 
			    mac_defs[mac_md5].pad_size));

	    MD5_Update(md5cx, spec->msItem.data, spec->msItem.len);
	    MD5_Update(md5cx, mac_pad_1, mac_defs[mac_md5].pad_size);
	    MD5_End(md5cx, md5_inner, &outLength, MD5_LENGTH);

	    PRINT_BUF(95, (NULL, "MD5 inner: result", md5_inner, outLength));

	    if (sender != 0) {
		SHA1_Update(shacx, s, 4);
		PRINT_BUF(95, (NULL, "SHA inner: sender", s, 4));
	    }

	    PRINT_BUF(95, (NULL, "SHA inner: MAC Pad 1", mac_pad_1, 
			    mac_defs[mac_sha].pad_size));

	    SHA1_Update(shacx, spec->msItem.data, spec->msItem.len);
	    SHA1_Update(shacx, mac_pad_1, mac_defs[mac_sha].pad_size);
	    SHA1_End(shacx, sha_inner, &outLength, SHA1_LENGTH);

	    PRINT_BUF(95, (NULL, "SHA inner: result", sha_inner, outLength));
	    PRINT_BUF(95, (NULL, "MD5 outer: MAC Pad 2", mac_pad_2, 
			    mac_defs[mac_md5].pad_size));
	    PRINT_BUF(95, (NULL, "MD5 outer: MD5 inner", md5_inner, MD5_LENGTH));

	    MD5_Begin(md5cx);
	    MD5_Update(md5cx, spec->msItem.data, spec->msItem.len);
	    MD5_Update(md5cx, mac_pad_2, mac_defs[mac_md5].pad_size);
	    MD5_Update(md5cx, md5_inner, MD5_LENGTH);
	}
	MD5_End(md5cx, hashes->md5, &outLength, MD5_LENGTH);

	PRINT_BUF(60, (NULL, "MD5 outer: result", hashes->md5, MD5_LENGTH));

	if (!isTLS) {
	    PRINT_BUF(95, (NULL, "SHA outer: MAC Pad 2", mac_pad_2, 
			    mac_defs[mac_sha].pad_size));
	    PRINT_BUF(95, (NULL, "SHA outer: SHA inner", sha_inner, SHA1_LENGTH));

	    SHA1_Begin(shacx);
	    SHA1_Update(shacx, spec->msItem.data, spec->msItem.len);
	    SHA1_Update(shacx, mac_pad_2, mac_defs[mac_sha].pad_size);
	    SHA1_Update(shacx, sha_inner, SHA1_LENGTH);
	}
	SHA1_End(shacx, hashes->sha, &outLength, SHA1_LENGTH);

	PRINT_BUF(60, (NULL, "SHA outer: result", hashes->sha, SHA1_LENGTH));

	rv = SECSuccess;
#undef md5cx
#undef shacx
    } else {
	/* compute hases with PKCS11 */
	PK11Context * md5;
	PK11Context * sha       = NULL;
	unsigned char *md5StateBuf = NULL;
	unsigned char *shaStateBuf = NULL;
	unsigned int  md5StateLen, shaStateLen;
	unsigned char md5StackBuf[256];
	unsigned char shaStackBuf[512];

	if (!spec->master_secret) {
	    PORT_SetError(SSL_ERROR_RX_UNEXPECTED_HANDSHAKE);
	    return SECFailure;
	}

	md5StateBuf = PK11_SaveContextAlloc(ss->ssl3.hs.md5, md5StackBuf,
					    sizeof md5StackBuf, &md5StateLen);
	if (md5StateBuf == NULL) {
	    ssl_MapLowLevelError(SSL_ERROR_MD5_DIGEST_FAILURE);
	    goto loser;
	}
	md5 = ss->ssl3.hs.md5;

	shaStateBuf = PK11_SaveContextAlloc(ss->ssl3.hs.sha, shaStackBuf,
					    sizeof shaStackBuf, &shaStateLen);
	if (shaStateBuf == NULL) {
	    ssl_MapLowLevelError(SSL_ERROR_SHA_DIGEST_FAILURE);
	    goto loser;
	}
	sha = ss->ssl3.hs.sha;

	if (!isTLS) {
	    /* compute hashes for SSL3. */
	    unsigned char s[4];

	    s[0] = (unsigned char)(sender >> 24);
	    s[1] = (unsigned char)(sender >> 16);
	    s[2] = (unsigned char)(sender >> 8);
	    s[3] = (unsigned char)sender;

	    if (sender != 0) {
		rv |= PK11_DigestOp(md5, s, 4);
		PRINT_BUF(95, (NULL, "MD5 inner: sender", s, 4));
	    }

	    PRINT_BUF(95, (NULL, "MD5 inner: MAC Pad 1", mac_pad_1, 
			  mac_defs[mac_md5].pad_size));

	    rv |= PK11_DigestKey(md5,spec->master_secret);
	    rv |= PK11_DigestOp(md5, mac_pad_1, mac_defs[mac_md5].pad_size);
	    rv |= PK11_DigestFinal(md5, md5_inner, &outLength, MD5_LENGTH);
	    PORT_Assert(rv != SECSuccess || outLength == MD5_LENGTH);
	    if (rv != SECSuccess) {
		ssl_MapLowLevelError(SSL_ERROR_MD5_DIGEST_FAILURE);
		rv = SECFailure;
		goto loser;
	    }

	    PRINT_BUF(95, (NULL, "MD5 inner: result", md5_inner, outLength));

	    if (sender != 0) {
		rv |= PK11_DigestOp(sha, s, 4);
		PRINT_BUF(95, (NULL, "SHA inner: sender", s, 4));
	    }

	    PRINT_BUF(95, (NULL, "SHA inner: MAC Pad 1", mac_pad_1, 
			  mac_defs[mac_sha].pad_size));

	    rv |= PK11_DigestKey(sha, spec->master_secret);
	    rv |= PK11_DigestOp(sha, mac_pad_1, mac_defs[mac_sha].pad_size);
	    rv |= PK11_DigestFinal(sha, sha_inner, &outLength, SHA1_LENGTH);
	    PORT_Assert(rv != SECSuccess || outLength == SHA1_LENGTH);
	    if (rv != SECSuccess) {
		ssl_MapLowLevelError(SSL_ERROR_SHA_DIGEST_FAILURE);
		rv = SECFailure;
		goto loser;
	    }

	    PRINT_BUF(95, (NULL, "SHA inner: result", sha_inner, outLength));

	    PRINT_BUF(95, (NULL, "MD5 outer: MAC Pad 2", mac_pad_2, 
			  mac_defs[mac_md5].pad_size));
	    PRINT_BUF(95, (NULL, "MD5 outer: MD5 inner", md5_inner, MD5_LENGTH));

	    rv |= PK11_DigestBegin(md5);
	    rv |= PK11_DigestKey(md5, spec->master_secret);
	    rv |= PK11_DigestOp(md5, mac_pad_2, mac_defs[mac_md5].pad_size);
	    rv |= PK11_DigestOp(md5, md5_inner, MD5_LENGTH);
	}
	rv |= PK11_DigestFinal(md5, hashes->md5, &outLength, MD5_LENGTH);
	PORT_Assert(rv != SECSuccess || outLength == MD5_LENGTH);
	if (rv != SECSuccess) {
	    ssl_MapLowLevelError(SSL_ERROR_MD5_DIGEST_FAILURE);
	    rv = SECFailure;
	    goto loser;
	}

	PRINT_BUF(60, (NULL, "MD5 outer: result", hashes->md5, MD5_LENGTH));

	if (!isTLS) {
	    PRINT_BUF(95, (NULL, "SHA outer: MAC Pad 2", mac_pad_2, 
			  mac_defs[mac_sha].pad_size));
	    PRINT_BUF(95, (NULL, "SHA outer: SHA inner", sha_inner, SHA1_LENGTH));

	    rv |= PK11_DigestBegin(sha);
	    rv |= PK11_DigestKey(sha,spec->master_secret);
	    rv |= PK11_DigestOp(sha, mac_pad_2, mac_defs[mac_sha].pad_size);
	    rv |= PK11_DigestOp(sha, sha_inner, SHA1_LENGTH);
	}
	rv |= PK11_DigestFinal(sha, hashes->sha, &outLength, SHA1_LENGTH);
	PORT_Assert(rv != SECSuccess || outLength == SHA1_LENGTH);
	if (rv != SECSuccess) {
	    ssl_MapLowLevelError(SSL_ERROR_SHA_DIGEST_FAILURE);
	    rv = SECFailure;
	    goto loser;
	}

	PRINT_BUF(60, (NULL, "SHA outer: result", hashes->sha, SHA1_LENGTH));

	rv = SECSuccess;

    loser:
	if (md5StateBuf) {
	    if (PK11_RestoreContext(ss->ssl3.hs.md5, md5StateBuf, md5StateLen)
		 != SECSuccess) 
	    {
		ssl_MapLowLevelError(SSL_ERROR_MD5_DIGEST_FAILURE);
		rv = SECFailure;
	    }
	    if (md5StateBuf != md5StackBuf) {
		PORT_ZFree(md5StateBuf, md5StateLen);
	    }
	}
	if (shaStateBuf) {
	    if (PK11_RestoreContext(ss->ssl3.hs.sha, shaStateBuf, shaStateLen)
		 != SECSuccess) 
	    {
		ssl_MapLowLevelError(SSL_ERROR_SHA_DIGEST_FAILURE);
		rv = SECFailure;
	    }
	    if (shaStateBuf != shaStackBuf) {
		PORT_ZFree(shaStateBuf, shaStateLen);
	    }
	}
    }
    return rv;
}

/*
 * SSL 2 based implementations pass in the initial outbound buffer
 * so that the handshake hash can contain the included information.
 *
 * Called from ssl2_BeginClientHandshake() in sslcon.c
 */
SECStatus
ssl3_StartHandshakeHash(sslSocket *ss, unsigned char * buf, int length)
{
    SECStatus rv;

    ssl_GetSSL3HandshakeLock(ss);  /**************************************/

    rv = ssl3_InitState(ss);
    if (rv != SECSuccess) {
	goto done;		/* ssl3_InitState has set the error code. */
    }

    PORT_Memset(&ss->ssl3.hs.client_random, 0, SSL3_RANDOM_LENGTH);
    PORT_Memcpy(
	&ss->ssl3.hs.client_random.rand[SSL3_RANDOM_LENGTH - SSL_CHALLENGE_BYTES],
	&ss->sec.ci.clientChallenge,
	SSL_CHALLENGE_BYTES);

    rv = ssl3_UpdateHandshakeHashes(ss, buf, length);
    /* if it failed, ssl3_UpdateHandshakeHashes has set the error code. */

done:
    ssl_ReleaseSSL3HandshakeLock(ss);  /**************************************/
    return rv;
}

/**************************************************************************
 * end of Handshake Hash functions.
 * Begin Send and Handle functions for handshakes.
 **************************************************************************/

/* Called from ssl3_HandleHelloRequest(),
 *             ssl3_HandleFinished() (for step-up)
 *             ssl3_RedoHandshake()
 *             ssl2_BeginClientHandshake (when resuming ssl3 session)
 */
SECStatus
ssl3_SendClientHello(sslSocket *ss)
{
    sslSessionID *   sid;
    ssl3CipherSpec * cwSpec;
    SECStatus        rv;
    int              i;
    int              length;
    int              num_suites;
    int              actual_count = 0;
    PRInt32          total_exten_len = 0;

    SSL_TRC(3, ("%d: SSL3[%d]: send client_hello handshake", SSL_GETPID(),
		ss->fd));

    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) );
    PORT_Assert( ss->opt.noLocks || ssl_HaveXmitBufLock(ss) );

    rv = ssl3_InitState(ss);
    if (rv != SECSuccess) {
	return rv;		/* ssl3_InitState has set the error code. */
    }

    SSL_TRC(30,("%d: SSL3[%d]: reset handshake hashes",
	    SSL_GETPID(), ss->fd ));
    rv = ssl3_RestartHandshakeHashes(ss);
    if (rv != SECSuccess) {
	return rv;
    }

    /* We ignore ss->sec.ci.sid here, and use ssl_Lookup because Lookup
     * handles expired entries and other details.
     * XXX If we've been called from ssl2_BeginClientHandshake, then
     * this lookup is duplicative and wasteful.
     */
    sid = (ss->opt.noCache) ? NULL
	    : ssl_LookupSID(&ss->sec.ci.peer, ss->sec.ci.port, ss->peerID, ss->url);

    /* We can't resume based on a different token. If the sid exists,
     * make sure the token that holds the master secret still exists ...
     * If we previously did client-auth, make sure that the token that holds
     * the private key still exists, is logged in, hasn't been removed, etc.
     */
    if (sid) {
	PRBool sidOK = PR_TRUE;
	if (sid->u.ssl3.keys.msIsWrapped) {
	    /* Session key was wrapped, which means it was using PKCS11, */
	    PK11SlotInfo *slot = NULL;
	    if (sid->u.ssl3.masterValid && !ss->opt.bypassPKCS11) {
		slot = SECMOD_LookupSlot(sid->u.ssl3.masterModuleID,
					 sid->u.ssl3.masterSlotID);
	    }
	    if (slot == NULL) {
	       sidOK = PR_FALSE;
	    } else {
		PK11SymKey *wrapKey = NULL;
		if (!PK11_IsPresent(slot) ||
		    ((wrapKey = PK11_GetWrapKey(slot, 
						sid->u.ssl3.masterWrapIndex,
						sid->u.ssl3.masterWrapMech,
						sid->u.ssl3.masterWrapSeries,
						ss->pkcs11PinArg)) == NULL) ) {
		    sidOK = PR_FALSE;
		}
		if (wrapKey) PK11_FreeSymKey(wrapKey);
		PK11_FreeSlot(slot);
		slot = NULL;
	    }
	}
	/* If we previously did client-auth, make sure that the token that
	** holds the private key still exists, is logged in, hasn't been
	** removed, etc.
	*/
	if (sidOK && !ssl3_ClientAuthTokenPresent(sid)) {
	    sidOK = PR_FALSE;
	}

	if (!sidOK) {
	    SSL_AtomicIncrementLong(& ssl3stats.sch_sid_cache_not_ok );
	    (*ss->sec.uncache)(sid);
	    ssl_FreeSID(sid);
	    sid = NULL;
	}
    }

    if (sid) {
	SSL_AtomicIncrementLong(& ssl3stats.sch_sid_cache_hits );

	rv = ssl3_NegotiateVersion(ss, sid->version);
	if (rv != SECSuccess)
	    return rv;	/* error code was set */

	PRINT_BUF(4, (ss, "client, found session-id:", sid->u.ssl3.sessionID,
		      sid->u.ssl3.sessionIDLength));

	ss->ssl3.policy = sid->u.ssl3.policy;
    } else {
	SSL_AtomicIncrementLong(& ssl3stats.sch_sid_cache_misses );

	rv = ssl3_NegotiateVersion(ss, SSL_LIBRARY_VERSION_3_1_TLS);
	if (rv != SECSuccess)
	    return rv;	/* error code was set */

	sid = ssl3_NewSessionID(ss, PR_FALSE);
	if (!sid) {
	    return SECFailure;	/* memory error is set */
        }
    }

    ssl_GetSpecWriteLock(ss);
    cwSpec = ss->ssl3.cwSpec;
    if (cwSpec->mac_def->mac == mac_null) {
	/* SSL records are not being MACed. */
	cwSpec->version = ss->version;
    }
    ssl_ReleaseSpecWriteLock(ss);

    if (ss->sec.ci.sid != NULL) {
	ssl_FreeSID(ss->sec.ci.sid);	/* decrement ref count, free if zero */
    }
    ss->sec.ci.sid = sid;

    ss->sec.send = ssl3_SendApplicationData;

    /* shouldn't get here if SSL3 is disabled, but ... */
    PORT_Assert(ss->opt.enableSSL3 || ss->opt.enableTLS);
    if (!ss->opt.enableSSL3 && !ss->opt.enableTLS) {
	PORT_SetError(SSL_ERROR_SSL_DISABLED);
    	return SECFailure;
    }

    /* how many suites does our PKCS11 support (regardless of policy)? */
    num_suites = ssl3_config_match_init(ss);
    if (!num_suites)
    	return SECFailure;	/* ssl3_config_match_init has set error code. */

    if (ss->opt.enableTLS && ss->version > SSL_LIBRARY_VERSION_3_0) {
	PRUint32 maxBytes = 65535; /* 2^16 - 1 */
	PRInt32  extLen;

	extLen = ssl3_CallHelloExtensionSenders(ss, PR_FALSE, maxBytes, NULL);
	if (extLen < 0) {
	    return SECFailure;
	}
	maxBytes        -= extLen;
	total_exten_len += extLen;

	if (total_exten_len > 0)
	    total_exten_len += 2;
    }
#if defined(NSS_ENABLE_ECC) && !defined(NSS_ECC_MORE_THAN_SUITE_B)
    else { /* SSL3 only */
    	ssl3_DisableECCSuites(ss, NULL); /* disable all ECC suites */
    }
#endif

    /* how many suites are permitted by policy and user preference? */
    num_suites = count_cipher_suites(ss, ss->ssl3.policy, PR_TRUE);
    if (!num_suites)
    	return SECFailure;	/* count_cipher_suites has set error code. */

    length = sizeof(SSL3ProtocolVersion) + SSL3_RANDOM_LENGTH +
	1 + ((sid == NULL) ? 0 : sid->u.ssl3.sessionIDLength) +
	2 + num_suites*sizeof(ssl3CipherSuite) +
	1 + compressionMethodsCount + total_exten_len;

    rv = ssl3_AppendHandshakeHeader(ss, client_hello, length);
    if (rv != SECSuccess) {
	return rv;	/* err set by ssl3_AppendHandshake* */
    }

    ss->clientHelloVersion = ss->version;
    rv = ssl3_AppendHandshakeNumber(ss, ss->clientHelloVersion, 2);
    if (rv != SECSuccess) {
	return rv;	/* err set by ssl3_AppendHandshake* */
    }
    rv = ssl3_GetNewRandom(&ss->ssl3.hs.client_random);
    if (rv != SECSuccess) {
	return rv;	/* err set by GetNewRandom. */
    }
    rv = ssl3_AppendHandshake(ss, &ss->ssl3.hs.client_random,
                              SSL3_RANDOM_LENGTH);
    if (rv != SECSuccess) {
	return rv;	/* err set by ssl3_AppendHandshake* */
    }

    if (sid)
	rv = ssl3_AppendHandshakeVariable(
	    ss, sid->u.ssl3.sessionID, sid->u.ssl3.sessionIDLength, 1);
    else
	rv = ssl3_AppendHandshakeVariable(ss, NULL, 0, 1);
    if (rv != SECSuccess) {
	return rv;	/* err set by ssl3_AppendHandshake* */
    }

    rv = ssl3_AppendHandshakeNumber(ss, num_suites*sizeof(ssl3CipherSuite), 2);
    if (rv != SECSuccess) {
	return rv;	/* err set by ssl3_AppendHandshake* */
    }


    for (i = 0; i < ssl_V3_SUITES_IMPLEMENTED; i++) {
	ssl3CipherSuiteCfg *suite = &ss->cipherSuites[i];
	if (config_match(suite, ss->ssl3.policy, PR_TRUE)) {
	    actual_count++;
	    if (actual_count > num_suites) {
		/* set error card removal/insertion error */
		PORT_SetError(SSL_ERROR_TOKEN_INSERTION_REMOVAL);
		return SECFailure;
	    }
	    rv = ssl3_AppendHandshakeNumber(ss, suite->cipher_suite,
					    sizeof(ssl3CipherSuite));
	    if (rv != SECSuccess) {
		return rv;	/* err set by ssl3_AppendHandshake* */
	    }
	}
    }

    /* if cards were removed or inserted between count_cipher_suites and
     * generating our list, detect the error here rather than send it off to
     * the server.. */
    if (actual_count != num_suites) {
	/* Card removal/insertion error */
	PORT_SetError(SSL_ERROR_TOKEN_INSERTION_REMOVAL);
	return SECFailure;
    }

    rv = ssl3_AppendHandshakeNumber(ss, compressionMethodsCount, 1);
    if (rv != SECSuccess) {
	return rv;	/* err set by ssl3_AppendHandshake* */
    }
    for (i = 0; i < compressionMethodsCount; i++) {
	rv = ssl3_AppendHandshakeNumber(ss, compressions[i], 1);
	if (rv != SECSuccess) {
	    return rv;	/* err set by ssl3_AppendHandshake* */
	}
    }

    if (total_exten_len) {
	PRUint32 maxBytes = total_exten_len - 2;
	PRInt32  extLen;

	rv = ssl3_AppendHandshakeNumber(ss, maxBytes, 2);
	if (rv != SECSuccess) {
	    return rv;	/* err set by AppendHandshake. */
	}

	extLen = ssl3_CallHelloExtensionSenders(ss, PR_TRUE, maxBytes, NULL);
	if (extLen < 0) {
	    return SECFailure;
	}
	maxBytes -= extLen;
	PORT_Assert(!maxBytes);
    }


    rv = ssl3_FlushHandshake(ss, 0);
    if (rv != SECSuccess) {
	return rv;	/* error code set by ssl3_FlushHandshake */
    }

    ss->ssl3.hs.ws = wait_server_hello;
    return rv;
}


/* Called from ssl3_HandleHandshakeMessage() when it has deciphered a complete
 * ssl3 Hello Request.
 * Caller must hold Handshake and RecvBuf locks.
 */
static SECStatus
ssl3_HandleHelloRequest(sslSocket *ss)
{
    sslSessionID *sid = ss->sec.ci.sid;
    SECStatus     rv;

    SSL_TRC(3, ("%d: SSL3[%d]: handle hello_request handshake",
		SSL_GETPID(), ss->fd));

    PORT_Assert( ss->opt.noLocks || ssl_HaveRecvBufLock(ss) );
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) );

    if (ss->ssl3.hs.ws == wait_server_hello)
	return SECSuccess;
    if (ss->ssl3.hs.ws != idle_handshake || ss->sec.isServer) {
	(void)SSL3_SendAlert(ss, alert_fatal, unexpected_message);
	PORT_SetError(SSL_ERROR_RX_UNEXPECTED_HELLO_REQUEST);
	return SECFailure;
    }
    if (sid) {
	ss->sec.uncache(sid);
	ssl_FreeSID(sid);
	ss->sec.ci.sid = NULL;
    }

    ssl_GetXmitBufLock(ss);
    rv = ssl3_SendClientHello(ss);
    ssl_ReleaseXmitBufLock(ss);

    return rv;
}

#define UNKNOWN_WRAP_MECHANISM 0x7fffffff

static const CK_MECHANISM_TYPE wrapMechanismList[SSL_NUM_WRAP_MECHS] = {
    CKM_DES3_ECB,
    CKM_CAST5_ECB,
    CKM_DES_ECB,
    CKM_KEY_WRAP_LYNKS,
    CKM_IDEA_ECB,
    CKM_CAST3_ECB,
    CKM_CAST_ECB,
    CKM_RC5_ECB,
    CKM_RC2_ECB,
    CKM_CDMF_ECB,
    CKM_SKIPJACK_WRAP,
    CKM_SKIPJACK_CBC64,
    CKM_AES_ECB,
    UNKNOWN_WRAP_MECHANISM
};

static int
ssl_FindIndexByWrapMechanism(CK_MECHANISM_TYPE mech)
{
    const CK_MECHANISM_TYPE *pMech = wrapMechanismList;

    while (mech != *pMech && *pMech != UNKNOWN_WRAP_MECHANISM) {
    	++pMech;
    }
    return (*pMech == UNKNOWN_WRAP_MECHANISM) ? -1
                                              : (pMech - wrapMechanismList);
}

static PK11SymKey *
ssl_UnwrapSymWrappingKey(
	SSLWrappedSymWrappingKey *pWswk,
	SECKEYPrivateKey *        svrPrivKey,
	SSL3KEAType               exchKeyType,
	CK_MECHANISM_TYPE         masterWrapMech,
	void *                    pwArg)
{
    PK11SymKey *             unwrappedWrappingKey  = NULL;
    SECItem                  wrappedKey;
#ifdef NSS_ENABLE_ECC
    PK11SymKey *             Ks;
    SECKEYPublicKey          pubWrapKey;
    ECCWrappedKeyInfo        *ecWrapped;
#endif /* NSS_ENABLE_ECC */

    /* found the wrapping key on disk. */
    PORT_Assert(pWswk->symWrapMechanism == masterWrapMech);
    PORT_Assert(pWswk->exchKeyType      == exchKeyType);
    if (pWswk->symWrapMechanism != masterWrapMech ||
	pWswk->exchKeyType      != exchKeyType) {
	goto loser;
    }
    wrappedKey.type = siBuffer;
    wrappedKey.data = pWswk->wrappedSymmetricWrappingkey;
    wrappedKey.len  = pWswk->wrappedSymKeyLen;
    PORT_Assert(wrappedKey.len <= sizeof pWswk->wrappedSymmetricWrappingkey);

    switch (exchKeyType) {

    case kt_rsa:
	unwrappedWrappingKey =
	    PK11_PubUnwrapSymKey(svrPrivKey, &wrappedKey,
				 masterWrapMech, CKA_UNWRAP, 0);
	break;

#ifdef NSS_ENABLE_ECC
    case kt_ecdh:
        /* 
         * For kt_ecdh, we first create an EC public key based on
         * data stored with the wrappedSymmetricWrappingkey. Next,
         * we do an ECDH computation involving this public key and
         * the SSL server's (long-term) EC private key. The resulting
         * shared secret is treated the same way as Fortezza's Ks, i.e.,
         * it is used to recover the symmetric wrapping key.
         *
         * The data in wrappedSymmetricWrappingkey is laid out as defined
         * in the ECCWrappedKeyInfo structure.
         */
        ecWrapped = (ECCWrappedKeyInfo *) pWswk->wrappedSymmetricWrappingkey;

        PORT_Assert(ecWrapped->encodedParamLen + ecWrapped->pubValueLen + 
            ecWrapped->wrappedKeyLen <= MAX_EC_WRAPPED_KEY_BUFLEN);

        if (ecWrapped->encodedParamLen + ecWrapped->pubValueLen + 
            ecWrapped->wrappedKeyLen > MAX_EC_WRAPPED_KEY_BUFLEN) {
            PORT_SetError(SEC_ERROR_LIBRARY_FAILURE);
            goto loser;
        }

        pubWrapKey.keyType = ecKey;
        pubWrapKey.u.ec.size = ecWrapped->size;
        pubWrapKey.u.ec.DEREncodedParams.len = ecWrapped->encodedParamLen;
        pubWrapKey.u.ec.DEREncodedParams.data = ecWrapped->var;
        pubWrapKey.u.ec.publicValue.len = ecWrapped->pubValueLen;
        pubWrapKey.u.ec.publicValue.data = ecWrapped->var + 
            ecWrapped->encodedParamLen;

        wrappedKey.len  = ecWrapped->wrappedKeyLen;
        wrappedKey.data = ecWrapped->var + ecWrapped->encodedParamLen + 
            ecWrapped->pubValueLen;
        
        /* Derive Ks using ECDH */
        Ks = PK11_PubDeriveWithKDF(svrPrivKey, &pubWrapKey, PR_FALSE, NULL,
				   NULL, CKM_ECDH1_DERIVE, masterWrapMech, 
				   CKA_DERIVE, 0, CKD_NULL, NULL, NULL);
        if (Ks == NULL) {
            goto loser;
        }

        /*  Use Ks to unwrap the wrapping key */
        unwrappedWrappingKey = PK11_UnwrapSymKey(Ks, masterWrapMech, NULL, 
						 &wrappedKey, masterWrapMech, 
						 CKA_UNWRAP, 0);
        PK11_FreeSymKey(Ks);
        
        break;
#endif

    default:
	/* Assert? */
	SET_ERROR_CODE
	goto loser;
    }
loser:
    return unwrappedWrappingKey;
}

/* Each process sharing the server session ID cache has its own array of
 * SymKey pointers for the symmetric wrapping keys that are used to wrap
 * the master secrets.  There is one key for each KEA type.  These Symkeys
 * correspond to the wrapped SymKeys kept in the server session cache.
 */

typedef struct {
    PK11SymKey *      symWrapKey[kt_kea_size];
} ssl3SymWrapKey;

static PZLock *          symWrapKeysLock = NULL;
static ssl3SymWrapKey    symWrapKeys[SSL_NUM_WRAP_MECHS];

SECStatus
SSL3_ShutdownServerCache(void)
{
    int             i, j;

    if (!symWrapKeysLock)
    	return SECSuccess;	/* was never initialized */
    PZ_Lock(symWrapKeysLock);
    /* get rid of all symWrapKeys */
    for (i = 0; i < SSL_NUM_WRAP_MECHS; ++i) {
    	for (j = 0; j < kt_kea_size; ++j) {
	    PK11SymKey **   pSymWrapKey;
	    pSymWrapKey = &symWrapKeys[i].symWrapKey[j];
	    if (*pSymWrapKey) {
		PK11_FreeSymKey(*pSymWrapKey);
	    	*pSymWrapKey = NULL;
	    }
	}
    }

    PZ_Unlock(symWrapKeysLock);
    return SECSuccess;
}

void ssl_InitSymWrapKeysLock(void)
{
    /* atomically initialize the lock */
    if (!symWrapKeysLock)
	nss_InitLock(&symWrapKeysLock, nssILockOther);
}

/* Try to get wrapping key for mechanism from in-memory array.
 * If that fails, look for one on disk.
 * If that fails, generate a new one, put the new one on disk,
 * Put the new key in the in-memory array.
 */
static PK11SymKey *
getWrappingKey( sslSocket *       ss,
		PK11SlotInfo *    masterSecretSlot,
		SSL3KEAType       exchKeyType,
                CK_MECHANISM_TYPE masterWrapMech,
	        void *            pwArg)
{
    SECKEYPrivateKey *       svrPrivKey;
    SECKEYPublicKey *        svrPubKey             = NULL;
    PK11SymKey *             unwrappedWrappingKey  = NULL;
    PK11SymKey **            pSymWrapKey;
    CK_MECHANISM_TYPE        asymWrapMechanism = CKM_INVALID_MECHANISM;
    int                      length;
    int                      symWrapMechIndex;
    SECStatus                rv;
    SECItem                  wrappedKey;
    SSLWrappedSymWrappingKey wswk;

    svrPrivKey  = ss->serverCerts[exchKeyType].SERVERKEY;
    PORT_Assert(svrPrivKey != NULL);
    if (!svrPrivKey) {
    	return NULL;	/* why are we here?!? */
    }

    symWrapMechIndex = ssl_FindIndexByWrapMechanism(masterWrapMech);
    PORT_Assert(symWrapMechIndex >= 0);
    if (symWrapMechIndex < 0)
    	return NULL;	/* invalid masterWrapMech. */

    pSymWrapKey = &symWrapKeys[symWrapMechIndex].symWrapKey[exchKeyType];

    ssl_InitSymWrapKeysLock();

    PZ_Lock(symWrapKeysLock);

    unwrappedWrappingKey = *pSymWrapKey;
    if (unwrappedWrappingKey != NULL) {
	if (PK11_VerifyKeyOK(unwrappedWrappingKey)) {
	    unwrappedWrappingKey = PK11_ReferenceSymKey(unwrappedWrappingKey);
	    goto done;
	}
	/* slot series has changed, so this key is no good any more. */
	PK11_FreeSymKey(unwrappedWrappingKey);
	*pSymWrapKey = unwrappedWrappingKey = NULL;
    }

    /* Try to get wrapped SymWrapping key out of the (disk) cache. */
    /* Following call fills in wswk on success. */
    if (ssl_GetWrappingKey(symWrapMechIndex, exchKeyType, &wswk)) {
    	/* found the wrapped sym wrapping key on disk. */
	unwrappedWrappingKey =
	    ssl_UnwrapSymWrappingKey(&wswk, svrPrivKey, exchKeyType,
                                     masterWrapMech, pwArg);
	if (unwrappedWrappingKey) {
	    goto install;
	}
    }

    if (!masterSecretSlot) 	/* caller doesn't want to create a new one. */
    	goto loser;

    length = PK11_GetBestKeyLength(masterSecretSlot, masterWrapMech);
    /* Zero length means fixed key length algorithm, or error.
     * It's ambiguous.
     */
    unwrappedWrappingKey = PK11_KeyGen(masterSecretSlot, masterWrapMech, NULL,
                                       length, pwArg);
    if (!unwrappedWrappingKey) {
    	goto loser;
    }

    /* Prepare the buffer to receive the wrappedWrappingKey,
     * the symmetric wrapping key wrapped using the server's pub key.
     */
    PORT_Memset(&wswk, 0, sizeof wswk);	/* eliminate UMRs. */

    if (ss->serverCerts[exchKeyType].serverKeyPair) {
	svrPubKey = ss->serverCerts[exchKeyType].serverKeyPair->pubKey;
    }
    if (svrPubKey == NULL) {
	PORT_SetError(SEC_ERROR_LIBRARY_FAILURE);
	goto loser;
    }
    wrappedKey.type = siBuffer;
    wrappedKey.len  = SECKEY_PublicKeyStrength(svrPubKey);
    wrappedKey.data = wswk.wrappedSymmetricWrappingkey;

    PORT_Assert(wrappedKey.len <= sizeof wswk.wrappedSymmetricWrappingkey);
    if (wrappedKey.len > sizeof wswk.wrappedSymmetricWrappingkey)
    	goto loser;

    /* wrap symmetric wrapping key in server's public key. */
    switch (exchKeyType) {
#ifdef NSS_ENABLE_ECC
    PK11SymKey *      Ks;
    SECKEYPublicKey   *pubWrapKey = NULL;
    SECKEYPrivateKey  *privWrapKey = NULL;
    ECCWrappedKeyInfo *ecWrapped;
#endif /* NSS_ENABLE_ECC */

    case kt_rsa:
	asymWrapMechanism = CKM_RSA_PKCS;
	rv = PK11_PubWrapSymKey(asymWrapMechanism, svrPubKey,
	                        unwrappedWrappingKey, &wrappedKey);
	break;

#ifdef NSS_ENABLE_ECC
    case kt_ecdh:
	/*
	 * We generate an ephemeral EC key pair. Perform an ECDH
	 * computation involving this ephemeral EC public key and
	 * the SSL server's (long-term) EC private key. The resulting
	 * shared secret is treated in the same way as Fortezza's Ks, 
	 * i.e., it is used to wrap the wrapping key. To facilitate
	 * unwrapping in ssl_UnwrapWrappingKey, we also store all
	 * relevant info about the ephemeral EC public key in
	 * wswk.wrappedSymmetricWrappingkey and lay it out as 
	 * described in the ECCWrappedKeyInfo structure.
	 */
	PORT_Assert(svrPubKey->keyType == ecKey);
	if (svrPubKey->keyType != ecKey) {
	    /* something is wrong in sslsecur.c if this isn't an ecKey */
	    PORT_SetError(SEC_ERROR_LIBRARY_FAILURE);
	    rv = SECFailure;
	    goto ec_cleanup;
	}

	privWrapKey = SECKEY_CreateECPrivateKey(
	    &svrPubKey->u.ec.DEREncodedParams, &pubWrapKey, NULL);
	if ((privWrapKey == NULL) || (pubWrapKey == NULL)) {
	    rv = SECFailure;
	    goto ec_cleanup;
	}
	
	/* Set the key size in bits */
	if (pubWrapKey->u.ec.size == 0) {
	    pubWrapKey->u.ec.size = SECKEY_PublicKeyStrengthInBits(svrPubKey);
	}

	PORT_Assert(pubWrapKey->u.ec.DEREncodedParams.len + 
	    pubWrapKey->u.ec.publicValue.len < MAX_EC_WRAPPED_KEY_BUFLEN);
	if (pubWrapKey->u.ec.DEREncodedParams.len + 
	    pubWrapKey->u.ec.publicValue.len >= MAX_EC_WRAPPED_KEY_BUFLEN) {
	    PORT_SetError(SEC_ERROR_INVALID_KEY);
	    rv = SECFailure;
	    goto ec_cleanup;
	}

	/* Derive Ks using ECDH */
	Ks = PK11_PubDeriveWithKDF(svrPrivKey, pubWrapKey, PR_FALSE, NULL,
				   NULL, CKM_ECDH1_DERIVE, masterWrapMech, 
				   CKA_DERIVE, 0, CKD_NULL, NULL, NULL);
	if (Ks == NULL) {
	    rv = SECFailure;
	    goto ec_cleanup;
	}

	ecWrapped = (ECCWrappedKeyInfo *) (wswk.wrappedSymmetricWrappingkey);
	ecWrapped->size = pubWrapKey->u.ec.size;
	ecWrapped->encodedParamLen = pubWrapKey->u.ec.DEREncodedParams.len;
	PORT_Memcpy(ecWrapped->var, pubWrapKey->u.ec.DEREncodedParams.data, 
	    pubWrapKey->u.ec.DEREncodedParams.len);

	ecWrapped->pubValueLen = pubWrapKey->u.ec.publicValue.len;
	PORT_Memcpy(ecWrapped->var + ecWrapped->encodedParamLen, 
		    pubWrapKey->u.ec.publicValue.data, 
		    pubWrapKey->u.ec.publicValue.len);

	wrappedKey.len = MAX_EC_WRAPPED_KEY_BUFLEN - 
	    (ecWrapped->encodedParamLen + ecWrapped->pubValueLen);
	wrappedKey.data = ecWrapped->var + ecWrapped->encodedParamLen +
	    ecWrapped->pubValueLen;

	/* wrap symmetricWrapping key with the local Ks */
	rv = PK11_WrapSymKey(masterWrapMech, NULL, Ks,
			     unwrappedWrappingKey, &wrappedKey);

	if (rv != SECSuccess) {
	    goto ec_cleanup;
	}

	/* Write down the length of wrapped key in the buffer
	 * wswk.wrappedSymmetricWrappingkey at the appropriate offset
	 */
	ecWrapped->wrappedKeyLen = wrappedKey.len;

ec_cleanup:
	if (privWrapKey) SECKEY_DestroyPrivateKey(privWrapKey);
	if (pubWrapKey) SECKEY_DestroyPublicKey(pubWrapKey);
	if (Ks) PK11_FreeSymKey(Ks);
	asymWrapMechanism = masterWrapMech;
	break;
#endif /* NSS_ENABLE_ECC */

    default:
	rv = SECFailure;
	break;
    }

    if (rv != SECSuccess) {
	ssl_MapLowLevelError(SSL_ERROR_CLIENT_KEY_EXCHANGE_FAILURE);
	goto loser;
    }

    PORT_Assert(asymWrapMechanism != CKM_INVALID_MECHANISM);

    wswk.symWrapMechanism  = masterWrapMech;
    wswk.symWrapMechIndex  = symWrapMechIndex;
    wswk.asymWrapMechanism = asymWrapMechanism;
    wswk.exchKeyType       = exchKeyType;
    wswk.wrappedSymKeyLen  = wrappedKey.len;

    /* put it on disk. */
    /* If the wrapping key for this KEA type has already been set, 
     * then abandon the value we just computed and 
     * use the one we got from the disk.
     */
    if (ssl_SetWrappingKey(&wswk)) {
    	/* somebody beat us to it.  The original contents of our wswk
	 * has been replaced with the content on disk.  Now, discard
	 * the key we just created and unwrap this new one.
	 */
    	PK11_FreeSymKey(unwrappedWrappingKey);

	unwrappedWrappingKey =
	    ssl_UnwrapSymWrappingKey(&wswk, svrPrivKey, exchKeyType,
                                     masterWrapMech, pwArg);
    }

install:
    if (unwrappedWrappingKey) {
	*pSymWrapKey = PK11_ReferenceSymKey(unwrappedWrappingKey);
    }

loser:
done:
    PZ_Unlock(symWrapKeysLock);
    return unwrappedWrappingKey;
}


/* Called from ssl3_SendClientKeyExchange(). */
/* Presently, this always uses PKCS11.  There is no bypass for this. */
static SECStatus
sendRSAClientKeyExchange(sslSocket * ss, SECKEYPublicKey * svrPubKey)
{
    PK11SymKey *	pms 		= NULL;
    SECStatus           rv    		= SECFailure;
    SECItem 		enc_pms 	= {siBuffer, NULL, 0};
    PRBool              isTLS;

    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) );
    PORT_Assert( ss->opt.noLocks || ssl_HaveXmitBufLock(ss));

    /* Generate the pre-master secret ...  */
    ssl_GetSpecWriteLock(ss);
    isTLS = (PRBool)(ss->ssl3.pwSpec->version > SSL_LIBRARY_VERSION_3_0);

    pms = ssl3_GenerateRSAPMS(ss, ss->ssl3.pwSpec, NULL);
    ssl_ReleaseSpecWriteLock(ss);
    if (pms == NULL) {
	ssl_MapLowLevelError(SSL_ERROR_CLIENT_KEY_EXCHANGE_FAILURE);
	goto loser;
    }

#if defined(TRACE)
    if (ssl_trace >= 100) {
	SECStatus extractRV = PK11_ExtractKeyValue(pms);
	if (extractRV == SECSuccess) {
	    SECItem * keyData = PK11_GetKeyData(pms);
	    if (keyData && keyData->data && keyData->len) {
		ssl_PrintBuf(ss, "Pre-Master Secret", 
			     keyData->data, keyData->len);
	    }
    	}
    }
#endif

    /* Get the wrapped (encrypted) pre-master secret, enc_pms */
    enc_pms.len  = SECKEY_PublicKeyStrength(svrPubKey);
    enc_pms.data = (unsigned char*)PORT_Alloc(enc_pms.len);
    if (enc_pms.data == NULL) {
	goto loser;	/* err set by PORT_Alloc */
    }

    /* wrap pre-master secret in server's public key. */
    rv = PK11_PubWrapSymKey(CKM_RSA_PKCS, svrPubKey, pms, &enc_pms);
    if (rv != SECSuccess) {
	ssl_MapLowLevelError(SSL_ERROR_CLIENT_KEY_EXCHANGE_FAILURE);
	goto loser;
    }

    rv = ssl3_InitPendingCipherSpec(ss,  pms);
    PK11_FreeSymKey(pms); pms = NULL;

    if (rv != SECSuccess) {
	ssl_MapLowLevelError(SSL_ERROR_CLIENT_KEY_EXCHANGE_FAILURE);
	goto loser;
    }

    rv = ssl3_AppendHandshakeHeader(ss, client_key_exchange, 
				    isTLS ? enc_pms.len + 2 : enc_pms.len);
    if (rv != SECSuccess) {
	goto loser;	/* err set by ssl3_AppendHandshake* */
    }
    if (isTLS) {
    	rv = ssl3_AppendHandshakeVariable(ss, enc_pms.data, enc_pms.len, 2);
    } else {
	rv = ssl3_AppendHandshake(ss, enc_pms.data, enc_pms.len);
    }
    if (rv != SECSuccess) {
	goto loser;	/* err set by ssl3_AppendHandshake* */
    }

    rv = SECSuccess;

loser:
    if (enc_pms.data != NULL) {
	PORT_Free(enc_pms.data);
    }
    if (pms != NULL) {
    	PK11_FreeSymKey(pms);
    }
    return rv;
}

/* Called from ssl3_SendClientKeyExchange(). */
/* Presently, this always uses PKCS11.  There is no bypass for this. */
static SECStatus
sendDHClientKeyExchange(sslSocket * ss, SECKEYPublicKey * svrPubKey)
{
    PK11SymKey *	pms 		= NULL;
    SECStatus           rv    		= SECFailure;
    PRBool              isTLS;
    CK_MECHANISM_TYPE	target;

    SECKEYDHParams	dhParam;		/* DH parameters */
    SECKEYPublicKey	*pubKey = NULL;		/* Ephemeral DH key */
    SECKEYPrivateKey	*privKey = NULL;	/* Ephemeral DH key */

    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) );
    PORT_Assert( ss->opt.noLocks || ssl_HaveXmitBufLock(ss));

    isTLS = (PRBool)(ss->ssl3.pwSpec->version > SSL_LIBRARY_VERSION_3_0);

    /* Copy DH parameters from server key */

    if (svrPubKey->keyType != dhKey) {
	PORT_SetError(SEC_ERROR_BAD_KEY);
	goto loser;
    }
    dhParam.prime.data = svrPubKey->u.dh.prime.data;
    dhParam.prime.len = svrPubKey->u.dh.prime.len;
    dhParam.base.data = svrPubKey->u.dh.base.data;
    dhParam.base.len = svrPubKey->u.dh.base.len;

    /* Generate ephemeral DH keypair */
    privKey = SECKEY_CreateDHPrivateKey(&dhParam, &pubKey, NULL);
    if (!privKey || !pubKey) {
	    ssl_MapLowLevelError(SEC_ERROR_KEYGEN_FAIL);
	    rv = SECFailure;
	    goto loser;
    }
    PRINT_BUF(50, (ss, "DH public value:",
					pubKey->u.dh.publicValue.data,
					pubKey->u.dh.publicValue.len));

    if (isTLS) target = CKM_TLS_MASTER_KEY_DERIVE_DH;
    else target = CKM_SSL3_MASTER_KEY_DERIVE_DH;

    /* Determine the PMS */

    pms = PK11_PubDerive(privKey, svrPubKey, PR_FALSE, NULL, NULL,
			    CKM_DH_PKCS_DERIVE, target, CKA_DERIVE, 0, NULL);

    if (pms == NULL) {
	ssl_MapLowLevelError(SSL_ERROR_CLIENT_KEY_EXCHANGE_FAILURE);
	goto loser;
    }

    SECKEY_DestroyPrivateKey(privKey);
    privKey = NULL;

    rv = ssl3_InitPendingCipherSpec(ss,  pms);
    PK11_FreeSymKey(pms); pms = NULL;

    if (rv != SECSuccess) {
	ssl_MapLowLevelError(SSL_ERROR_CLIENT_KEY_EXCHANGE_FAILURE);
	goto loser;
    }

    rv = ssl3_AppendHandshakeHeader(ss, client_key_exchange, 
					pubKey->u.dh.publicValue.len + 2);
    if (rv != SECSuccess) {
	goto loser;	/* err set by ssl3_AppendHandshake* */
    }
    rv = ssl3_AppendHandshakeVariable(ss, 
					pubKey->u.dh.publicValue.data,
					pubKey->u.dh.publicValue.len, 2);
    SECKEY_DestroyPublicKey(pubKey);
    pubKey = NULL;

    if (rv != SECSuccess) {
	goto loser;	/* err set by ssl3_AppendHandshake* */
    }

    rv = SECSuccess;


loser:

    if(pms) PK11_FreeSymKey(pms);
    if(privKey) SECKEY_DestroyPrivateKey(privKey);
    if(pubKey) SECKEY_DestroyPublicKey(pubKey);
    return rv;
}





/* Called from ssl3_HandleServerHelloDone(). */
static SECStatus
ssl3_SendClientKeyExchange(sslSocket *ss)
{
    SECKEYPublicKey *	serverKey 	= NULL;
    SECStatus 		rv 		= SECFailure;
    PRBool              isTLS;

    SSL_TRC(3, ("%d: SSL3[%d]: send client_key_exchange handshake",
		SSL_GETPID(), ss->fd));

    PORT_Assert( ss->opt.noLocks || ssl_HaveXmitBufLock(ss));
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss));

    if (ss->sec.peerKey == NULL) {
	serverKey = CERT_ExtractPublicKey(ss->sec.peerCert);
	if (serverKey == NULL) {
	    PORT_SetError(SSL_ERROR_EXTRACT_PUBLIC_KEY_FAILURE);
	    return SECFailure;
	}
    } else {
	serverKey = ss->sec.peerKey;
	ss->sec.peerKey = NULL; /* we're done with it now */
    }

    isTLS = (PRBool)(ss->ssl3.pwSpec->version > SSL_LIBRARY_VERSION_3_0);
    /* enforce limits on kea key sizes. */
    if (ss->ssl3.hs.kea_def->is_limited) {
	int keyLen = SECKEY_PublicKeyStrength(serverKey);	/* bytes */

	if (keyLen * BPB > ss->ssl3.hs.kea_def->key_size_limit) {
	    if (isTLS)
		(void)SSL3_SendAlert(ss, alert_fatal, export_restriction);
	    else
		(void)ssl3_HandshakeFailure(ss);
	    PORT_SetError(SSL_ERROR_PUB_KEY_SIZE_LIMIT_EXCEEDED);
	    goto loser;
	}
    }

    ss->sec.keaType    = ss->ssl3.hs.kea_def->exchKeyType;
    ss->sec.keaKeyBits = SECKEY_PublicKeyStrengthInBits(serverKey);

    switch (ss->ssl3.hs.kea_def->exchKeyType) {
    case kt_rsa:
	rv = sendRSAClientKeyExchange(ss, serverKey);
	break;

    case kt_dh:
	rv = sendDHClientKeyExchange(ss, serverKey);
	break;

#ifdef NSS_ENABLE_ECC
    case kt_ecdh:
	rv = ssl3_SendECDHClientKeyExchange(ss, serverKey);
	break;
#endif /* NSS_ENABLE_ECC */

    default:
	/* got an unknown or unsupported Key Exchange Algorithm.  */
	SEND_ALERT
	PORT_SetError(SEC_ERROR_UNSUPPORTED_KEYALG);
	break;
    }

    SSL_TRC(3, ("%d: SSL3[%d]: DONE sending client_key_exchange",
		SSL_GETPID(), ss->fd));

loser:
    if (serverKey) 
    	SECKEY_DestroyPublicKey(serverKey);
    return rv;	/* err code already set. */
}

/* Called from ssl3_HandleServerHelloDone(). */
static SECStatus
ssl3_SendCertificateVerify(sslSocket *ss)
{
    SECStatus     rv		= SECFailure;
    PRBool        isTLS;
    SECItem       buf           = {siBuffer, NULL, 0};
    SSL3Hashes    hashes;

    PORT_Assert( ss->opt.noLocks || ssl_HaveXmitBufLock(ss));
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss));

    SSL_TRC(3, ("%d: SSL3[%d]: send certificate_verify handshake",
		SSL_GETPID(), ss->fd));

    ssl_GetSpecReadLock(ss);
    rv = ssl3_ComputeHandshakeHashes(ss, ss->ssl3.pwSpec, &hashes, 0);
    ssl_ReleaseSpecReadLock(ss);
    if (rv != SECSuccess) {
	goto done;	/* err code was set by ssl3_ComputeHandshakeHashes */
    }

    isTLS = (PRBool)(ss->ssl3.pwSpec->version > SSL_LIBRARY_VERSION_3_0);
    rv = ssl3_SignHashes(&hashes, ss->ssl3.clientPrivateKey, &buf, isTLS);
    if (rv == SECSuccess) {
	PK11SlotInfo * slot;
	sslSessionID * sid   = ss->sec.ci.sid;

    	/* Remember the info about the slot that did the signing.
	** Later, when doing an SSL restart handshake, verify this.
	** These calls are mere accessors, and can't fail.
	*/
	slot = PK11_GetSlotFromPrivateKey(ss->ssl3.clientPrivateKey);
	sid->u.ssl3.clAuthSeries     = PK11_GetSlotSeries(slot);
	sid->u.ssl3.clAuthSlotID     = PK11_GetSlotID(slot);
	sid->u.ssl3.clAuthModuleID   = PK11_GetModuleID(slot);
	sid->u.ssl3.clAuthValid      = PR_TRUE;
	PK11_FreeSlot(slot);
    }
    /* If we're doing RSA key exchange, we're all done with the private key
     * here.  Diffie-Hellman key exchanges need the client's
     * private key for the key exchange.
     */
    if (ss->ssl3.hs.kea_def->exchKeyType == kt_rsa) {
	SECKEY_DestroyPrivateKey(ss->ssl3.clientPrivateKey);
	ss->ssl3.clientPrivateKey = NULL;
    }
    if (rv != SECSuccess) {
	goto done;	/* err code was set by ssl3_SignHashes */
    }

    rv = ssl3_AppendHandshakeHeader(ss, certificate_verify, buf.len + 2);
    if (rv != SECSuccess) {
	goto done;	/* error code set by AppendHandshake */
    }
    rv = ssl3_AppendHandshakeVariable(ss, buf.data, buf.len, 2);
    if (rv != SECSuccess) {
	goto done;	/* error code set by AppendHandshake */
    }

done:
    if (buf.data)
	PORT_Free(buf.data);
    return rv;
}

/* Called from ssl3_HandleHandshakeMessage() when it has deciphered a complete
 * ssl3 ServerHello message.
 * Caller must hold Handshake and RecvBuf locks.
 */
static SECStatus
ssl3_HandleServerHello(sslSocket *ss, SSL3Opaque *b, PRUint32 length)
{
    sslSessionID *sid		= ss->sec.ci.sid;
    PRInt32       temp;		/* allow for consume number failure */
    PRBool        suite_found   = PR_FALSE;
    int           i;
    int           errCode	= SSL_ERROR_RX_MALFORMED_SERVER_HELLO;
    SECStatus     rv;
    SECItem       sidBytes 	= {siBuffer, NULL, 0};
    PRBool        sid_match;
    PRBool        isTLS		= PR_FALSE;
    SSL3AlertDescription desc   = illegal_parameter;
    SSL3ProtocolVersion version;

    SSL_TRC(3, ("%d: SSL3[%d]: handle server_hello handshake",
    	SSL_GETPID(), ss->fd));
    PORT_Assert( ss->opt.noLocks || ssl_HaveRecvBufLock(ss) );
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) );

    rv = ssl3_InitState(ss);
    if (rv != SECSuccess) {
	errCode = PORT_GetError(); /* ssl3_InitState has set the error code. */
	goto alert_loser;
    }
    if (ss->ssl3.hs.ws != wait_server_hello) {
        errCode = SSL_ERROR_RX_UNEXPECTED_SERVER_HELLO;
	desc    = unexpected_message;
	goto alert_loser;
    }

    temp = ssl3_ConsumeHandshakeNumber(ss, 2, &b, &length);
    if (temp < 0) {
    	goto loser; 	/* alert has been sent */
    }
    version = (SSL3ProtocolVersion)temp;

    /* this is appropriate since the negotiation is complete, and we only
    ** know SSL 3.x.
    */
    if (MSB(version) != MSB(SSL_LIBRARY_VERSION_3_0)) {
    	desc = (version > SSL_LIBRARY_VERSION_3_0) ? protocol_version 
						   : handshake_failure;
	goto alert_loser;
    }

    rv = ssl3_NegotiateVersion(ss, version);
    if (rv != SECSuccess) {
    	desc = (version > SSL_LIBRARY_VERSION_3_0) ? protocol_version 
						   : handshake_failure;
	errCode = SSL_ERROR_NO_CYPHER_OVERLAP;
	goto alert_loser;
    }
    isTLS = (ss->version > SSL_LIBRARY_VERSION_3_0);

    rv = ssl3_ConsumeHandshake(
	ss, &ss->ssl3.hs.server_random, SSL3_RANDOM_LENGTH, &b, &length);
    if (rv != SECSuccess) {
    	goto loser; 	/* alert has been sent */
    }

    rv = ssl3_ConsumeHandshakeVariable(ss, &sidBytes, 1, &b, &length);
    if (rv != SECSuccess) {
    	goto loser; 	/* alert has been sent */
    }
    if (sidBytes.len > SSL3_SESSIONID_BYTES) {
	if (isTLS)
	    desc = decode_error;
	goto alert_loser;	/* malformed. */
    }

    /* find selected cipher suite in our list. */
    temp = ssl3_ConsumeHandshakeNumber(ss, 2, &b, &length);
    if (temp < 0) {
    	goto loser; 	/* alert has been sent */
    }
    ssl3_config_match_init(ss);
    for (i = 0; i < ssl_V3_SUITES_IMPLEMENTED; i++) {
	ssl3CipherSuiteCfg *suite = &ss->cipherSuites[i];
	if ((temp == suite->cipher_suite) &&
	    (config_match(suite, ss->ssl3.policy, PR_TRUE))) {
	    suite_found = PR_TRUE;
	    break;	/* success */
	}
    }
    if (!suite_found) {
    	desc    = handshake_failure;
	errCode = SSL_ERROR_NO_CYPHER_OVERLAP;
	goto alert_loser;
    }
    ss->ssl3.hs.cipher_suite = (ssl3CipherSuite)temp;
    ss->ssl3.hs.suite_def    = ssl_LookupCipherSuiteDef((ssl3CipherSuite)temp);
    PORT_Assert(ss->ssl3.hs.suite_def);
    if (!ss->ssl3.hs.suite_def) {
    	PORT_SetError(errCode = SEC_ERROR_LIBRARY_FAILURE);
	goto loser;	/* we don't send alerts for our screw-ups. */
    }

    /* find selected compression method in our list. */
    temp = ssl3_ConsumeHandshakeNumber(ss, 1, &b, &length);
    if (temp < 0) {
    	goto loser; 	/* alert has been sent */
    }
    suite_found = PR_FALSE;
    for (i = 0; i < compressionMethodsCount; i++) {
	if (temp == compressions[i])  {
	    suite_found = PR_TRUE;
	    break;	/* success */
    	}
    }
    if (!suite_found) {
    	desc    = handshake_failure;
	errCode = SSL_ERROR_NO_COMPRESSION_OVERLAP;
	goto alert_loser;
    }
    ss->ssl3.hs.compression = (SSL3CompressionMethod)temp;

#ifdef DISALLOW_SERVER_HELLO_EXTENSIONS
    if (length != 0) {	/* malformed */
	goto alert_loser;
    }
#endif

    /* Any errors after this point are not "malformed" errors. */
    desc    = handshake_failure;

    /* we need to call ssl3_SetupPendingCipherSpec here so we can check the
     * key exchange algorithm. */
    rv = ssl3_SetupPendingCipherSpec(ss);
    if (rv != SECSuccess) {
	goto alert_loser;	/* error code is set. */
    }

    /* We may or may not have sent a session id, we may get one back or
     * not and if so it may match the one we sent.
     * Attempt to restore the master secret to see if this is so...
     * Don't consider failure to find a matching SID an error.
     */
    sid_match = (PRBool)(sidBytes.len > 0 &&
	sidBytes.len == sid->u.ssl3.sessionIDLength &&
	!PORT_Memcmp(sid->u.ssl3.sessionID, sidBytes.data, sidBytes.len));

    if (sid_match &&
	sid->version == ss->version &&
	sid->u.ssl3.cipherSuite == ss->ssl3.hs.cipher_suite) do {
	ssl3CipherSpec *pwSpec = ss->ssl3.pwSpec;

	SECItem       wrappedMS;   /* wrapped master secret. */

	ss->sec.authAlgorithm = sid->authAlgorithm;
	ss->sec.authKeyBits   = sid->authKeyBits;
	ss->sec.keaType       = sid->keaType;
	ss->sec.keaKeyBits    = sid->keaKeyBits;

	/* 3 cases here:
	 * a) key is wrapped (implies using PKCS11)
	 * b) key is unwrapped, but we're still using PKCS11
	 * c) key is unwrapped, and we're bypassing PKCS11.
	 */
	if (sid->u.ssl3.keys.msIsWrapped) {
	    PK11SlotInfo *slot;
	    PK11SymKey *  wrapKey;     /* wrapping key */
	    CK_FLAGS      keyFlags      = 0;

	    if (ss->opt.bypassPKCS11) {
		/* we cannot restart a non-bypass session in a 
		** bypass socket.
		*/
		break;  
	    }
	    /* unwrap master secret with PKCS11 */
	    slot = SECMOD_LookupSlot(sid->u.ssl3.masterModuleID,
				     sid->u.ssl3.masterSlotID);
	    if (slot == NULL) {
		break;		/* not considered an error. */
	    }
	    if (!PK11_IsPresent(slot)) {
		PK11_FreeSlot(slot);
		break;		/* not considered an error. */
	    }
	    wrapKey = PK11_GetWrapKey(slot, sid->u.ssl3.masterWrapIndex,
				      sid->u.ssl3.masterWrapMech,
				      sid->u.ssl3.masterWrapSeries,
				      ss->pkcs11PinArg);
	    PK11_FreeSlot(slot);
	    if (wrapKey == NULL) {
		break;		/* not considered an error. */
	    }

	    if (ss->version > SSL_LIBRARY_VERSION_3_0) {	/* isTLS */
		keyFlags = CKF_SIGN | CKF_VERIFY;
	    }

	    wrappedMS.data = sid->u.ssl3.keys.wrapped_master_secret;
	    wrappedMS.len  = sid->u.ssl3.keys.wrapped_master_secret_len;
	    pwSpec->master_secret =
		PK11_UnwrapSymKeyWithFlags(wrapKey, sid->u.ssl3.masterWrapMech, 
			    NULL, &wrappedMS, CKM_SSL3_MASTER_KEY_DERIVE,
			    CKA_DERIVE, sizeof(SSL3MasterSecret), keyFlags);
	    errCode = PORT_GetError();
	    PK11_FreeSymKey(wrapKey);
	    if (pwSpec->master_secret == NULL) {
		break;	/* errorCode set just after call to UnwrapSymKey. */
	    }
	} else if (ss->opt.bypassPKCS11) {
	    /* MS is not wrapped */
	    wrappedMS.data = sid->u.ssl3.keys.wrapped_master_secret;
	    wrappedMS.len  = sid->u.ssl3.keys.wrapped_master_secret_len;
	    memcpy(pwSpec->raw_master_secret, wrappedMS.data, wrappedMS.len);
	    pwSpec->msItem.data = pwSpec->raw_master_secret;
	    pwSpec->msItem.len  = wrappedMS.len;
	} else {
	    /* We CAN restart a bypass session in a non-bypass socket. */
	    /* need to import the raw master secret to session object */
	    PK11SlotInfo *slot = PK11_GetInternalSlot();
	    wrappedMS.data = sid->u.ssl3.keys.wrapped_master_secret;
	    wrappedMS.len  = sid->u.ssl3.keys.wrapped_master_secret_len;
	    pwSpec->master_secret =  
		PK11_ImportSymKey(slot, CKM_SSL3_MASTER_KEY_DERIVE, 
				  PK11_OriginUnwrap, CKA_ENCRYPT, 
				  &wrappedMS, NULL);
	    PK11_FreeSlot(slot);
	    if (pwSpec->master_secret == NULL) {
		break; 
	    }
	}

	/* Got a Match */
	SSL_AtomicIncrementLong(& ssl3stats.hsh_sid_cache_hits );
	ss->ssl3.hs.ws         = wait_change_cipher;
	ss->ssl3.hs.isResuming = PR_TRUE;

	/* copy the peer cert from the SID */
	if (sid->peerCert != NULL) {
	    ss->sec.peerCert = CERT_DupCertificate(sid->peerCert);
	}


	/* NULL value for PMS signifies re-use of the old MS */
	rv = ssl3_InitPendingCipherSpec(ss,  NULL);
	if (rv != SECSuccess) {
	    goto alert_loser;	/* err code was set */
	}
	return SECSuccess;
    } while (0);

    if (sid_match)
	SSL_AtomicIncrementLong(& ssl3stats.hsh_sid_cache_not_ok );
    else
	SSL_AtomicIncrementLong(& ssl3stats.hsh_sid_cache_misses );

    /* throw the old one away */
    sid->u.ssl3.keys.resumable = PR_FALSE;
    (*ss->sec.uncache)(sid);
    ssl_FreeSID(sid);

    /* get a new sid */
    ss->sec.ci.sid = sid = ssl3_NewSessionID(ss, PR_FALSE);
    if (sid == NULL) {
	goto alert_loser;	/* memory error is set. */
    }

    sid->version = ss->version;
    sid->u.ssl3.sessionIDLength = sidBytes.len;
    PORT_Memcpy(sid->u.ssl3.sessionID, sidBytes.data, sidBytes.len);

    ss->ssl3.hs.isResuming = PR_FALSE;
    ss->ssl3.hs.ws         = wait_server_cert;
    return SECSuccess;

alert_loser:
    (void)SSL3_SendAlert(ss, alert_fatal, desc);

loser:
    errCode = ssl_MapLowLevelError(errCode);
    return SECFailure;
}

/* Called from ssl3_HandleHandshakeMessage() when it has deciphered a complete
 * ssl3 ServerKeyExchange message.
 * Caller must hold Handshake and RecvBuf locks.
 */
static SECStatus
ssl3_HandleServerKeyExchange(sslSocket *ss, SSL3Opaque *b, PRUint32 length)
{
    PRArenaPool *    arena     = NULL;
    SECKEYPublicKey *peerKey   = NULL;
    PRBool           isTLS;
    SECStatus        rv;
    int              errCode   = SSL_ERROR_RX_MALFORMED_SERVER_KEY_EXCH;
    SSL3AlertDescription desc  = illegal_parameter;
    SSL3Hashes       hashes;
    SECItem          signature = {siBuffer, NULL, 0};

    SSL_TRC(3, ("%d: SSL3[%d]: handle server_key_exchange handshake",
		SSL_GETPID(), ss->fd));
    PORT_Assert( ss->opt.noLocks || ssl_HaveRecvBufLock(ss) );
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) );

    if (ss->ssl3.hs.ws != wait_server_key &&
	ss->ssl3.hs.ws != wait_server_cert) {
	errCode = SSL_ERROR_RX_UNEXPECTED_SERVER_KEY_EXCH;
	desc    = unexpected_message;
	goto alert_loser;
    }
    if (ss->sec.peerCert == NULL) {
	errCode = SSL_ERROR_RX_UNEXPECTED_SERVER_KEY_EXCH;
	desc    = unexpected_message;
	goto alert_loser;
    }

    isTLS = (PRBool)(ss->ssl3.prSpec->version > SSL_LIBRARY_VERSION_3_0);

    switch (ss->ssl3.hs.kea_def->exchKeyType) {

    case kt_rsa: {
	SECItem          modulus   = {siBuffer, NULL, 0};
	SECItem          exponent  = {siBuffer, NULL, 0};

    	rv = ssl3_ConsumeHandshakeVariable(ss, &modulus, 2, &b, &length);
    	if (rv != SECSuccess) {
	    goto loser;		/* malformed. */
	}
    	rv = ssl3_ConsumeHandshakeVariable(ss, &exponent, 2, &b, &length);
    	if (rv != SECSuccess) {
	    goto loser;		/* malformed. */
	}
    	rv = ssl3_ConsumeHandshakeVariable(ss, &signature, 2, &b, &length);
    	if (rv != SECSuccess) {
	    goto loser;		/* malformed. */
	}
    	if (length != 0) {
	    if (isTLS)
		desc = decode_error;
	    goto alert_loser;		/* malformed. */
	}

	/* failures after this point are not malformed handshakes. */
	/* TLS: send decrypt_error if signature failed. */
    	desc = isTLS ? decrypt_error : handshake_failure;

    	/*
     	 *  check to make sure the hash is signed by right guy
     	 */
    	rv = ssl3_ComputeExportRSAKeyHash(modulus, exponent,
					  &ss->ssl3.hs.client_random,
					  &ss->ssl3.hs.server_random, 
					  &hashes, ss->opt.bypassPKCS11);
        if (rv != SECSuccess) {
	    errCode =
	    	ssl_MapLowLevelError(SSL_ERROR_SERVER_KEY_EXCHANGE_FAILURE);
	    goto alert_loser;
	}
        rv = ssl3_VerifySignedHashes(&hashes, ss->sec.peerCert, &signature,
				    isTLS, ss->pkcs11PinArg);
	if (rv != SECSuccess)  {
	    errCode =
	    	ssl_MapLowLevelError(SSL_ERROR_SERVER_KEY_EXCHANGE_FAILURE);
	    goto alert_loser;
	}

	/*
	 * we really need to build a new key here because we can no longer
	 * ignore calling SECKEY_DestroyPublicKey. Using the key may allocate
	 * pkcs11 slots and ID's.
	 */
    	arena = PORT_NewArena(DER_DEFAULT_CHUNKSIZE);
	if (arena == NULL) {
	    goto no_memory;
	}

    	peerKey = PORT_ArenaZNew(arena, SECKEYPublicKey);
    	if (peerKey == NULL) {
            PORT_FreeArena(arena, PR_FALSE);
	    goto no_memory;
	}

	peerKey->arena              = arena;
	peerKey->keyType            = rsaKey;
	peerKey->pkcs11Slot         = NULL;
	peerKey->pkcs11ID           = CK_INVALID_HANDLE;
	if (SECITEM_CopyItem(arena, &peerKey->u.rsa.modulus,        &modulus) ||
	    SECITEM_CopyItem(arena, &peerKey->u.rsa.publicExponent, &exponent))
	{
            PORT_FreeArena(arena, PR_FALSE);
	    goto no_memory;
        }
    	ss->sec.peerKey = peerKey;
    	ss->ssl3.hs.ws = wait_cert_request;
    	return SECSuccess;
    }

    case kt_dh: {
	SECItem          dh_p      = {siBuffer, NULL, 0};
	SECItem          dh_g      = {siBuffer, NULL, 0};
	SECItem          dh_Ys     = {siBuffer, NULL, 0};

    	rv = ssl3_ConsumeHandshakeVariable(ss, &dh_p, 2, &b, &length);
    	if (rv != SECSuccess) {
	    goto loser;		/* malformed. */
	}
    	rv = ssl3_ConsumeHandshakeVariable(ss, &dh_g, 2, &b, &length);
    	if (rv != SECSuccess) {
	    goto loser;		/* malformed. */
	}
    	rv = ssl3_ConsumeHandshakeVariable(ss, &dh_Ys, 2, &b, &length);
    	if (rv != SECSuccess) {
	    goto loser;		/* malformed. */
	}
    	rv = ssl3_ConsumeHandshakeVariable(ss, &signature, 2, &b, &length);
    	if (rv != SECSuccess) {
	    goto loser;		/* malformed. */
	}
    	if (length != 0) {
	    if (isTLS)
		desc = decode_error;
	    goto alert_loser;		/* malformed. */
	}

	PRINT_BUF(60, (NULL, "Server DH p", dh_p.data, dh_p.len));
	PRINT_BUF(60, (NULL, "Server DH g", dh_g.data, dh_g.len));
	PRINT_BUF(60, (NULL, "Server DH Ys", dh_Ys.data, dh_Ys.len));

	/* failures after this point are not malformed handshakes. */
	/* TLS: send decrypt_error if signature failed. */
    	desc = isTLS ? decrypt_error : handshake_failure;

    	/*
     	 *  check to make sure the hash is signed by right guy
     	 */
    	rv = ssl3_ComputeDHKeyHash(dh_p, dh_g, dh_Ys,
					  &ss->ssl3.hs.client_random,
					  &ss->ssl3.hs.server_random, 
					  &hashes, ss->opt.bypassPKCS11);
        if (rv != SECSuccess) {
	    errCode =
	    	ssl_MapLowLevelError(SSL_ERROR_SERVER_KEY_EXCHANGE_FAILURE);
	    goto alert_loser;
	}
        rv = ssl3_VerifySignedHashes(&hashes, ss->sec.peerCert, &signature,
				    isTLS, ss->pkcs11PinArg);
	if (rv != SECSuccess)  {
	    errCode =
	    	ssl_MapLowLevelError(SSL_ERROR_SERVER_KEY_EXCHANGE_FAILURE);
	    goto alert_loser;
	}

	/*
	 * we really need to build a new key here because we can no longer
	 * ignore calling SECKEY_DestroyPublicKey. Using the key may allocate
	 * pkcs11 slots and ID's.
	 */
    	arena = PORT_NewArena(DER_DEFAULT_CHUNKSIZE);
	if (arena == NULL) {
	    goto no_memory;
	}

    	ss->sec.peerKey = peerKey = PORT_ArenaZNew(arena, SECKEYPublicKey);
    	if (peerKey == NULL) {
	    goto no_memory;
	}

	peerKey->arena              = arena;
	peerKey->keyType            = dhKey;
	peerKey->pkcs11Slot         = NULL;
	peerKey->pkcs11ID           = CK_INVALID_HANDLE;

	if (SECITEM_CopyItem(arena, &peerKey->u.dh.prime,        &dh_p) ||
	    SECITEM_CopyItem(arena, &peerKey->u.dh.base,         &dh_g) ||
	    SECITEM_CopyItem(arena, &peerKey->u.dh.publicValue,  &dh_Ys))
	{
            PORT_FreeArena(arena, PR_FALSE);
	    goto no_memory;
        }
    	ss->sec.peerKey = peerKey;
    	ss->ssl3.hs.ws = wait_cert_request;
    	return SECSuccess;
    }

#ifdef NSS_ENABLE_ECC
    case kt_ecdh:
	rv = ssl3_HandleECDHServerKeyExchange(ss, b, length);
	return rv;
#endif /* NSS_ENABLE_ECC */

    default:
    	desc    = handshake_failure;
	errCode = SEC_ERROR_UNSUPPORTED_KEYALG;
	break;		/* goto alert_loser; */
    }

alert_loser:
    (void)SSL3_SendAlert(ss, alert_fatal, desc);
loser:
    PORT_SetError( errCode );
    return SECFailure;

no_memory:	/* no-memory error has already been set. */
    ssl_MapLowLevelError(SSL_ERROR_SERVER_KEY_EXCHANGE_FAILURE);
    return SECFailure;
}


typedef struct dnameNode {
    struct dnameNode *next;
    SECItem           name;
} dnameNode;

/* Called from ssl3_HandleHandshakeMessage() when it has deciphered a complete
 * ssl3 Certificate Request message.
 * Caller must hold Handshake and RecvBuf locks.
 */
static SECStatus
ssl3_HandleCertificateRequest(sslSocket *ss, SSL3Opaque *b, PRUint32 length)
{
    PRArenaPool *        arena       = NULL;
    dnameNode *          node;
    PRInt32              remaining;
    PRBool               isTLS       = PR_FALSE;
    int                  i;
    int                  errCode     = SSL_ERROR_RX_MALFORMED_CERT_REQUEST;
    int                  nnames      = 0;
    SECStatus            rv;
    SSL3AlertDescription desc        = illegal_parameter;
    SECItem              cert_types  = {siBuffer, NULL, 0};
    CERTDistNames        ca_list;

    SSL_TRC(3, ("%d: SSL3[%d]: handle certificate_request handshake",
		SSL_GETPID(), ss->fd));
    PORT_Assert( ss->opt.noLocks || ssl_HaveRecvBufLock(ss) );
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) );

    if (ss->ssl3.hs.ws != wait_cert_request &&
    	ss->ssl3.hs.ws != wait_server_key) {
	desc    = unexpected_message;
	errCode = SSL_ERROR_RX_UNEXPECTED_CERT_REQUEST;
	goto alert_loser;
    }

    /* clean up anything left from previous handshake. */
    if (ss->ssl3.clientCertChain != NULL) {
       CERT_DestroyCertificateList(ss->ssl3.clientCertChain);
       ss->ssl3.clientCertChain = NULL;
    }
    if (ss->ssl3.clientCertificate != NULL) {
       CERT_DestroyCertificate(ss->ssl3.clientCertificate);
       ss->ssl3.clientCertificate = NULL;
    }
    if (ss->ssl3.clientPrivateKey != NULL) {
       SECKEY_DestroyPrivateKey(ss->ssl3.clientPrivateKey);
       ss->ssl3.clientPrivateKey = NULL;
    }

    isTLS = (PRBool)(ss->ssl3.prSpec->version > SSL_LIBRARY_VERSION_3_0);
    rv = ssl3_ConsumeHandshakeVariable(ss, &cert_types, 1, &b, &length);
    if (rv != SECSuccess)
    	goto loser;		/* malformed, alert has been sent */

    arena = ca_list.arena = PORT_NewArena(DER_DEFAULT_CHUNKSIZE);
    if (arena == NULL)
    	goto no_mem;

    remaining = ssl3_ConsumeHandshakeNumber(ss, 2, &b, &length);
    if (remaining < 0)
    	goto loser;	 	/* malformed, alert has been sent */

    if ((PRUint32)remaining > length)
	goto alert_loser;

    ca_list.head = node = PORT_ArenaZNew(arena, dnameNode);
    if (node == NULL)
    	goto no_mem;

    while (remaining > 0) {
	PRInt32 len;

	if (remaining < 2)
	    goto alert_loser;	/* malformed */

	node->name.len = len = ssl3_ConsumeHandshakeNumber(ss, 2, &b, &length);
	if (len <= 0)
	    goto loser;		/* malformed, alert has been sent */

	remaining -= 2;
	if (remaining < len)
	    goto alert_loser;	/* malformed */

	node->name.data = b;
	b         += len;
	length    -= len;
	remaining -= len;
	nnames++;
	if (remaining <= 0)
	    break;		/* success */

	node->next = PORT_ArenaZNew(arena, dnameNode);
	node = node->next;
	if (node == NULL)
	    goto no_mem;
    }

    ca_list.nnames = nnames;
    ca_list.names  = PORT_ArenaNewArray(arena, SECItem, nnames);
    if (nnames > 0 && ca_list.names == NULL)
        goto no_mem;

    for(i = 0, node = (dnameNode*)ca_list.head;
	i < nnames;
	i++, node = node->next) {
	ca_list.names[i] = node->name;
    }

    if (length != 0)
        goto alert_loser;   	/* malformed */

    desc = no_certificate;
    ss->ssl3.hs.ws = wait_hello_done;

    if (ss->getClientAuthData == NULL) {
	rv = SECFailure; /* force it to send a no_certificate alert */
    } else {
	/* XXX Should pass cert_types in this call!! */
	rv = (SECStatus)(*ss->getClientAuthData)(ss->getClientAuthDataArg,
						 ss->fd, &ca_list,
						 &ss->ssl3.clientCertificate,
						 &ss->ssl3.clientPrivateKey);
    }
    switch (rv) {
    case SECWouldBlock:	/* getClientAuthData has put up a dialog box. */
	ssl_SetAlwaysBlock(ss);
	break;	/* not an error */

    case SECSuccess:
        /* check what the callback function returned */
        if ((!ss->ssl3.clientCertificate) || (!ss->ssl3.clientPrivateKey)) {
            /* we are missing either the key or cert */
            if (ss->ssl3.clientCertificate) {
                /* got a cert, but no key - free it */
                CERT_DestroyCertificate(ss->ssl3.clientCertificate);
                ss->ssl3.clientCertificate = NULL;
            }
            if (ss->ssl3.clientPrivateKey) {
                /* got a key, but no cert - free it */
                SECKEY_DestroyPrivateKey(ss->ssl3.clientPrivateKey);
                ss->ssl3.clientPrivateKey = NULL;
            }
            goto send_no_certificate;
        }
	/* Setting ssl3.clientCertChain non-NULL will cause
	 * ssl3_HandleServerHelloDone to call SendCertificate.
	 */
	ss->ssl3.clientCertChain = CERT_CertChainFromCert(
					ss->ssl3.clientCertificate,
					certUsageSSLClient, PR_FALSE);
	if (ss->ssl3.clientCertChain == NULL) {
	    if (ss->ssl3.clientCertificate != NULL) {
		CERT_DestroyCertificate(ss->ssl3.clientCertificate);
		ss->ssl3.clientCertificate = NULL;
	    }
	    if (ss->ssl3.clientPrivateKey != NULL) {
		SECKEY_DestroyPrivateKey(ss->ssl3.clientPrivateKey);
		ss->ssl3.clientPrivateKey = NULL;
	    }
	    goto send_no_certificate;
	}
	break;	/* not an error */

    case SECFailure:
    default:
send_no_certificate:
	if (isTLS) {
	    ss->ssl3.sendEmptyCert = PR_TRUE;
	} else {
	    (void)SSL3_SendAlert(ss, alert_warning, no_certificate);
	}
	rv = SECSuccess;
	break;
    }
    goto done;

no_mem:
    rv = SECFailure;
    PORT_SetError(SEC_ERROR_NO_MEMORY);
    goto done;

alert_loser:
    if (isTLS && desc == illegal_parameter)
    	desc = decode_error;
    (void)SSL3_SendAlert(ss, alert_fatal, desc);
loser:
    PORT_SetError(errCode);
    rv = SECFailure;
done:
    if (arena != NULL)
    	PORT_FreeArena(arena, PR_FALSE);
    return rv;
}

/*
 * attempt to restart the handshake after asynchronously handling
 * a request for the client's certificate.
 *
 * inputs:
 *	cert	Client cert chosen by application.
 *		Note: ssl takes this reference, and does not bump the
 *		reference count.  The caller should drop its reference
 *		without calling CERT_DestroyCert after calling this function.
 *
 *	key	Private key associated with cert.  This function makes a
 *		copy of the private key, so the caller remains responsible
 *		for destroying its copy after this function returns.
 *
 *	certChain  DER-encoded certs, client cert and its signers.
 *		Note: ssl takes this reference, and does not copy the chain.
 *		The caller should drop its reference without destroying the
 *		chain.  SSL will free the chain when it is done with it.
 *
 * Return value: XXX
 *
 * XXX This code only works on the initial handshake on a connection, XXX
 *     It does not work on a subsequent handshake (redo).
 *
 * Caller holds 1stHandshakeLock.
 */
SECStatus
ssl3_RestartHandshakeAfterCertReq(sslSocket *         ss,
				CERTCertificate *    cert,
				SECKEYPrivateKey *   key,
				CERTCertificateList *certChain)
{
    SECStatus        rv          = SECSuccess;

    if (MSB(ss->version) == MSB(SSL_LIBRARY_VERSION_3_0)) {
	/* XXX This code only works on the initial handshake on a connection,
	** XXX It does not work on a subsequent handshake (redo).
	*/
	if (ss->handshake != 0) {
	    ss->handshake               = ssl_GatherRecord1stHandshake;
	    ss->ssl3.clientCertificate = cert;
	    ss->ssl3.clientCertChain   = certChain;
	    if (key == NULL) {
		(void)SSL3_SendAlert(ss, alert_warning, no_certificate);
		ss->ssl3.clientPrivateKey = NULL;
	    } else {
		ss->ssl3.clientPrivateKey = SECKEY_CopyPrivateKey(key);
	    }
	    ssl_GetRecvBufLock(ss);
	    if (ss->ssl3.hs.msgState.buf != NULL) {
		rv = ssl3_HandleRecord(ss, NULL, &ss->gs.buf);
	    }
	    ssl_ReleaseRecvBufLock(ss);
	}
    }
    return rv;
}



/* Called from ssl3_HandleHandshakeMessage() when it has deciphered a complete
 * ssl3 Server Hello Done message.
 * Caller must hold Handshake and RecvBuf locks.
 */
static SECStatus
ssl3_HandleServerHelloDone(sslSocket *ss)
{
    SECStatus     rv;
    SSL3WaitState ws          = ss->ssl3.hs.ws;
    PRBool        send_verify = PR_FALSE;

    SSL_TRC(3, ("%d: SSL3[%d]: handle server_hello_done handshake",
		SSL_GETPID(), ss->fd));
    PORT_Assert( ss->opt.noLocks || ssl_HaveRecvBufLock(ss) );
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) );

    if (ws != wait_hello_done  &&
        ws != wait_server_cert &&
	ws != wait_server_key  &&
	ws != wait_cert_request) {
	SSL3_SendAlert(ss, alert_fatal, unexpected_message);
	PORT_SetError(SSL_ERROR_RX_UNEXPECTED_HELLO_DONE);
	return SECFailure;
    }

    ssl_GetXmitBufLock(ss);		/*******************************/

    if (ss->ssl3.sendEmptyCert) {
	ss->ssl3.sendEmptyCert = PR_FALSE;
	rv = ssl3_SendEmptyCertificate(ss);
	/* Don't send verify */
	if (rv != SECSuccess) {
	    goto loser;	/* error code is set. */
    	}
    } else
    if (ss->ssl3.clientCertChain  != NULL &&
	ss->ssl3.clientPrivateKey != NULL) {
	send_verify = PR_TRUE;
	rv = ssl3_SendCertificate(ss);
	if (rv != SECSuccess) {
	    goto loser;	/* error code is set. */
    	}
    }

    rv = ssl3_SendClientKeyExchange(ss);
    if (rv != SECSuccess) {
    	goto loser;	/* err is set. */
    }

    if (send_verify) {
	rv = ssl3_SendCertificateVerify(ss);
	if (rv != SECSuccess) {
	    goto loser;	/* err is set. */
        }
    }
    rv = ssl3_SendChangeCipherSpecs(ss);
    if (rv != SECSuccess) {
	goto loser;	/* err code was set. */
    }
    rv = ssl3_SendFinished(ss, 0);
    if (rv != SECSuccess) {
	goto loser;	/* err code was set. */
    }

    ssl_ReleaseXmitBufLock(ss);		/*******************************/

    ss->ssl3.hs.ws = wait_change_cipher;
    return SECSuccess;

loser:
    ssl_ReleaseXmitBufLock(ss);
    return rv;
}

/*
 * Routines used by servers
 */
static SECStatus
ssl3_SendHelloRequest(sslSocket *ss)
{
    SECStatus rv;

    SSL_TRC(3, ("%d: SSL3[%d]: send hello_request handshake", SSL_GETPID(),
		ss->fd));

    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) );
    PORT_Assert( ss->opt.noLocks || ssl_HaveXmitBufLock(ss) );

    rv = ssl3_AppendHandshakeHeader(ss, hello_request, 0);
    if (rv != SECSuccess) {
	return rv;	/* err set by AppendHandshake */
    }
    rv = ssl3_FlushHandshake(ss, 0);
    if (rv != SECSuccess) {
	return rv;	/* error code set by ssl3_FlushHandshake */
    }
    ss->ssl3.hs.ws = wait_client_hello;
    return SECSuccess;
}

/* Sets memory error when returning NULL.
 * Called from:
 *	ssl3_SendClientHello()
 *	ssl3_HandleServerHello()
 *	ssl3_HandleClientHello()
 *	ssl3_HandleV2ClientHello()
 */
static sslSessionID *
ssl3_NewSessionID(sslSocket *ss, PRBool is_server)
{
    sslSessionID *sid;

    sid = PORT_ZNew(sslSessionID);
    if (sid == NULL)
    	return sid;

    sid->peerID		= (ss->peerID == NULL) ? NULL : PORT_Strdup(ss->peerID);
    sid->urlSvrName	= (ss->url    == NULL) ? NULL : PORT_Strdup(ss->url);
    sid->addr           = ss->sec.ci.peer;
    sid->port           = ss->sec.ci.port;
    sid->references     = 1;
    sid->cached         = never_cached;
    sid->version        = ss->version;

    sid->u.ssl3.keys.resumable = PR_TRUE;
    sid->u.ssl3.policy         = SSL_ALLOWED;
    sid->u.ssl3.clientWriteKey = NULL;
    sid->u.ssl3.serverWriteKey = NULL;

    if (is_server) {
	SECStatus rv;
	int       pid = SSL_GETPID();

	sid->u.ssl3.sessionIDLength = SSL3_SESSIONID_BYTES;
	sid->u.ssl3.sessionID[0]    = (pid >> 8) & 0xff;
	sid->u.ssl3.sessionID[1]    =  pid       & 0xff;
	rv = PK11_GenerateRandom(sid->u.ssl3.sessionID + 2,
	                         SSL3_SESSIONID_BYTES -2);
	if (rv != SECSuccess) {
	    ssl_FreeSID(sid);
	    ssl_MapLowLevelError(SSL_ERROR_GENERATE_RANDOM_FAILURE);
	    return NULL;
    	}
    }
    return sid;
}

/* Called from:  ssl3_HandleClientHello, ssl3_HandleV2ClientHello */
static SECStatus
ssl3_SendServerHelloSequence(sslSocket *ss)
{
    const ssl3KEADef *kea_def;
    SECStatus         rv;

    SSL_TRC(3, ("%d: SSL3[%d]: begin send server_hello sequence",
		SSL_GETPID(), ss->fd));

    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) );
    PORT_Assert( ss->opt.noLocks || ssl_HaveXmitBufLock(ss) );

    rv = ssl3_SendServerHello(ss);
    if (rv != SECSuccess) {
	return rv;	/* err code is set. */
    }
    rv = ssl3_SendCertificate(ss);
    if (rv != SECSuccess) {
	return rv;	/* error code is set. */
    }
    /* We have to do this after the call to ssl3_SendServerHello,
     * because kea_def is set up by ssl3_SendServerHello().
     */
    kea_def = ss->ssl3.hs.kea_def;
    ss->ssl3.hs.usedStepDownKey = PR_FALSE;

    if (kea_def->is_limited && kea_def->exchKeyType == kt_rsa) {
	/* see if we can legally use the key in the cert. */
	int keyLen;  /* bytes */

	keyLen = PK11_GetPrivateModulusLen(
			    ss->serverCerts[kea_def->exchKeyType].SERVERKEY);

	if (keyLen > 0 &&
	    keyLen * BPB <= kea_def->key_size_limit ) {
	    /* XXX AND cert is not signing only!! */
	    /* just fall through and use it. */
	} else if (ss->stepDownKeyPair != NULL) {
	    ss->ssl3.hs.usedStepDownKey = PR_TRUE;
	    rv = ssl3_SendServerKeyExchange(ss);
	    if (rv != SECSuccess) {
		return rv;	/* err code was set. */
	    }
	} else {
#ifndef HACKED_EXPORT_SERVER
	    PORT_SetError(SSL_ERROR_PUB_KEY_SIZE_LIMIT_EXCEEDED);
	    return rv;
#endif
	}
#ifdef NSS_ENABLE_ECC
    } else if ((kea_def->kea == kea_ecdhe_rsa) ||
	       (kea_def->kea == kea_ecdhe_ecdsa)) {
	rv = ssl3_SendServerKeyExchange(ss);
	if (rv != SECSuccess) {
	    return rv;	/* err code was set. */
	}
#endif /* NSS_ENABLE_ECC */
    }

    if (ss->opt.requestCertificate) {
	rv = ssl3_SendCertificateRequest(ss);
	if (rv != SECSuccess) {
	    return rv;		/* err code is set. */
	}
    }
    rv = ssl3_SendServerHelloDone(ss);
    if (rv != SECSuccess) {
	return rv;		/* err code is set. */
    }

    ss->ssl3.hs.ws = (ss->opt.requestCertificate) ? wait_client_cert
                                               : wait_client_key;
    return SECSuccess;
}

/* Called from ssl3_HandleHandshakeMessage() when it has deciphered a complete
 * ssl3 Client Hello message.
 * Caller must hold Handshake and RecvBuf locks.
 */
static SECStatus
ssl3_HandleClientHello(sslSocket *ss, SSL3Opaque *b, PRUint32 length)
{
    sslSessionID *      sid      = NULL;
    PRInt32		tmp;
    unsigned int        i;
    int                 j;
    SECStatus           rv;
    int                 errCode  = SSL_ERROR_RX_MALFORMED_CLIENT_HELLO;
    SSL3AlertDescription desc    = illegal_parameter;
    SSL3ProtocolVersion version;
    SECItem             sidBytes = {siBuffer, NULL, 0};
    SECItem             suites   = {siBuffer, NULL, 0};
    SECItem             comps    = {siBuffer, NULL, 0};
    PRBool              haveSpecWriteLock = PR_FALSE;
    PRBool              haveXmitBufLock   = PR_FALSE;

    SSL_TRC(3, ("%d: SSL3[%d]: handle client_hello handshake",
    	SSL_GETPID(), ss->fd));

    PORT_Assert( ss->opt.noLocks || ssl_HaveRecvBufLock(ss) );
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss));

    /* Get peer name of client */
    rv = ssl_GetPeerInfo(ss);
    if (rv != SECSuccess) {
	return rv;		/* error code is set. */
    }

    rv = ssl3_InitState(ss);
    if (rv != SECSuccess) {
	return rv;		/* ssl3_InitState has set the error code. */
    }

    if ((ss->ssl3.hs.ws != wait_client_hello) &&
	(ss->ssl3.hs.ws != idle_handshake)) {
	desc    = unexpected_message;
	errCode = SSL_ERROR_RX_UNEXPECTED_CLIENT_HELLO;
	goto alert_loser;
    }

    tmp = ssl3_ConsumeHandshakeNumber(ss, 2, &b, &length);
    if (tmp < 0)
	goto loser;		/* malformed, alert already sent */
    ss->clientHelloVersion = version = (SSL3ProtocolVersion)tmp;
    rv = ssl3_NegotiateVersion(ss, version);
    if (rv != SECSuccess) {
    	desc = (version > SSL_LIBRARY_VERSION_3_0) ? protocol_version : handshake_failure;
	errCode = SSL_ERROR_NO_CYPHER_OVERLAP;
	goto alert_loser;
    }

    /* grab the client random data. */
    rv = ssl3_ConsumeHandshake(
	ss, &ss->ssl3.hs.client_random, SSL3_RANDOM_LENGTH, &b, &length);
    if (rv != SECSuccess) {
	goto loser;		/* malformed */
    }

    /* grab the client's SID, if present. */
    rv = ssl3_ConsumeHandshakeVariable(ss, &sidBytes, 1, &b, &length);
    if (rv != SECSuccess) {
	goto loser;		/* malformed */
    }

    if (sidBytes.len > 0 && !ss->opt.noCache) {
	SSL_TRC(7, ("%d: SSL3[%d]: server, lookup client session-id for 0x%08x%08x%08x%08x",
                    SSL_GETPID(), ss->fd, ss->sec.ci.peer.pr_s6_addr32[0],
		    ss->sec.ci.peer.pr_s6_addr32[1], 
		    ss->sec.ci.peer.pr_s6_addr32[2],
		    ss->sec.ci.peer.pr_s6_addr32[3]));
	if (ssl_sid_lookup) {
	    sid = (*ssl_sid_lookup)(&ss->sec.ci.peer, sidBytes.data, 
	                            sidBytes.len, ss->dbHandle);
    	} else {
	    errCode = SSL_ERROR_SERVER_CACHE_NOT_CONFIGURED;
	    goto loser;
	}
    }

    /* grab the list of cipher suites. */
    rv = ssl3_ConsumeHandshakeVariable(ss, &suites, 2, &b, &length);
    if (rv != SECSuccess) {
	goto loser;		/* malformed */
    }

    /* grab the list of compression methods. */
    rv = ssl3_ConsumeHandshakeVariable(ss, &comps, 1, &b, &length);
    if (rv != SECSuccess) {
	goto loser;		/* malformed */
    }

    desc = handshake_failure;

    if (sid != NULL) {
	/* We've found a session cache entry for this client.
	 * Now, if we're going to require a client-auth cert,
	 * and we don't already have this client's cert in the session cache,
	 * and this is the first handshake on this connection (not a redo),
	 * then drop this old cache entry and start a new session.
	 */
	if ((sid->peerCert == NULL) && ss->opt.requestCertificate &&
	    ((ss->opt.requireCertificate == SSL_REQUIRE_ALWAYS) ||
	     (ss->opt.requireCertificate == SSL_REQUIRE_NO_ERROR) ||
	     ((ss->opt.requireCertificate == SSL_REQUIRE_FIRST_HANDSHAKE) 
	      && !ss->firstHsDone))) {

	    SSL_AtomicIncrementLong(& ssl3stats.hch_sid_cache_not_ok );
	    ss->sec.uncache(sid);
	    ssl_FreeSID(sid);
	    sid = NULL;
	}
    }

#ifdef NSS_ENABLE_ECC
    /* Disable any ECC cipher suites for which we have no cert. */
    ssl3_FilterECCipherSuitesByServerCerts(ss);
#endif

#ifdef PARANOID
    /* Look for a matching cipher suite. */
    j = ssl3_config_match_init(ss);
    if (j <= 0) {		/* no ciphers are working/supported by PK11 */
    	errCode = PORT_GetError();	/* error code is already set. */
	goto alert_loser;
    }
#endif

    /* If we already have a session for this client, be sure to pick the
    ** same cipher suite we picked before.
    ** This is not a loop, despite appearances.
    */
    if (sid) do {
	ssl3CipherSuiteCfg *suite = ss->cipherSuites;
	/* Find the entry for the cipher suite used in the cached session. */
	for (j = ssl_V3_SUITES_IMPLEMENTED; j > 0; --j, ++suite) {
	    if (suite->cipher_suite == sid->u.ssl3.cipherSuite)
		break;
	}
	PORT_Assert(j > 0);
	if (j <= 0)
	    break;
#ifdef PARANOID
	/* Double check that the cached cipher suite is still enabled,
	 * implemented, and allowed by policy.  Might have been disabled.
	 * The product policy won't change during the process lifetime.  
	 * Implemented ("isPresent") shouldn't change for servers.
	 */
	if (!config_match(suite, ss->ssl3.policy, PR_TRUE))
	    break;
#else
	if (!suite->enabled)
	    break;
#endif
	/* Double check that the cached cipher suite is in the client's list */
	for (i = 0; i < suites.len; i += 2) {
	    if ((suites.data[i]     == MSB(suite->cipher_suite)) &&
	        (suites.data[i + 1] == LSB(suite->cipher_suite))) {

		ss->ssl3.hs.cipher_suite = suite->cipher_suite;
		ss->ssl3.hs.suite_def =
		    ssl_LookupCipherSuiteDef(ss->ssl3.hs.cipher_suite);
		goto suite_found;
	    }
	}
    } while (0);

    /* START A NEW SESSION */

    /* Handle TLS hello extensions, for SSL3 & TLS, 
     * only if we're not restarting a previous session.
     */
    if (length) {
	/* Get length of hello extensions */
	PRInt32 extension_length;
	extension_length = ssl3_ConsumeHandshakeNumber(ss, 2, &b, &length);
        if (extension_length < 0) {
	    goto loser;				/* alert already sent */
	}
	if (extension_length != length) {
	    ssl3_DecodeError(ss);		/* send alert */
	    goto loser;
    	}
	rv = ssl3_HandleClientHelloExtensions(ss, &b, &length);
	if (rv != SECSuccess) {
	    goto loser;		/* malformed */
	}
    }

#ifndef PARANOID
    /* Look for a matching cipher suite. */
    j = ssl3_config_match_init(ss);
    if (j <= 0) {		/* no ciphers are working/supported by PK11 */
    	errCode = PORT_GetError();	/* error code is already set. */
	goto alert_loser;
    }
#endif

    /* Select a cipher suite.
    ** NOTE: This suite selection algorithm should be the same as the one in
    ** ssl3_HandleV2ClientHello().  
    */
    for (j = 0; j < ssl_V3_SUITES_IMPLEMENTED; j++) {
	ssl3CipherSuiteCfg *suite = &ss->cipherSuites[j];
	if (!config_match(suite, ss->ssl3.policy, PR_TRUE))
	    continue;
	for (i = 0; i < suites.len; i += 2) {
	    if ((suites.data[i]     == MSB(suite->cipher_suite)) &&
	        (suites.data[i + 1] == LSB(suite->cipher_suite))) {

		ss->ssl3.hs.cipher_suite = suite->cipher_suite;
		ss->ssl3.hs.suite_def =
		    ssl_LookupCipherSuiteDef(ss->ssl3.hs.cipher_suite);
		goto suite_found;
	    }
	}
    }
    errCode = SSL_ERROR_NO_CYPHER_OVERLAP;
    goto alert_loser;

suite_found:
    /* Look for a matching compression algorithm. */
    for (i = 0; i < comps.len; i++) {
	for (j = 0; j < compressionMethodsCount; j++) {
	    if (comps.data[i] == compressions[j]) {
		ss->ssl3.hs.compression = 
					(SSL3CompressionMethod)compressions[j];
		goto compression_found;
	    }
	}
    }
    errCode = SSL_ERROR_NO_COMPRESSION_OVERLAP;
    				/* null compression must be supported */
    goto alert_loser;

compression_found:
    suites.data = NULL;
    comps.data = NULL;

    ss->sec.send = ssl3_SendApplicationData;

    /* If there are any failures while processing the old sid,
     * we don't consider them to be errors.  Instead, We just behave
     * as if the client had sent us no sid to begin with, and make a new one.
     */
    if (sid != NULL) do {
	ssl3CipherSpec *pwSpec;
	SECItem         wrappedMS;  	/* wrapped key */

	if (sid->version != ss->version  ||
	    sid->u.ssl3.cipherSuite != ss->ssl3.hs.cipher_suite) {
	    break;	/* not an error */
	}

	if (ss->sec.ci.sid) {
	    ss->sec.uncache(ss->sec.ci.sid);
	    PORT_Assert(ss->sec.ci.sid != sid);  /* should be impossible, but ... */
	    if (ss->sec.ci.sid != sid) {
		ssl_FreeSID(ss->sec.ci.sid);
	    }
	    ss->sec.ci.sid = NULL;
	}
	/* we need to resurrect the master secret.... */

	ssl_GetSpecWriteLock(ss);  haveSpecWriteLock = PR_TRUE;
	pwSpec = ss->ssl3.pwSpec;
	if (sid->u.ssl3.keys.msIsWrapped) {
	    PK11SymKey *    wrapKey; 	/* wrapping key */
	    CK_FLAGS        keyFlags      = 0;
	    if (ss->opt.bypassPKCS11) {
		/* we cannot restart a non-bypass session in a 
		** bypass socket.
		*/
		break;  
	    }

	    wrapKey = getWrappingKey(ss, NULL, sid->u.ssl3.exchKeyType,
				     sid->u.ssl3.masterWrapMech, 
				     ss->pkcs11PinArg);
	    if (!wrapKey) {
		/* we have a SID cache entry, but no wrapping key for it??? */
		break;
	    }

	    if (ss->version > SSL_LIBRARY_VERSION_3_0) {	/* isTLS */
		keyFlags = CKF_SIGN | CKF_VERIFY;
	    }

	    wrappedMS.data = sid->u.ssl3.keys.wrapped_master_secret;
	    wrappedMS.len  = sid->u.ssl3.keys.wrapped_master_secret_len;

	    /* unwrap the master secret. */
	    pwSpec->master_secret =
		PK11_UnwrapSymKeyWithFlags(wrapKey, sid->u.ssl3.masterWrapMech, 
			    NULL, &wrappedMS, CKM_SSL3_MASTER_KEY_DERIVE,
			    CKA_DERIVE, sizeof(SSL3MasterSecret), keyFlags);
	    PK11_FreeSymKey(wrapKey);
	    if (pwSpec->master_secret == NULL) {
		break;	/* not an error */
	    }
	} else if (ss->opt.bypassPKCS11) {
	    wrappedMS.data = sid->u.ssl3.keys.wrapped_master_secret;
	    wrappedMS.len  = sid->u.ssl3.keys.wrapped_master_secret_len;
	    memcpy(pwSpec->raw_master_secret, wrappedMS.data, wrappedMS.len);
	    pwSpec->msItem.data = pwSpec->raw_master_secret;
	    pwSpec->msItem.len  = wrappedMS.len;
	} else {
	    /* We CAN restart a bypass session in a non-bypass socket. */
	    /* need to import the raw master secret to session object */
	    PK11SlotInfo * slot;
	    wrappedMS.data = sid->u.ssl3.keys.wrapped_master_secret;
	    wrappedMS.len  = sid->u.ssl3.keys.wrapped_master_secret_len;
	    slot = PK11_GetInternalSlot();
	    pwSpec->master_secret =  
		PK11_ImportSymKey(slot, CKM_SSL3_MASTER_KEY_DERIVE, 
				  PK11_OriginUnwrap, CKA_ENCRYPT, &wrappedMS, 
				  NULL);
	    PK11_FreeSlot(slot);
	    if (pwSpec->master_secret == NULL) {
		break;	/* not an error */
	    }
	}
	ss->sec.ci.sid = sid;
	if (sid->peerCert != NULL) {
	    ss->sec.peerCert = CERT_DupCertificate(sid->peerCert);
	}

	/*
	 * Old SID passed all tests, so resume this old session.
	 *
	 * XXX make sure compression still matches
	 */
	SSL_AtomicIncrementLong(& ssl3stats.hch_sid_cache_hits );
	ss->ssl3.hs.isResuming = PR_TRUE;

        ss->sec.authAlgorithm = sid->authAlgorithm;
	ss->sec.authKeyBits   = sid->authKeyBits;
	ss->sec.keaType       = sid->keaType;
	ss->sec.keaKeyBits    = sid->keaKeyBits;

	/* server sids don't remember the server cert we previously sent,
	** but they do remember the kea type we originally used, so we
	** can locate it again, provided that the current ssl socket
	** has had its server certs configured the same as the previous one.
	*/
	ss->sec.localCert     = 
		CERT_DupCertificate(ss->serverCerts[sid->keaType].serverCert);

	ssl_GetXmitBufLock(ss); haveXmitBufLock = PR_TRUE;

	rv = ssl3_SendServerHello(ss);
	if (rv != SECSuccess) {
	    errCode = PORT_GetError();
	    goto loser;
	}

	if (haveSpecWriteLock) {
	    ssl_ReleaseSpecWriteLock(ss);
	    haveSpecWriteLock = PR_FALSE;
	}

	/* NULL value for PMS signifies re-use of the old MS */
	rv = ssl3_InitPendingCipherSpec(ss,  NULL);
	if (rv != SECSuccess) {
	    errCode = PORT_GetError();
	    goto loser;
	}

	rv = ssl3_SendChangeCipherSpecs(ss);
	if (rv != SECSuccess) {
	    errCode = PORT_GetError();
	    goto loser;
	}
	rv = ssl3_SendFinished(ss, 0);
	ss->ssl3.hs.ws = wait_change_cipher;
	if (rv != SECSuccess) {
	    errCode = PORT_GetError();
	    goto loser;
	}

	if (haveXmitBufLock) {
	    ssl_ReleaseXmitBufLock(ss);
	    haveXmitBufLock = PR_FALSE;
	}

        return SECSuccess;
    } while (0);

    if (haveSpecWriteLock) {
	ssl_ReleaseSpecWriteLock(ss);
	haveSpecWriteLock = PR_FALSE;
    }

    if (sid) { 	/* we had a sid, but it's no longer valid, free it */
	SSL_AtomicIncrementLong(& ssl3stats.hch_sid_cache_not_ok );
	ss->sec.uncache(sid);
	ssl_FreeSID(sid);
	sid = NULL;
    }
    SSL_AtomicIncrementLong(& ssl3stats.hch_sid_cache_misses );

    sid = ssl3_NewSessionID(ss, PR_TRUE);
    if (sid == NULL) {
	errCode = PORT_GetError();
	goto loser;	/* memory error is set. */
    }
    ss->sec.ci.sid = sid;

    ss->ssl3.hs.isResuming = PR_FALSE;
    ssl_GetXmitBufLock(ss);
    rv = ssl3_SendServerHelloSequence(ss);
    ssl_ReleaseXmitBufLock(ss);
    if (rv != SECSuccess) {
	errCode = PORT_GetError();
	goto loser;
    }

    if (haveXmitBufLock) {
	ssl_ReleaseXmitBufLock(ss);
	haveXmitBufLock = PR_FALSE;
    }

    return SECSuccess;

alert_loser:
    if (haveSpecWriteLock) {
	ssl_ReleaseSpecWriteLock(ss);
	haveSpecWriteLock = PR_FALSE;
    }
    (void)SSL3_SendAlert(ss, alert_fatal, desc);
    /* FALLTHRU */
loser:
    if (haveSpecWriteLock) {
	ssl_ReleaseSpecWriteLock(ss);
	haveSpecWriteLock = PR_FALSE;
    }

    if (haveXmitBufLock) {
	ssl_ReleaseXmitBufLock(ss);
	haveXmitBufLock = PR_FALSE;
    }

    PORT_SetError(errCode);
    return SECFailure;
}

/*
 * ssl3_HandleV2ClientHello is used when a V2 formatted hello comes
 * in asking to use the V3 handshake.
 * Called from ssl2_HandleClientHelloMessage() in sslcon.c
 */
SECStatus
ssl3_HandleV2ClientHello(sslSocket *ss, unsigned char *buffer, int length)
{
    sslSessionID *      sid 		= NULL;
    unsigned char *     suites;
    unsigned char *     random;
    SSL3ProtocolVersion version;
    SECStatus           rv;
    int                 i;
    int                 j;
    int                 sid_length;
    int                 suite_length;
    int                 rand_length;
    int                 errCode  = SSL_ERROR_RX_MALFORMED_CLIENT_HELLO;
    SSL3AlertDescription desc    = handshake_failure;

    SSL_TRC(3, ("%d: SSL3[%d]: handle v2 client_hello", SSL_GETPID(), ss->fd));

    PORT_Assert( ss->opt.noLocks || ssl_HaveRecvBufLock(ss) );

    ssl_GetSSL3HandshakeLock(ss);

    rv = ssl3_InitState(ss);
    if (rv != SECSuccess) {
	ssl_ReleaseSSL3HandshakeLock(ss);
	return rv;		/* ssl3_InitState has set the error code. */
    }

    if (ss->ssl3.hs.ws != wait_client_hello) {
	desc    = unexpected_message;
	errCode = SSL_ERROR_RX_UNEXPECTED_CLIENT_HELLO;
	goto loser;	/* alert_loser */
    }

    version      = (buffer[1] << 8) | buffer[2];
    suite_length = (buffer[3] << 8) | buffer[4];
    sid_length   = (buffer[5] << 8) | buffer[6];
    rand_length  = (buffer[7] << 8) | buffer[8];
    ss->clientHelloVersion = version;

    rv = ssl3_NegotiateVersion(ss, version);
    if (rv != SECSuccess) {
	/* send back which ever alert client will understand. */
    	desc = (version > SSL_LIBRARY_VERSION_3_0) ? protocol_version : handshake_failure;
	errCode = SSL_ERROR_NO_CYPHER_OVERLAP;
	goto alert_loser;
    }

    /* if we get a non-zero SID, just ignore it. */
    if (length !=
        SSL_HL_CLIENT_HELLO_HBYTES + suite_length + sid_length + rand_length) {
	SSL_DBG(("%d: SSL3[%d]: bad v2 client hello message, len=%d should=%d",
		 SSL_GETPID(), ss->fd, length,
		 SSL_HL_CLIENT_HELLO_HBYTES + suite_length + sid_length +
		 rand_length));
	goto loser;	/* malformed */	/* alert_loser */
    }

    suites = buffer + SSL_HL_CLIENT_HELLO_HBYTES;
    random = suites + suite_length + sid_length;

    if (rand_length < SSL_MIN_CHALLENGE_BYTES ||
	rand_length > SSL_MAX_CHALLENGE_BYTES) {
	goto loser;	/* malformed */	/* alert_loser */
    }

    PORT_Assert(SSL_MAX_CHALLENGE_BYTES == SSL3_RANDOM_LENGTH);

    PORT_Memset(&ss->ssl3.hs.client_random, 0, SSL3_RANDOM_LENGTH);
    PORT_Memcpy(
	&ss->ssl3.hs.client_random.rand[SSL3_RANDOM_LENGTH - rand_length],
	random, rand_length);

    PRINT_BUF(60, (ss, "client random:", &ss->ssl3.hs.client_random.rand[0],
		   SSL3_RANDOM_LENGTH));
#ifdef NSS_ENABLE_ECC
    /* Disable any ECC cipher suites for which we have no cert. */
    ssl3_FilterECCipherSuitesByServerCerts(ss);
#endif
    i = ssl3_config_match_init(ss);
    if (i <= 0) {
    	errCode = PORT_GetError();	/* error code is already set. */
	goto alert_loser;
    }

    /* Select a cipher suite.
    ** NOTE: This suite selection algorithm should be the same as the one in
    ** ssl3_HandleClientHello().  
    */
    for (j = 0; j < ssl_V3_SUITES_IMPLEMENTED; j++) {
	ssl3CipherSuiteCfg *suite = &ss->cipherSuites[j];
	if (!config_match(suite, ss->ssl3.policy, PR_TRUE))
	    continue;
	for (i = 0; i < suite_length; i += 3) {
	    if ((suites[i]   == 0) &&
		(suites[i+1] == MSB(suite->cipher_suite)) &&
		(suites[i+2] == LSB(suite->cipher_suite))) {

		ss->ssl3.hs.cipher_suite = suite->cipher_suite;
		ss->ssl3.hs.suite_def =
		    ssl_LookupCipherSuiteDef(ss->ssl3.hs.cipher_suite);
		goto suite_found;
	    }
	}
    }
    errCode = SSL_ERROR_NO_CYPHER_OVERLAP;
    goto alert_loser;

suite_found:

    ss->ssl3.hs.compression = compression_null;
    ss->sec.send            = ssl3_SendApplicationData;

    /* we don't even search for a cache hit here.  It's just a miss. */
    SSL_AtomicIncrementLong(& ssl3stats.hch_sid_cache_misses );
    sid = ssl3_NewSessionID(ss, PR_TRUE);
    if (sid == NULL) {
    	errCode = PORT_GetError();
	goto loser;	/* memory error is set. */
    }
    ss->sec.ci.sid = sid;
    /* do not worry about memory leak of sid since it now belongs to ci */

    /* We have to update the handshake hashes before we can send stuff */
    rv = ssl3_UpdateHandshakeHashes(ss, buffer, length);
    if (rv != SECSuccess) {
    	errCode = PORT_GetError();
	goto loser;
    }

    ssl_GetXmitBufLock(ss);
    rv = ssl3_SendServerHelloSequence(ss);
    ssl_ReleaseXmitBufLock(ss);
    if (rv != SECSuccess) {
    	errCode = PORT_GetError();
	goto loser;
    }

    /* XXX_1 	The call stack to here is:
     * ssl_Do1stHandshake -> ssl2_HandleClientHelloMessage -> here.
     * ssl2_HandleClientHelloMessage returns whatever we return here.
     * ssl_Do1stHandshake will continue looping if it gets back either
     *		SECSuccess or SECWouldBlock.
     * SECSuccess is preferable here.  See XXX_1 in sslgathr.c.
     */
    ssl_ReleaseSSL3HandshakeLock(ss);
    return SECSuccess;

alert_loser:
    SSL3_SendAlert(ss, alert_fatal, desc);
loser:
    ssl_ReleaseSSL3HandshakeLock(ss);
    PORT_SetError(errCode);
    return SECFailure;
}

/* The negotiated version number has been already placed in ss->version.
**
** Called from:  ssl3_HandleClientHello                     (resuming session),
** 	ssl3_SendServerHelloSequence <- ssl3_HandleClientHello   (new session),
** 	ssl3_SendServerHelloSequence <- ssl3_HandleV2ClientHello (new session)
*/
static SECStatus
ssl3_SendServerHello(sslSocket *ss)
{
    sslSessionID *sid;
    SECStatus     rv;
    PRUint32      maxBytes = 65535;
    PRUint32      length;
    PRInt32       extensions_len = 0;

    SSL_TRC(3, ("%d: SSL3[%d]: send server_hello handshake", SSL_GETPID(),
		ss->fd));

    PORT_Assert( ss->opt.noLocks || ssl_HaveXmitBufLock(ss));
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss));
    PORT_Assert( MSB(ss->version) == MSB(SSL_LIBRARY_VERSION_3_0));

    if (MSB(ss->version) != MSB(SSL_LIBRARY_VERSION_3_0)) {
	PORT_SetError(SSL_ERROR_NO_CYPHER_OVERLAP);
	return SECFailure;
    }

    sid = ss->sec.ci.sid;

    extensions_len = ssl3_CallHelloExtensionSenders(ss, PR_FALSE, maxBytes,
					       &ss->serverExtensionSenders[0]);
    if (extensions_len > 0)
    	extensions_len += 2; /* Add sizeof total extension length */

    length = sizeof(SSL3ProtocolVersion) + SSL3_RANDOM_LENGTH + 1 +
             ((sid == NULL) ? 0: SSL3_SESSIONID_BYTES) +
	     sizeof(ssl3CipherSuite) + 1 + extensions_len;
    rv = ssl3_AppendHandshakeHeader(ss, server_hello, length);
    if (rv != SECSuccess) {
	return rv;	/* err set by AppendHandshake. */
    }

    rv = ssl3_AppendHandshakeNumber(ss, ss->version, 2);
    if (rv != SECSuccess) {
	return rv;	/* err set by AppendHandshake. */
    }
    rv = ssl3_GetNewRandom(&ss->ssl3.hs.server_random);
    if (rv != SECSuccess) {
	ssl_MapLowLevelError(SSL_ERROR_GENERATE_RANDOM_FAILURE);
	return rv;
    }
    rv = ssl3_AppendHandshake(
	ss, &ss->ssl3.hs.server_random, SSL3_RANDOM_LENGTH);
    if (rv != SECSuccess) {
	return rv;	/* err set by AppendHandshake. */
    }

    if (sid)
	rv = ssl3_AppendHandshakeVariable(
	    ss, sid->u.ssl3.sessionID, sid->u.ssl3.sessionIDLength, 1);
    else
	rv = ssl3_AppendHandshakeVariable(ss, NULL, 0, 1);
    if (rv != SECSuccess) {
	return rv;	/* err set by AppendHandshake. */
    }

    rv = ssl3_AppendHandshakeNumber(ss, ss->ssl3.hs.cipher_suite, 2);
    if (rv != SECSuccess) {
	return rv;	/* err set by AppendHandshake. */
    }
    rv = ssl3_AppendHandshakeNumber(ss, ss->ssl3.hs.compression, 1);
    if (rv != SECSuccess) {
	return rv;	/* err set by AppendHandshake. */
    }
    if (extensions_len) {
	PRInt32 sent_len;

    	extensions_len -= 2;
	rv = ssl3_AppendHandshakeNumber(ss, extensions_len, 2);
	if (rv != SECSuccess) 
	    return rv;	/* err set by ssl3_SetupPendingCipherSpec */
	sent_len = ssl3_CallHelloExtensionSenders(ss, PR_TRUE, extensions_len,
					   &ss->serverExtensionSenders[0]);
        PORT_Assert(sent_len == extensions_len);
	if (sent_len != extensions_len) {
	    if (sent_len >= 0)
	    	PORT_SetError(SEC_ERROR_LIBRARY_FAILURE);
	    return SECFailure;
	}
    }
    rv = ssl3_SetupPendingCipherSpec(ss);
    if (rv != SECSuccess) {
	return rv;	/* err set by ssl3_SetupPendingCipherSpec */
    }

    return SECSuccess;
}


static SECStatus
ssl3_SendServerKeyExchange(sslSocket *ss)
{
const ssl3KEADef *     kea_def     = ss->ssl3.hs.kea_def;
    SECStatus          rv          = SECFailure;
    int                length;
    PRBool             isTLS;
    SECItem            signed_hash = {siBuffer, NULL, 0};
    SSL3Hashes         hashes;
    SECKEYPublicKey *  sdPub;	/* public key for step-down */

    SSL_TRC(3, ("%d: SSL3[%d]: send server_key_exchange handshake",
		SSL_GETPID(), ss->fd));

    PORT_Assert( ss->opt.noLocks || ssl_HaveXmitBufLock(ss));
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss));

    switch (kea_def->exchKeyType) {
    case kt_rsa:
	/* Perform SSL Step-Down here. */
	sdPub = ss->stepDownKeyPair->pubKey;
	PORT_Assert(sdPub != NULL);
	if (!sdPub) {
	    PORT_SetError(SSL_ERROR_SERVER_KEY_EXCHANGE_FAILURE);
	    return SECFailure;
	}
    	rv = ssl3_ComputeExportRSAKeyHash(sdPub->u.rsa.modulus,
					  sdPub->u.rsa.publicExponent,
	                                  &ss->ssl3.hs.client_random,
	                                  &ss->ssl3.hs.server_random,
					  &hashes, ss->opt.bypassPKCS11);
        if (rv != SECSuccess) {
	    ssl_MapLowLevelError(SSL_ERROR_SERVER_KEY_EXCHANGE_FAILURE);
	    return rv;
	}

	isTLS = (PRBool)(ss->ssl3.pwSpec->version > SSL_LIBRARY_VERSION_3_0);
	rv = ssl3_SignHashes(&hashes, ss->serverCerts[kt_rsa].SERVERKEY, 
	                     &signed_hash, isTLS);
        if (rv != SECSuccess) {
	    goto loser;		/* ssl3_SignHashes has set err. */
	}
	if (signed_hash.data == NULL) {
	    /* how can this happen and rv == SECSuccess ?? */
	    PORT_SetError(SSL_ERROR_SERVER_KEY_EXCHANGE_FAILURE);
	    goto loser;
	}
	length = 2 + sdPub->u.rsa.modulus.len +
	         2 + sdPub->u.rsa.publicExponent.len +
	         2 + signed_hash.len;

	rv = ssl3_AppendHandshakeHeader(ss, server_key_exchange, length);
	if (rv != SECSuccess) {
	    goto loser; 	/* err set by AppendHandshake. */
	}

	rv = ssl3_AppendHandshakeVariable(ss, sdPub->u.rsa.modulus.data,
					  sdPub->u.rsa.modulus.len, 2);
	if (rv != SECSuccess) {
	    goto loser; 	/* err set by AppendHandshake. */
	}

	rv = ssl3_AppendHandshakeVariable(
				ss, sdPub->u.rsa.publicExponent.data,
				sdPub->u.rsa.publicExponent.len, 2);
	if (rv != SECSuccess) {
	    goto loser; 	/* err set by AppendHandshake. */
	}

	rv = ssl3_AppendHandshakeVariable(ss, signed_hash.data,
	                                  signed_hash.len, 2);
	if (rv != SECSuccess) {
	    goto loser; 	/* err set by AppendHandshake. */
	}
	PORT_Free(signed_hash.data);
	return SECSuccess;

#ifdef NSS_ENABLE_ECC
    case kt_ecdh: {
	rv = ssl3_SendECDHServerKeyExchange(ss);
	return rv;
    }
#endif /* NSS_ENABLE_ECC */

    case kt_dh:
    case kt_null:
    default:
	PORT_SetError(SEC_ERROR_UNSUPPORTED_KEYALG);
	break;
    }
loser:
    if (signed_hash.data != NULL) 
    	PORT_Free(signed_hash.data);
    return SECFailure;
}


static SECStatus
ssl3_SendCertificateRequest(sslSocket *ss)
{
    SECItem *      name;
    CERTDistNames *ca_list;
const uint8 *      certTypes;
    SECItem *      names	= NULL;
    SECStatus      rv;
    int            length;
    int            i;
    int            calen	= 0;
    int            nnames	= 0;
    int            certTypesLength;

    SSL_TRC(3, ("%d: SSL3[%d]: send certificate_request handshake",
		SSL_GETPID(), ss->fd));

    PORT_Assert( ss->opt.noLocks || ssl_HaveXmitBufLock(ss));
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss));

    /* ssl3.ca_list is initialized to NULL, and never changed. */
    ca_list = ss->ssl3.ca_list;
    if (!ca_list) {
	ca_list = ssl3_server_ca_list;
    }

    if (ca_list != NULL) {
	names = ca_list->names;
	nnames = ca_list->nnames;
    }

    if (!nnames) {
	PORT_SetError(SSL_ERROR_NO_TRUSTED_SSL_CLIENT_CA);
	return SECFailure;
    }

    for (i = 0, name = names; i < nnames; i++, name++) {
	calen += 2 + name->len;
    }

    certTypes       = certificate_types;
    certTypesLength = sizeof certificate_types;

    length = 1 + certTypesLength + 2 + calen;

    rv = ssl3_AppendHandshakeHeader(ss, certificate_request, length);
    if (rv != SECSuccess) {
	return rv; 		/* err set by AppendHandshake. */
    }
    rv = ssl3_AppendHandshakeVariable(ss, certTypes, certTypesLength, 1);
    if (rv != SECSuccess) {
	return rv; 		/* err set by AppendHandshake. */
    }
    rv = ssl3_AppendHandshakeNumber(ss, calen, 2);
    if (rv != SECSuccess) {
	return rv; 		/* err set by AppendHandshake. */
    }
    for (i = 0, name = names; i < nnames; i++, name++) {
	rv = ssl3_AppendHandshakeVariable(ss, name->data, name->len, 2);
	if (rv != SECSuccess) {
	    return rv; 		/* err set by AppendHandshake. */
	}
    }

    return SECSuccess;
}

static SECStatus
ssl3_SendServerHelloDone(sslSocket *ss)
{
    SECStatus rv;

    SSL_TRC(3, ("%d: SSL3[%d]: send server_hello_done handshake",
		SSL_GETPID(), ss->fd));

    PORT_Assert( ss->opt.noLocks || ssl_HaveXmitBufLock(ss));
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss));

    rv = ssl3_AppendHandshakeHeader(ss, server_hello_done, 0);
    if (rv != SECSuccess) {
	return rv; 		/* err set by AppendHandshake. */
    }
    rv = ssl3_FlushHandshake(ss, 0);
    if (rv != SECSuccess) {
	return rv;	/* error code set by ssl3_FlushHandshake */
    }
    return SECSuccess;
}

/* Called from ssl3_HandleHandshakeMessage() when it has deciphered a complete
 * ssl3 Certificate Verify message
 * Caller must hold Handshake and RecvBuf locks.
 */
static SECStatus
ssl3_HandleCertificateVerify(sslSocket *ss, SSL3Opaque *b, PRUint32 length,
			     SSL3Hashes *hashes)
{
    SECItem              signed_hash = {siBuffer, NULL, 0};
    SECStatus            rv;
    int                  errCode     = SSL_ERROR_RX_MALFORMED_CERT_VERIFY;
    SSL3AlertDescription desc        = handshake_failure;
    PRBool               isTLS;

    SSL_TRC(3, ("%d: SSL3[%d]: handle certificate_verify handshake",
		SSL_GETPID(), ss->fd));
    PORT_Assert( ss->opt.noLocks || ssl_HaveRecvBufLock(ss) );
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) );

    if (ss->ssl3.hs.ws != wait_cert_verify || ss->sec.peerCert == NULL) {
	desc    = unexpected_message;
	errCode = SSL_ERROR_RX_UNEXPECTED_CERT_VERIFY;
	goto alert_loser;
    }

    rv = ssl3_ConsumeHandshakeVariable(ss, &signed_hash, 2, &b, &length);
    if (rv != SECSuccess) {
	goto loser;		/* malformed. */
    }

    isTLS = (PRBool)(ss->ssl3.prSpec->version > SSL_LIBRARY_VERSION_3_0);

    /* XXX verify that the key & kea match */
    rv = ssl3_VerifySignedHashes(hashes, ss->sec.peerCert, &signed_hash,
				 isTLS, ss->pkcs11PinArg);
    if (rv != SECSuccess) {
    	errCode = PORT_GetError();
	desc = isTLS ? decrypt_error : handshake_failure;
	goto alert_loser;
    }

    signed_hash.data = NULL;

    if (length != 0) {
	desc    = isTLS ? decode_error : illegal_parameter;
	goto alert_loser;	/* malformed */
    }
    ss->ssl3.hs.ws = wait_change_cipher;
    return SECSuccess;

alert_loser:
    SSL3_SendAlert(ss, alert_fatal, desc);
loser:
    PORT_SetError(errCode);
    return SECFailure;
}


/* find a slot that is able to generate a PMS and wrap it with RSA.
 * Then generate and return the PMS.
 * If the serverKeySlot parameter is non-null, this function will use
 * that slot to do the job, otherwise it will find a slot.
 *
 * Called from  ssl3_DeriveConnectionKeysPKCS11()  (above)
 *		sendRSAClientKeyExchange()         (above)
 *		ssl3_HandleRSAClientKeyExchange()  (below)
 * Caller must hold the SpecWriteLock, the SSL3HandshakeLock
 */
static PK11SymKey *
ssl3_GenerateRSAPMS(sslSocket *ss, ssl3CipherSpec *spec,
                    PK11SlotInfo * serverKeySlot)
{
    PK11SymKey *      pms		= NULL;
    PK11SlotInfo *    slot		= serverKeySlot;
    void *	      pwArg 		= ss->pkcs11PinArg;
    SECItem           param;
    CK_VERSION 	      version;
    CK_MECHANISM_TYPE mechanism_array[3];

    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) );

    if (slot == NULL) {
	SSLCipherAlgorithm calg;
	/* The specReadLock would suffice here, but we cannot assert on
	** read locks.  Also, all the callers who call with a non-null
	** slot already hold the SpecWriteLock.
	*/
	PORT_Assert( ss->opt.noLocks || ssl_HaveSpecWriteLock(ss));
	PORT_Assert(ss->ssl3.prSpec == ss->ssl3.pwSpec);

        calg = spec->cipher_def->calg;
	PORT_Assert(alg2Mech[calg].calg == calg);

	/* First get an appropriate slot.  */
	mechanism_array[0] = CKM_SSL3_PRE_MASTER_KEY_GEN;
	mechanism_array[1] = CKM_RSA_PKCS;
	mechanism_array[2] = alg2Mech[calg].cmech;

	slot = PK11_GetBestSlotMultiple(mechanism_array, 3, pwArg);
	if (slot == NULL) {
	   /* can't find a slot with all three, find a slot with the minimum */
	    slot = PK11_GetBestSlotMultiple(mechanism_array, 2, pwArg);
	    if (slot == NULL) {
		PORT_SetError(SSL_ERROR_TOKEN_SLOT_NOT_FOUND);
		return pms;	/* which is NULL */
	    }
	}
    }

    /* Generate the pre-master secret ...  */
    version.major = MSB(ss->clientHelloVersion);
    version.minor = LSB(ss->clientHelloVersion);

    param.data = (unsigned char *)&version;
    param.len  = sizeof version;

    pms = PK11_KeyGen(slot, CKM_SSL3_PRE_MASTER_KEY_GEN, &param, 0, pwArg);
    if (!serverKeySlot)
	PK11_FreeSlot(slot);
    if (pms == NULL) {
	ssl_MapLowLevelError(SSL_ERROR_CLIENT_KEY_EXCHANGE_FAILURE);
    }
    return pms;
}

/* Note: The Bleichenbacher attack on PKCS#1 necessitates that we NEVER
 * return any indication of failure of the Client Key Exchange message,
 * where that failure is caused by the content of the client's message.
 * This function must not return SECFailure for any reason that is directly
 * or indirectly caused by the content of the client's encrypted PMS.
 * We must not send an alert and also not drop the connection.
 * Instead, we generate a random PMS.  This will cause a failure
 * in the processing the finished message, which is exactly where
 * the failure must occur.
 *
 * Called from ssl3_HandleClientKeyExchange
 */
static SECStatus
ssl3_HandleRSAClientKeyExchange(sslSocket *ss,
                                SSL3Opaque *b,
				PRUint32 length,
				SECKEYPrivateKey *serverKey)
{
    PK11SymKey *      pms;
    unsigned char *   cr     = (unsigned char *)&ss->ssl3.hs.client_random;
    unsigned char *   sr     = (unsigned char *)&ss->ssl3.hs.server_random;
    ssl3CipherSpec *  pwSpec = ss->ssl3.pwSpec;
    unsigned int      outLen = 0;
    PRBool            isTLS  = PR_FALSE;
    SECStatus         rv;
    SECItem           enc_pms;
    unsigned char     rsaPmsBuf[SSL3_RSA_PMS_LENGTH];
    SECItem           pmsItem = {siBuffer, rsaPmsBuf, sizeof rsaPmsBuf};

    PORT_Assert( ss->opt.noLocks || ssl_HaveRecvBufLock(ss) );
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) );

    enc_pms.data = b;
    enc_pms.len  = length;

    if (ss->ssl3.prSpec->version > SSL_LIBRARY_VERSION_3_0) { /* isTLS */
	PRInt32 kLen;
	kLen = ssl3_ConsumeHandshakeNumber(ss, 2, &enc_pms.data, &enc_pms.len);
	if (kLen < 0) {
	    PORT_SetError(SSL_ERROR_CLIENT_KEY_EXCHANGE_FAILURE);
	    return SECFailure;
	}
	if ((unsigned)kLen < enc_pms.len) {
	    enc_pms.len = kLen;
	}
	isTLS = PR_TRUE;
    } else {
	isTLS = (PRBool)(ss->ssl3.hs.kea_def->tls_keygen != 0);
    }

    if (ss->opt.bypassPKCS11) {
	/* TRIPLE BYPASS, get PMS directly from RSA decryption.
	 * Use PK11_PrivDecryptPKCS1 to decrypt the PMS to a buffer, 
	 * then, check for version rollback attack, then 
	 * do the equivalent of ssl3_DeriveMasterSecret, placing the MS in 
	 * pwSpec->msItem.  Finally call ssl3_InitPendingCipherSpec with 
	 * ss and NULL, so that it will use the MS we've already derived here. 
	 */

	rv = PK11_PrivDecryptPKCS1(serverKey, rsaPmsBuf, &outLen, 
				   sizeof rsaPmsBuf, enc_pms.data, enc_pms.len);
	if (rv != SECSuccess) {
	    /* triple bypass failed.  Let's try for a double bypass. */
	    goto double_bypass;
	} else if (ss->opt.detectRollBack) {
	    SSL3ProtocolVersion client_version = 
					 (rsaPmsBuf[0] << 8) | rsaPmsBuf[1];
	    if (client_version != ss->clientHelloVersion) {
		/* Version roll-back detected. ensure failure.  */
		rv = PK11_GenerateRandom(rsaPmsBuf, sizeof rsaPmsBuf);
	    }
	}
	/* have PMS, build MS without PKCS11 */
	rv = ssl3_MasterKeyDeriveBypass(pwSpec, cr, sr, &pmsItem, isTLS, 
					PR_TRUE);
	if (rv != SECSuccess) {
	    pwSpec->msItem.data = pwSpec->raw_master_secret;
	    pwSpec->msItem.len  = SSL3_MASTER_SECRET_LENGTH;
	    PK11_GenerateRandom(pwSpec->msItem.data, pwSpec->msItem.len);
	}
	rv = ssl3_InitPendingCipherSpec(ss,  NULL);
    } else {
double_bypass:
	/*
	 * unwrap pms out of the incoming buffer
	 * Note: CKM_SSL3_MASTER_KEY_DERIVE is NOT the mechanism used to do 
	 *	the unwrap.  Rather, it is the mechanism with which the 
	 *      unwrapped pms will be used.
	 */
	pms = PK11_PubUnwrapSymKey(serverKey, &enc_pms,
				   CKM_SSL3_MASTER_KEY_DERIVE, CKA_DERIVE, 0);
	if (pms != NULL) {
	    PRINT_BUF(60, (ss, "decrypted premaster secret:",
			   PK11_GetKeyData(pms)->data,
			   PK11_GetKeyData(pms)->len));
	} else {
	    /* unwrap failed. Generate a bogus PMS and carry on. */
	    PK11SlotInfo *  slot   = PK11_GetSlotFromPrivateKey(serverKey);

	    ssl_GetSpecWriteLock(ss);
	    pms = ssl3_GenerateRSAPMS(ss, ss->ssl3.prSpec, slot);
	    ssl_ReleaseSpecWriteLock(ss);
	    PK11_FreeSlot(slot);
	}

	if (pms == NULL) {
	    /* last gasp.  */
	    ssl_MapLowLevelError(SSL_ERROR_CLIENT_KEY_EXCHANGE_FAILURE);
	    return SECFailure;
	}

	/* This step will derive the MS from the PMS, among other things. */
	rv = ssl3_InitPendingCipherSpec(ss,  pms);
	PK11_FreeSymKey(pms);
    }

    if (rv != SECSuccess) {
	SEND_ALERT
	return SECFailure; /* error code set by ssl3_InitPendingCipherSpec */
    }
    return SECSuccess;
}


/* Called from ssl3_HandleHandshakeMessage() when it has deciphered a complete
 * ssl3 ClientKeyExchange message from the remote client
 * Caller must hold Handshake and RecvBuf locks.
 */
static SECStatus
ssl3_HandleClientKeyExchange(sslSocket *ss, SSL3Opaque *b, PRUint32 length)
{
    SECKEYPrivateKey *serverKey         = NULL;
    SECStatus         rv;
const ssl3KEADef *    kea_def;
    ssl3KeyPair     *serverKeyPair      = NULL;
#ifdef NSS_ENABLE_ECC
    SECKEYPublicKey *serverPubKey       = NULL;
#endif /* NSS_ENABLE_ECC */

    SSL_TRC(3, ("%d: SSL3[%d]: handle client_key_exchange handshake",
		SSL_GETPID(), ss->fd));

    PORT_Assert( ss->opt.noLocks || ssl_HaveRecvBufLock(ss) );
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) );

    if (ss->ssl3.hs.ws != wait_client_key) {
	SSL3_SendAlert(ss, alert_fatal, unexpected_message);
    	PORT_SetError(SSL_ERROR_RX_UNEXPECTED_CLIENT_KEY_EXCH);
	return SECFailure;
    }

    kea_def   = ss->ssl3.hs.kea_def;

    if (ss->ssl3.hs.usedStepDownKey) {
	 PORT_Assert(kea_def->is_limited /* XXX OR cert is signing only */
		 && kea_def->exchKeyType == kt_rsa 
		 && ss->stepDownKeyPair != NULL);
	 if (!kea_def->is_limited  ||
	      kea_def->exchKeyType != kt_rsa ||
	      ss->stepDownKeyPair == NULL) {
	 	/* shouldn't happen, don't use step down if it does */
		goto skip;
	 }
    	serverKeyPair = ss->stepDownKeyPair;
	ss->sec.keaKeyBits = EXPORT_RSA_KEY_LENGTH * BPB;
    } else 
skip:
#ifdef NSS_ENABLE_ECC
    /* XXX Using SSLKEAType to index server certifiates
     * does not work for (EC)DHE ciphers. Until we have
     * an indexing mechanism general enough for all key
     * exchange algorithms, we'll need to deal with each
     * one seprately.
     */
    if ((kea_def->kea == kea_ecdhe_rsa) ||
               (kea_def->kea == kea_ecdhe_ecdsa)) {
	if (ss->ephemeralECDHKeyPair != NULL) {
	   serverKeyPair = ss->ephemeralECDHKeyPair;
	   if (serverKeyPair->pubKey) {
		ss->sec.keaKeyBits = 
		    SECKEY_PublicKeyStrengthInBits(serverKeyPair->pubKey);
	   }
	}
    } else 
#endif
    {
	sslServerCerts * sc = ss->serverCerts + kea_def->exchKeyType;
	serverKeyPair = sc->serverKeyPair;
	ss->sec.keaKeyBits = sc->serverKeyBits;
    }

    if (serverKeyPair) {
	serverKey = serverKeyPair->privKey;
    }

    if (serverKey == NULL) {
    	SEND_ALERT
	PORT_SetError(SSL_ERROR_NO_SERVER_KEY_FOR_ALG);
	return SECFailure;
    }

    ss->sec.keaType    = kea_def->exchKeyType;

    switch (kea_def->exchKeyType) {
    case kt_rsa:
	rv = ssl3_HandleRSAClientKeyExchange(ss, b, length, serverKey);
	if (rv != SECSuccess) {
	    SEND_ALERT
	    return SECFailure;	/* error code set */
	}
	break;


#ifdef NSS_ENABLE_ECC
    case kt_ecdh:
	/* XXX We really ought to be able to store multiple
	 * EC certs (a requirement if we wish to support both
	 * ECDH-RSA and ECDH-ECDSA key exchanges concurrently).
	 * When we make that change, we'll need an index other
	 * than kt_ecdh to pick the right EC certificate.
	 */
	if (serverKeyPair) {
	    serverPubKey = serverKeyPair->pubKey;
        }
	if (serverPubKey == NULL) {
	    /* XXX Is this the right error code? */
	    PORT_SetError(SSL_ERROR_EXTRACT_PUBLIC_KEY_FAILURE);
	    return SECFailure;
	}
	rv = ssl3_HandleECDHClientKeyExchange(ss, b, length, 
					      serverPubKey, serverKey);
	if (rv != SECSuccess) {
	    return SECFailure;	/* error code set */
	}
	break;
#endif /* NSS_ENABLE_ECC */

    default:
	(void) ssl3_HandshakeFailure(ss);
	PORT_SetError(SEC_ERROR_UNSUPPORTED_KEYALG);
	return SECFailure;
    }
    ss->ssl3.hs.ws = ss->sec.peerCert ? wait_cert_verify : wait_change_cipher;
    return SECSuccess;

}

/* This is TLS's equivalent of sending a no_certificate alert. */
static SECStatus
ssl3_SendEmptyCertificate(sslSocket *ss)
{
    SECStatus            rv;

    rv = ssl3_AppendHandshakeHeader(ss, certificate, 3);
    if (rv == SECSuccess) {
	rv = ssl3_AppendHandshakeNumber(ss, 0, 3);
    }
    return rv;	/* error, if any, set by functions called above. */
}

#ifdef NISCC_TEST
static PRInt32 connNum = 0;

static SECStatus 
get_fake_cert(SECItem *pCertItem, int *pIndex)
{
    PRFileDesc *cf;
    char *      testdir;
    char *      startat;
    char *      stopat;
    const char *extension;
    int         fileNum;
    PRInt32     numBytes   = 0;
    PRStatus    prStatus;
    PRFileInfo  info;
    char        cfn[100];

    pCertItem->data = 0;
    if ((testdir = PR_GetEnv("NISCC_TEST")) == NULL) {
	return SECSuccess;
    }
    *pIndex   = (NULL != strstr(testdir, "root"));
    extension = (strstr(testdir, "simple") ? "" : ".der");
    fileNum     = PR_AtomicIncrement(&connNum) - 1;
    if ((startat = PR_GetEnv("START_AT")) != NULL) {
	fileNum += atoi(startat);
    }
    if ((stopat = PR_GetEnv("STOP_AT")) != NULL && 
	fileNum >= atoi(stopat)) {
	*pIndex = -1;
	return SECSuccess;
    }
    sprintf(cfn, "%s/%08d%s", testdir, fileNum, extension);
    cf = PR_Open(cfn, PR_RDONLY, 0);
    if (!cf) {
	goto loser;
    }
    prStatus = PR_GetOpenFileInfo(cf, &info);
    if (prStatus != PR_SUCCESS) {
	PR_Close(cf);
	goto loser;
    }
    pCertItem = SECITEM_AllocItem(NULL, pCertItem, info.size);
    if (pCertItem) {
	numBytes = PR_Read(cf, pCertItem->data, info.size);
    }
    PR_Close(cf);
    if (numBytes != info.size) {
	SECITEM_FreeItem(pCertItem, PR_FALSE);
	PORT_SetError(SEC_ERROR_IO);
	goto loser;
    }
    fprintf(stderr, "using %s\n", cfn);
    return SECSuccess;

loser:
    fprintf(stderr, "failed to use %s\n", cfn);
    *pIndex = -1;
    return SECFailure;
}
#endif

/*
 * Used by both client and server.
 * Called from HandleServerHelloDone and from SendServerHelloSequence.
 */
static SECStatus
ssl3_SendCertificate(sslSocket *ss)
{
    SECStatus            rv;
    CERTCertificateList *certChain;
    int                  len 		= 0;
    int                  i;
    SSL3KEAType          certIndex;
#ifdef NISCC_TEST
    SECItem              fakeCert;
    int                  ndex           = -1;
#endif

    SSL_TRC(3, ("%d: SSL3[%d]: send certificate handshake",
		SSL_GETPID(), ss->fd));

    PORT_Assert( ss->opt.noLocks || ssl_HaveXmitBufLock(ss));
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss));

    if (ss->sec.localCert)
    	CERT_DestroyCertificate(ss->sec.localCert);
    if (ss->sec.isServer) {
	sslServerCerts * sc = NULL;

	/* XXX SSLKEAType isn't really a good choice for 
	 * indexing certificates (it breaks when we deal
	 * with (EC)DHE-* cipher suites. This hack ensures
	 * the RSA cert is picked for (EC)DHE-RSA.
	 * Revisit this when we add server side support
	 * for ECDHE-ECDSA or client-side authentication
	 * using EC certificates.
	 */
	if ((ss->ssl3.hs.kea_def->kea == kea_ecdhe_rsa) ||
	    (ss->ssl3.hs.kea_def->kea == kea_dhe_rsa)) {
	    certIndex = kt_rsa;
	} else {
	    certIndex = ss->ssl3.hs.kea_def->exchKeyType;
	}
	sc                    = ss->serverCerts + certIndex;
	certChain             = sc->serverCertChain;
	ss->sec.authKeyBits   = sc->serverKeyBits;
	ss->sec.authAlgorithm = ss->ssl3.hs.kea_def->signKeyType;
	ss->sec.localCert     = CERT_DupCertificate(sc->serverCert);
    } else {
	certChain          = ss->ssl3.clientCertChain;
	ss->sec.localCert = CERT_DupCertificate(ss->ssl3.clientCertificate);
    }

#ifdef NISCC_TEST
    rv = get_fake_cert(&fakeCert, &ndex);
#endif

    if (certChain) {
	for (i = 0; i < certChain->len; i++) {
#ifdef NISCC_TEST
	    if (fakeCert.len > 0 && i == ndex) {
		len += fakeCert.len + 3;
	    } else {
		len += certChain->certs[i].len + 3;
	    }
#else
	    len += certChain->certs[i].len + 3;
#endif
	}
    }

    rv = ssl3_AppendHandshakeHeader(ss, certificate, len + 3);
    if (rv != SECSuccess) {
	return rv; 		/* err set by AppendHandshake. */
    }
    rv = ssl3_AppendHandshakeNumber(ss, len, 3);
    if (rv != SECSuccess) {
	return rv; 		/* err set by AppendHandshake. */
    }
    if (certChain) {
        for (i = 0; i < certChain->len; i++) {
#ifdef NISCC_TEST
            if (fakeCert.len > 0 && i == ndex) {
                rv = ssl3_AppendHandshakeVariable(ss, fakeCert.data,
                                                  fakeCert.len, 3);
                SECITEM_FreeItem(&fakeCert, PR_FALSE);
            } else {
                rv = ssl3_AppendHandshakeVariable(ss, certChain->certs[i].data,
                                                  certChain->certs[i].len, 3);
            }
#else
            rv = ssl3_AppendHandshakeVariable(ss, certChain->certs[i].data,
                                              certChain->certs[i].len, 3);
#endif
            if (rv != SECSuccess) {
                return rv; 		/* err set by AppendHandshake. */
            }
        }
    }

    return SECSuccess;
}

/* This is used to delete the CA certificates in the peer certificate chain
 * from the cert database after they've been validated.
 */
static void
ssl3_CleanupPeerCerts(sslSocket *ss)
{
    PRArenaPool * arena = ss->ssl3.peerCertArena;
    ssl3CertNode *certs = (ssl3CertNode *)ss->ssl3.peerCertChain;

    for (; certs; certs = certs->next) {
	CERT_DestroyCertificate(certs->cert);
    }
    if (arena) PORT_FreeArena(arena, PR_FALSE);
    ss->ssl3.peerCertArena = NULL;
    ss->ssl3.peerCertChain = NULL;
}

/* Called from ssl3_HandleHandshakeMessage() when it has deciphered a complete
 * ssl3 Certificate message.
 * Caller must hold Handshake and RecvBuf locks.
 */
static SECStatus
ssl3_HandleCertificate(sslSocket *ss, SSL3Opaque *b, PRUint32 length)
{
    ssl3CertNode *   c;
    ssl3CertNode *   certs 	= NULL;
    PRArenaPool *    arena 	= NULL;
    CERTCertificate *cert;
    PRInt32          remaining  = 0;
    PRInt32          size;
    SECStatus        rv;
    PRBool           isServer	= (PRBool)(!!ss->sec.isServer);
    PRBool           trusted 	= PR_FALSE;
    PRBool           isTLS;
    SSL3AlertDescription desc	= bad_certificate;
    int              errCode    = SSL_ERROR_RX_MALFORMED_CERTIFICATE;
    SECItem          certItem;

    SSL_TRC(3, ("%d: SSL3[%d]: handle certificate handshake",
		SSL_GETPID(), ss->fd));
    PORT_Assert( ss->opt.noLocks || ssl_HaveRecvBufLock(ss) );
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) );

    if ((ss->ssl3.hs.ws != wait_server_cert) &&
	(ss->ssl3.hs.ws != wait_client_cert)) {
	desc    = unexpected_message;
	errCode = SSL_ERROR_RX_UNEXPECTED_CERTIFICATE;
	goto alert_loser;
    }

    if (ss->sec.peerCert != NULL) {
	if (ss->sec.peerKey) {
	    SECKEY_DestroyPublicKey(ss->sec.peerKey);
	    ss->sec.peerKey = NULL;
	}
	CERT_DestroyCertificate(ss->sec.peerCert);
	ss->sec.peerCert = NULL;
    }

    ssl3_CleanupPeerCerts(ss);
    isTLS = (PRBool)(ss->ssl3.prSpec->version > SSL_LIBRARY_VERSION_3_0);

    /* It is reported that some TLS client sends a Certificate message
    ** with a zero-length message body.  We'll treat that case like a
    ** normal no_certificates message to maximize interoperability.
    */
    if (length) {
	remaining = ssl3_ConsumeHandshakeNumber(ss, 3, &b, &length);
	if (remaining < 0)
	    goto loser;	/* fatal alert already sent by ConsumeHandshake. */
	if ((PRUint32)remaining > length)
	    goto decode_loser;
    }

    if (!remaining) {
	if (!(isTLS && isServer))
	    goto alert_loser;
    	/* This is TLS's version of a no_certificate alert. */
    	/* I'm a server. I've requested a client cert. He hasn't got one. */
	rv = ssl3_HandleNoCertificate(ss);
	if (rv != SECSuccess) {
	    errCode = PORT_GetError();
	    goto loser;
	}
	goto cert_block;
    }

    ss->ssl3.peerCertArena = arena = PORT_NewArena(DER_DEFAULT_CHUNKSIZE);
    if ( arena == NULL ) {
	goto loser;	/* don't send alerts on memory errors */
    }

    /* First get the peer cert. */
    remaining -= 3;
    if (remaining < 0)
	goto decode_loser;

    size = ssl3_ConsumeHandshakeNumber(ss, 3, &b, &length);
    if (size <= 0)
	goto loser;	/* fatal alert already sent by ConsumeHandshake. */

    if (remaining < size)
	goto decode_loser;

    certItem.data = b;
    certItem.len = size;
    b      += size;
    length -= size;
    remaining -= size;

    ss->sec.peerCert = CERT_NewTempCertificate(ss->dbHandle, &certItem, NULL,
                                            PR_FALSE, PR_TRUE);
    if (ss->sec.peerCert == NULL) {
	/* We should report an alert if the cert was bad, but not if the
	 * problem was just some local problem, like memory error.
	 */
	goto ambiguous_err;
    }

    /* Now get all of the CA certs. */
    while (remaining > 0) {
	remaining -= 3;
	if (remaining < 0)
	    goto decode_loser;

	size = ssl3_ConsumeHandshakeNumber(ss, 3, &b, &length);
	if (size <= 0)
	    goto loser;	/* fatal alert already sent by ConsumeHandshake. */

	if (remaining < size)
	    goto decode_loser;

	certItem.data = b;
	certItem.len = size;
	b      += size;
	length -= size;
	remaining -= size;

	c = PORT_ArenaNew(arena, ssl3CertNode);
	if (c == NULL) {
	    goto loser;	/* don't send alerts on memory errors */
	}

	c->cert = CERT_NewTempCertificate(ss->dbHandle, &certItem, NULL,
	                                  PR_FALSE, PR_TRUE);
	if (c->cert == NULL) {
	    goto ambiguous_err;
	}

	if (c->cert->trust)
	    trusted = PR_TRUE;

	c->next = certs;
	certs = c;
    }

    if (remaining != 0)
        goto decode_loser;

    SECKEY_UpdateCertPQG(ss->sec.peerCert);

    /*
     * Ask caller-supplied callback function to validate cert chain.
     */
    rv = (SECStatus)(*ss->authCertificate)(ss->authCertificateArg, ss->fd,
					   PR_TRUE, isServer);
    if (rv) {
	errCode = PORT_GetError();
	if (!ss->handleBadCert) {
	    goto bad_cert;
	}
	rv = (SECStatus)(*ss->handleBadCert)(ss->badCertArg, ss->fd);
	if ( rv ) {
	    if ( rv == SECWouldBlock ) {
		/* someone will handle this connection asynchronously*/
		SSL_DBG(("%d: SSL3[%d]: go to async cert handler",
			 SSL_GETPID(), ss->fd));
		ss->ssl3.peerCertChain = certs;
		certs               = NULL;
		ssl_SetAlwaysBlock(ss);
		goto cert_block;
	    }
	    /* cert is bad */
	    goto bad_cert;
	}
	/* cert is good */
    }

    /* start SSL Step Up, if appropriate */
    cert = ss->sec.peerCert;
    if (!isServer &&
    	ssl3_global_policy_some_restricted &&
        ss->ssl3.policy == SSL_ALLOWED &&
	anyRestrictedEnabled(ss) &&
	SECSuccess == CERT_VerifyCertNow(cert->dbhandle, cert,
	                                 PR_FALSE, /* checkSig */
				         certUsageSSLServerWithStepUp,
/*XXX*/				         ss->authCertificateArg) ) {
	ss->ssl3.policy         = SSL_RESTRICTED;
	ss->ssl3.hs.rehandshake = PR_TRUE;
    }

    ss->sec.ci.sid->peerCert = CERT_DupCertificate(ss->sec.peerCert);

    if (!ss->sec.isServer) {
	/* set the server authentication and key exchange types and sizes
	** from the value in the cert.  If the key exchange key is different,
	** it will get fixed when we handle the server key exchange message.
	*/
	SECKEYPublicKey * pubKey  = CERT_ExtractPublicKey(cert);
	ss->sec.authAlgorithm = ss->ssl3.hs.kea_def->signKeyType;
	ss->sec.keaType       = ss->ssl3.hs.kea_def->exchKeyType;
	if (pubKey) {
	    ss->sec.keaKeyBits = ss->sec.authKeyBits =
		SECKEY_PublicKeyStrengthInBits(pubKey);
#ifdef NSS_ENABLE_ECC
	    if (ss->sec.keaType == kt_ecdh) {
		/* Get authKeyBits from signing key.
		 * XXX The code below uses a quick approximation of
		 * key size based on cert->signatureWrap.signature.data
		 * (which contains the DER encoded signature). The field
		 * cert->signatureWrap.signature.len contains the
		 * length of the encoded signature in bits.
		 */
		if (ss->ssl3.hs.kea_def->kea == kea_ecdh_ecdsa) {
		    ss->sec.authKeyBits = 
			cert->signatureWrap.signature.data[3]*8;
		    if (cert->signatureWrap.signature.data[4] == 0x00)
			    ss->sec.authKeyBits -= 8;
		    /* 
		     * XXX: if cert is not signed by ecdsa we should
		     * destroy pubKey and goto bad_cert
		     */
		} else if (ss->ssl3.hs.kea_def->kea == kea_ecdh_rsa) {
		    ss->sec.authKeyBits = cert->signatureWrap.signature.len;
		    /* 
		     * XXX: if cert is not signed by rsa we should
		     * destroy pubKey and goto bad_cert
		     */
		}
	    }
#endif /* NSS_ENABLE_ECC */
	    SECKEY_DestroyPublicKey(pubKey); 
	    pubKey = NULL;
    	}
    }

    ss->ssl3.peerCertChain = certs;  certs = NULL;  arena = NULL;

cert_block:
    if (ss->sec.isServer) {
	ss->ssl3.hs.ws = wait_client_key;
    } else {
	ss->ssl3.hs.ws = wait_cert_request; /* disallow server_key_exchange */
	if (ss->ssl3.hs.kea_def->is_limited ||
	    /* XXX OR server cert is signing only. */
#ifdef NSS_ENABLE_ECC
	    ss->ssl3.hs.kea_def->kea == kea_ecdhe_ecdsa ||
	    ss->ssl3.hs.kea_def->kea == kea_ecdhe_rsa ||
#endif /* NSS_ENABLE_ECC */
	    ss->ssl3.hs.kea_def->exchKeyType == kt_dh) {
	    ss->ssl3.hs.ws = wait_server_key; /* allow server_key_exchange */
	}
    }

    /* rv must normally be equal to SECSuccess here.  If we called
     * handleBadCert, it can also be SECWouldBlock.
     */
    return rv;

ambiguous_err:
    errCode = PORT_GetError();
    switch (errCode) {
    case PR_OUT_OF_MEMORY_ERROR:
    case SEC_ERROR_BAD_DATABASE:
    case SEC_ERROR_NO_MEMORY:
	if (isTLS) {
	    desc = internal_error;
	    goto alert_loser;
	}
	goto loser;
    }
    /* fall through to bad_cert. */

bad_cert:	/* caller has set errCode. */
    switch (errCode) {
    case SEC_ERROR_LIBRARY_FAILURE:     desc = unsupported_certificate; break;
    case SEC_ERROR_EXPIRED_CERTIFICATE: desc = certificate_expired;     break;
    case SEC_ERROR_REVOKED_CERTIFICATE: desc = certificate_revoked;     break;
    case SEC_ERROR_INADEQUATE_KEY_USAGE:
    case SEC_ERROR_INADEQUATE_CERT_TYPE:
		                        desc = certificate_unknown;     break;
    case SEC_ERROR_UNTRUSTED_CERT:
		    desc = isTLS ? access_denied : certificate_unknown; break;
    case SEC_ERROR_UNKNOWN_ISSUER:      
    case SEC_ERROR_UNTRUSTED_ISSUER:    
		    desc = isTLS ? unknown_ca : certificate_unknown; break;
    case SEC_ERROR_EXPIRED_ISSUER_CERTIFICATE:
		    desc = isTLS ? unknown_ca : certificate_expired; break;

    case SEC_ERROR_CERT_NOT_IN_NAME_SPACE:
    case SEC_ERROR_PATH_LEN_CONSTRAINT_INVALID:
    case SEC_ERROR_CA_CERT_INVALID:
    case SEC_ERROR_BAD_SIGNATURE:
    default:                            desc = bad_certificate;     break;
    }
    SSL_DBG(("%d: SSL3[%d]: peer certificate is no good: error=%d",
	     SSL_GETPID(), ss->fd, errCode));

    goto alert_loser;

decode_loser:
    desc = isTLS ? decode_error : bad_certificate;

alert_loser:
    (void)SSL3_SendAlert(ss, alert_fatal, desc);

loser:
    ss->ssl3.peerCertChain = certs;  certs = NULL;  arena = NULL;
    ssl3_CleanupPeerCerts(ss);

    if (ss->sec.peerCert != NULL) {
	CERT_DestroyCertificate(ss->sec.peerCert);
	ss->sec.peerCert = NULL;
    }
    (void)ssl_MapLowLevelError(errCode);
    return SECFailure;
}


/* restart an SSL connection that we stopped to run certificate dialogs
** XXX	Need to document here how an application marks a cert to show that
**	the application has accepted it (overridden CERT_VerifyCert).
 *
 * XXX This code only works on the initial handshake on a connection, XXX
 *     It does not work on a subsequent handshake (redo).
 *
 * Return value: XXX
 *
 * Caller holds 1stHandshakeLock.
*/
int
ssl3_RestartHandshakeAfterServerCert(sslSocket *ss)
{
    CERTCertificate * cert;
    int               rv	= SECSuccess;

    if (MSB(ss->version) != MSB(SSL_LIBRARY_VERSION_3_0)) {
	SET_ERROR_CODE
    	return SECFailure;
    }
    if (!ss->ssl3.initialized) {
	SET_ERROR_CODE
    	return SECFailure;
    }

    cert = ss->sec.peerCert;

    /* Permit step up if user decided to accept the cert */
    if (!ss->sec.isServer &&
    	ssl3_global_policy_some_restricted &&
        ss->ssl3.policy == SSL_ALLOWED &&
	anyRestrictedEnabled(ss) &&
	(SECSuccess == CERT_VerifyCertNow(cert->dbhandle, cert,
	                                  PR_FALSE, /* checksig */
				          certUsageSSLServerWithStepUp,
/*XXX*/				          ss->authCertificateArg) )) {
	ss->ssl3.policy         = SSL_RESTRICTED;
	ss->ssl3.hs.rehandshake = PR_TRUE;
    }

    if (ss->handshake != NULL) {
	ss->handshake = ssl_GatherRecord1stHandshake;
	ss->sec.ci.sid->peerCert = CERT_DupCertificate(ss->sec.peerCert);

	ssl_GetRecvBufLock(ss);
	if (ss->ssl3.hs.msgState.buf != NULL) {
	    rv = ssl3_HandleRecord(ss, NULL, &ss->gs.buf);
	}
	ssl_ReleaseRecvBufLock(ss);
    }

    return rv;
}

static SECStatus
ssl3_ComputeTLSFinished(ssl3CipherSpec *spec,
			PRBool          isServer,
                const   SSL3Finished *  hashes,
                        TLSFinished  *  tlsFinished)
{
    const char * label;
    unsigned int len;
    SECStatus    rv;

    label = isServer ? "server finished" : "client finished";
    len   = 15;

    if (spec->master_secret && !spec->bypassCiphers) {
	SECItem      param       = {siBuffer, NULL, 0};
	PK11Context *prf_context =
	    PK11_CreateContextBySymKey(CKM_TLS_PRF_GENERAL, CKA_SIGN, 
				       spec->master_secret, &param);
	if (!prf_context)
	    return SECFailure;

	rv  = PK11_DigestBegin(prf_context);
	rv |= PK11_DigestOp(prf_context, (const unsigned char *) label, len);
	rv |= PK11_DigestOp(prf_context, hashes->md5, sizeof *hashes);
	rv |= PK11_DigestFinal(prf_context, tlsFinished->verify_data, 
			       &len, sizeof tlsFinished->verify_data);
	PORT_Assert(rv != SECSuccess || len == sizeof *tlsFinished);

	PK11_DestroyContext(prf_context, PR_TRUE);
    } else {
	/* bypass PKCS11 */
	SECItem inData  = { siBuffer, };
	SECItem outData = { siBuffer, };
	PRBool isFIPS   = PR_FALSE;

	inData.data  = (unsigned char *)hashes->md5;
	inData.len   = sizeof hashes[0];
	outData.data = tlsFinished->verify_data;
	outData.len  = sizeof tlsFinished->verify_data;
	rv = TLS_PRF(&spec->msItem, label, &inData, &outData, isFIPS);
	PORT_Assert(rv != SECSuccess || \
		    outData.len == sizeof tlsFinished->verify_data);
    }
    return rv;
}

/* called from ssl3_HandleServerHelloDone
 *             ssl3_HandleClientHello
 *             ssl3_HandleFinished
 */
static SECStatus
ssl3_SendFinished(sslSocket *ss, PRInt32 flags)
{
    ssl3CipherSpec *cwSpec;
    PRBool          isTLS;
    PRBool          isServer = ss->sec.isServer;
    SECStatus       rv;
    SSL3Sender      sender = isServer ? sender_server : sender_client;
    SSL3Finished    hashes;
    TLSFinished     tlsFinished;

    SSL_TRC(3, ("%d: SSL3[%d]: send finished handshake", SSL_GETPID(), ss->fd));

    PORT_Assert( ss->opt.noLocks || ssl_HaveXmitBufLock(ss));
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss));

    ssl_GetSpecReadLock(ss);
    cwSpec = ss->ssl3.cwSpec;
    isTLS = (PRBool)(cwSpec->version > SSL_LIBRARY_VERSION_3_0);
    rv = ssl3_ComputeHandshakeHashes(ss, cwSpec, &hashes, sender);
    if (isTLS && rv == SECSuccess) {
	rv = ssl3_ComputeTLSFinished(cwSpec, isServer, &hashes, &tlsFinished);
    }
    ssl_ReleaseSpecReadLock(ss);
    if (rv != SECSuccess) {
	goto fail;	/* err code was set by ssl3_ComputeHandshakeHashes */
    }

    if (isTLS) {
	rv = ssl3_AppendHandshakeHeader(ss, finished, sizeof tlsFinished);
	if (rv != SECSuccess) 
	    goto fail; 		/* err set by AppendHandshake. */
	rv = ssl3_AppendHandshake(ss, &tlsFinished, sizeof tlsFinished);
	if (rv != SECSuccess) 
	    goto fail; 		/* err set by AppendHandshake. */
    } else {
	rv = ssl3_AppendHandshakeHeader(ss, finished, sizeof hashes);
	if (rv != SECSuccess) 
	    goto fail; 		/* err set by AppendHandshake. */
	rv = ssl3_AppendHandshake(ss, &hashes, sizeof hashes);
	if (rv != SECSuccess) 
	    goto fail; 		/* err set by AppendHandshake. */
    }
    rv = ssl3_FlushHandshake(ss, flags);
    if (rv != SECSuccess) {
	goto fail;	/* error code set by ssl3_FlushHandshake */
    }
    return SECSuccess;

fail:
    return rv;
}

/* wrap the master secret, and put it into the SID.
 * Caller holds the Spec read lock.
 */
static SECStatus
ssl3_CacheWrappedMasterSecret(sslSocket *ss, SSL3KEAType effectiveExchKeyType)
{
    sslSessionID *    sid	   = ss->sec.ci.sid;
    PK11SymKey *      wrappingKey  = NULL;
    PK11SlotInfo *    symKeySlot;
    void *            pwArg        = ss->pkcs11PinArg;
    SECStatus         rv           = SECFailure;
    PRBool            isServer     = ss->sec.isServer;
    CK_MECHANISM_TYPE mechanism    = CKM_INVALID_MECHANISM;
    symKeySlot = PK11_GetSlotFromKey(ss->ssl3.crSpec->master_secret);
    if (!isServer) {
	int  wrapKeyIndex;
	int  incarnation;

	/* these next few functions are mere accessors and don't fail. */
	sid->u.ssl3.masterWrapIndex  = wrapKeyIndex =
				       PK11_GetCurrentWrapIndex(symKeySlot);
	PORT_Assert(wrapKeyIndex == 0);	/* array has only one entry! */

	sid->u.ssl3.masterWrapSeries = incarnation =
				       PK11_GetSlotSeries(symKeySlot);
	sid->u.ssl3.masterSlotID   = PK11_GetSlotID(symKeySlot);
	sid->u.ssl3.masterModuleID = PK11_GetModuleID(symKeySlot);
	sid->u.ssl3.masterValid    = PR_TRUE;
	/* Get the default wrapping key, for wrapping the master secret before
	 * placing it in the SID cache entry. */
	wrappingKey = PK11_GetWrapKey(symKeySlot, wrapKeyIndex,
				      CKM_INVALID_MECHANISM, incarnation,
				      pwArg);
	if (wrappingKey) {
	    mechanism = PK11_GetMechanism(wrappingKey); /* can't fail. */
	} else {
	    int keyLength;
	    /* if the wrappingKey doesn't exist, attempt to create it.
	     * Note: we intentionally ignore errors here.  If we cannot
	     * generate a wrapping key, it is not fatal to this SSL connection,
	     * but we will not be able to restart this session.
	     */
	    mechanism = PK11_GetBestWrapMechanism(symKeySlot);
	    keyLength = PK11_GetBestKeyLength(symKeySlot, mechanism);
	    /* Zero length means fixed key length algorithm, or error.
	     * It's ambiguous.
	     */
	    wrappingKey = PK11_KeyGen(symKeySlot, mechanism, NULL,
				      keyLength, pwArg);
	    if (wrappingKey) {
		PK11_SetWrapKey(symKeySlot, wrapKeyIndex, wrappingKey);
	    }
	}
    } else {
	/* server socket using session cache. */
	mechanism = PK11_GetBestWrapMechanism(symKeySlot);
	if (mechanism != CKM_INVALID_MECHANISM) {
	    wrappingKey =
		getWrappingKey(ss, symKeySlot, effectiveExchKeyType,
			       mechanism, pwArg);
	    if (wrappingKey) {
		mechanism = PK11_GetMechanism(wrappingKey); /* can't fail. */
	    }
	}
    }

    sid->u.ssl3.masterWrapMech = mechanism;
    PK11_FreeSlot(symKeySlot);

    if (wrappingKey) {
	SECItem wmsItem;

	wmsItem.data = sid->u.ssl3.keys.wrapped_master_secret;
	wmsItem.len  = sizeof sid->u.ssl3.keys.wrapped_master_secret;
	rv = PK11_WrapSymKey(mechanism, NULL, wrappingKey,
			     ss->ssl3.crSpec->master_secret, &wmsItem);
	/* rv is examined below. */
	sid->u.ssl3.keys.wrapped_master_secret_len = wmsItem.len;
	PK11_FreeSymKey(wrappingKey);
    }
    return rv;
}

/* Called from ssl3_HandleHandshakeMessage() when it has deciphered a complete
 * ssl3 Finished message from the peer.
 * Caller must hold Handshake and RecvBuf locks.
 */
static SECStatus
ssl3_HandleFinished(sslSocket *ss, SSL3Opaque *b, PRUint32 length,
		    const SSL3Hashes *hashes)
{
    sslSessionID *    sid	   = ss->sec.ci.sid;
    SECStatus         rv           = SECSuccess;
    PRBool            isServer     = ss->sec.isServer;
    PRBool            isTLS;
    PRBool            doStepUp;
    SSL3KEAType       effectiveExchKeyType;

    PORT_Assert( ss->opt.noLocks || ssl_HaveRecvBufLock(ss) );
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) );

    SSL_TRC(3, ("%d: SSL3[%d]: handle finished handshake",
    	SSL_GETPID(), ss->fd));

    if (ss->ssl3.hs.ws != wait_finished) {
	SSL3_SendAlert(ss, alert_fatal, unexpected_message);
    	PORT_SetError(SSL_ERROR_RX_UNEXPECTED_FINISHED);
	return SECFailure;
    }

    isTLS = (PRBool)(ss->ssl3.crSpec->version > SSL_LIBRARY_VERSION_3_0);
    if (isTLS) {
	TLSFinished tlsFinished;

	if (length != sizeof tlsFinished) {
	    (void)SSL3_SendAlert(ss, alert_fatal, decode_error);
	    PORT_SetError(SSL_ERROR_RX_MALFORMED_FINISHED);
	    return SECFailure;
	}
	rv = ssl3_ComputeTLSFinished(ss->ssl3.crSpec, !isServer, 
	                             hashes, &tlsFinished);
	if (rv != SECSuccess ||
	    0 != PORT_Memcmp(&tlsFinished, b, length)) {
	    (void)SSL3_SendAlert(ss, alert_fatal, decrypt_error);
	    PORT_SetError(SSL_ERROR_BAD_HANDSHAKE_HASH_VALUE);
	    return SECFailure;
	}
    } else {
	if (length != sizeof(SSL3Hashes)) {
	    (void)ssl3_IllegalParameter(ss);
	    PORT_SetError(SSL_ERROR_RX_MALFORMED_FINISHED);
	    return SECFailure;
	}

	if (0 != PORT_Memcmp(hashes, b, length)) {
	    (void)ssl3_HandshakeFailure(ss);
	    PORT_SetError(SSL_ERROR_BAD_HANDSHAKE_HASH_VALUE);
	    return SECFailure;
	}
    }

    doStepUp = (PRBool)(!isServer && ss->ssl3.hs.rehandshake);

    ssl_GetXmitBufLock(ss);	/*************************************/

    if ((isServer && !ss->ssl3.hs.isResuming) ||
	(!isServer && ss->ssl3.hs.isResuming)) {
	PRInt32 flags = 0;

	rv = ssl3_SendChangeCipherSpecs(ss);
	if (rv != SECSuccess) {
	    goto xmit_loser;	/* err is set. */
	}
	/* If this thread is in SSL_SecureSend (trying to write some data) 
	** or if it is going to step up, 
	** then set the ssl_SEND_FLAG_FORCE_INTO_BUFFER flag, so that the 
	** last two handshake messages (change cipher spec and finished) 
	** will be sent in the same send/write call as the application data.
	*/
	if (doStepUp || ss->writerThread == PR_GetCurrentThread()) {
	    flags = ssl_SEND_FLAG_FORCE_INTO_BUFFER;
	}
	rv = ssl3_SendFinished(ss, flags);
	if (rv != SECSuccess) {
	    goto xmit_loser;	/* err is set. */
	}
    }

    /* Optimization: don't cache this connection if we're going to step up. */
    if (doStepUp) {
	ssl_FreeSID(sid);
	ss->sec.ci.sid     = sid = NULL;
	ss->ssl3.hs.rehandshake = PR_FALSE;
	rv = ssl3_SendClientHello(ss);
xmit_loser:
	ssl_ReleaseXmitBufLock(ss);
	return rv;	/* err code is set if appropriate. */
    }

    ssl_ReleaseXmitBufLock(ss);	/*************************************/

    /* The first handshake is now completed. */
    ss->handshake           = NULL;
    ss->firstHsDone         = PR_TRUE;
    ss->gs.writeOffset = 0;
    ss->gs.readOffset  = 0;

    if (ss->ssl3.hs.kea_def->kea == kea_ecdhe_rsa) {
	effectiveExchKeyType = kt_rsa;
    } else {
	effectiveExchKeyType = ss->ssl3.hs.kea_def->exchKeyType;
    }

    if (sid->cached == never_cached && !ss->opt.noCache && ss->sec.cache) {
	/* fill in the sid */
	sid->u.ssl3.cipherSuite = ss->ssl3.hs.cipher_suite;
	sid->u.ssl3.compression = ss->ssl3.hs.compression;
	sid->u.ssl3.policy      = ss->ssl3.policy;
#ifdef NSS_ENABLE_ECC
	sid->u.ssl3.negotiatedECCurves = ss->ssl3.hs.negotiatedECCurves;
#endif
	sid->u.ssl3.exchKeyType = effectiveExchKeyType;
	sid->version            = ss->version;
	sid->authAlgorithm      = ss->sec.authAlgorithm;
	sid->authKeyBits        = ss->sec.authKeyBits;
	sid->keaType            = ss->sec.keaType;
	sid->keaKeyBits         = ss->sec.keaKeyBits;
	sid->lastAccessTime     = sid->creationTime = ssl_Time();
	sid->expirationTime     = sid->creationTime + ssl3_sid_timeout;
	sid->localCert          = CERT_DupCertificate(ss->sec.localCert);

	ssl_GetSpecReadLock(ss);	/*************************************/

	/* Copy the master secret (wrapped or unwrapped) into the sid */
	if (ss->ssl3.crSpec->msItem.len && ss->ssl3.crSpec->msItem.data) {
	    sid->u.ssl3.keys.wrapped_master_secret_len = 
			    ss->ssl3.crSpec->msItem.len;
	    memcpy(sid->u.ssl3.keys.wrapped_master_secret, 
		   ss->ssl3.crSpec->msItem.data, ss->ssl3.crSpec->msItem.len);
	    sid->u.ssl3.masterValid    = PR_TRUE;
	    sid->u.ssl3.keys.msIsWrapped = PR_FALSE;
	    rv = SECSuccess;
	} else {
	    rv = ssl3_CacheWrappedMasterSecret(ss, effectiveExchKeyType);
	    sid->u.ssl3.keys.msIsWrapped = PR_TRUE;
	}
	ssl_ReleaseSpecReadLock(ss);  /*************************************/

	/* If the wrap failed, we don't cache the sid.
	 * The connection continues normally however.
	 */
	if (rv == SECSuccess) {
	    (*ss->sec.cache)(sid);
	}
    }
    ss->ssl3.hs.ws = idle_handshake;

    /* Do the handshake callback for sslv3 here. */
    if (ss->handshakeCallback != NULL) {
	(ss->handshakeCallback)(ss->fd, ss->handshakeCallbackData);
    }

    return SECSuccess;
}

/* Called from ssl3_HandleHandshake() when it has gathered a complete ssl3
 * hanshake message.
 * Caller must hold Handshake and RecvBuf locks.
 */
static SECStatus
ssl3_HandleHandshakeMessage(sslSocket *ss, SSL3Opaque *b, PRUint32 length)
{
    SECStatus         rv 	= SECSuccess;
    SSL3HandshakeType type 	= ss->ssl3.hs.msg_type;
    SSL3Hashes        hashes;	/* computed hashes are put here. */
    PRUint8           hdr[4];

    PORT_Assert( ss->opt.noLocks || ssl_HaveRecvBufLock(ss) );
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) );
    /*
     * We have to compute the hashes before we update them with the
     * current message.
     */
    ssl_GetSpecReadLock(ss);	/************************************/
    if((type == finished) || (type == certificate_verify)) {
	SSL3Sender      sender = (SSL3Sender)0;
	ssl3CipherSpec *rSpec  = ss->ssl3.prSpec;

	if (type == finished) {
	    sender = ss->sec.isServer ? sender_client : sender_server;
	    rSpec  = ss->ssl3.crSpec;
	}
	rv = ssl3_ComputeHandshakeHashes(ss, rSpec, &hashes, sender);
    }
    ssl_ReleaseSpecReadLock(ss); /************************************/
    if (rv != SECSuccess) {
	return rv;	/* error code was set by ssl3_ComputeHandshakeHashes*/
    }
    SSL_TRC(30,("%d: SSL3[%d]: handle handshake message: %s", SSL_GETPID(),
		ss->fd, ssl3_DecodeHandshakeType(ss->ssl3.hs.msg_type)));
    PRINT_BUF(60, (ss, "MD5 handshake hash:",
    	      (unsigned char*)ss->ssl3.hs.md5_cx, MD5_LENGTH));
    PRINT_BUF(95, (ss, "SHA handshake hash:",
    	      (unsigned char*)ss->ssl3.hs.sha_cx, SHA1_LENGTH));

    hdr[0] = (PRUint8)ss->ssl3.hs.msg_type;
    hdr[1] = (PRUint8)(length >> 16);
    hdr[2] = (PRUint8)(length >>  8);
    hdr[3] = (PRUint8)(length      );

    /* Start new handshake hashes when we start a new handshake */
    if (ss->ssl3.hs.msg_type == client_hello) {
	SSL_TRC(30,("%d: SSL3[%d]: reset handshake hashes",
		SSL_GETPID(), ss->fd ));
	rv = ssl3_RestartHandshakeHashes(ss);
	if (rv != SECSuccess) {
	    return rv;
	}
    }
    /* We should not include hello_request messages in the handshake hashes */
    if (ss->ssl3.hs.msg_type != hello_request) {
	rv = ssl3_UpdateHandshakeHashes(ss, (unsigned char*) hdr, 4);
	if (rv != SECSuccess) return rv;	/* err code already set. */
	rv = ssl3_UpdateHandshakeHashes(ss, b, length);
	if (rv != SECSuccess) return rv;	/* err code already set. */
    }

    PORT_SetError(0);	/* each message starts with no error. */
    switch (ss->ssl3.hs.msg_type) {
    case hello_request:
	if (length != 0) {
	    (void)ssl3_DecodeError(ss);
	    PORT_SetError(SSL_ERROR_RX_MALFORMED_HELLO_REQUEST);
	    return SECFailure;
	}
	if (ss->sec.isServer) {
	    (void)SSL3_SendAlert(ss, alert_fatal, unexpected_message);
	    PORT_SetError(SSL_ERROR_RX_UNEXPECTED_HELLO_REQUEST);
	    return SECFailure;
	}
	rv = ssl3_HandleHelloRequest(ss);
	break;
    case client_hello:
	if (!ss->sec.isServer) {
	    (void)SSL3_SendAlert(ss, alert_fatal, unexpected_message);
	    PORT_SetError(SSL_ERROR_RX_UNEXPECTED_CLIENT_HELLO);
	    return SECFailure;
	}
	rv = ssl3_HandleClientHello(ss, b, length);
	break;
    case server_hello:
	if (ss->sec.isServer) {
	    (void)SSL3_SendAlert(ss, alert_fatal, unexpected_message);
	    PORT_SetError(SSL_ERROR_RX_UNEXPECTED_SERVER_HELLO);
	    return SECFailure;
	}
	rv = ssl3_HandleServerHello(ss, b, length);
	break;
    case certificate:
	rv = ssl3_HandleCertificate(ss, b, length);
	break;
    case server_key_exchange:
	if (ss->sec.isServer) {
	    (void)SSL3_SendAlert(ss, alert_fatal, unexpected_message);
	    PORT_SetError(SSL_ERROR_RX_UNEXPECTED_SERVER_KEY_EXCH);
	    return SECFailure;
	}
	rv = ssl3_HandleServerKeyExchange(ss, b, length);
	break;
    case certificate_request:
	if (ss->sec.isServer) {
	    (void)SSL3_SendAlert(ss, alert_fatal, unexpected_message);
	    PORT_SetError(SSL_ERROR_RX_UNEXPECTED_CERT_REQUEST);
	    return SECFailure;
	}
	rv = ssl3_HandleCertificateRequest(ss, b, length);
	break;
    case server_hello_done:
	if (length != 0) {
	    (void)ssl3_DecodeError(ss);
	    PORT_SetError(SSL_ERROR_RX_MALFORMED_HELLO_DONE);
	    return SECFailure;
	}
	if (ss->sec.isServer) {
	    (void)SSL3_SendAlert(ss, alert_fatal, unexpected_message);
	    PORT_SetError(SSL_ERROR_RX_UNEXPECTED_HELLO_DONE);
	    return SECFailure;
	}
	rv = ssl3_HandleServerHelloDone(ss);
	break;
    case certificate_verify:
	if (!ss->sec.isServer) {
	    (void)SSL3_SendAlert(ss, alert_fatal, unexpected_message);
	    PORT_SetError(SSL_ERROR_RX_UNEXPECTED_CERT_VERIFY);
	    return SECFailure;
	}
	rv = ssl3_HandleCertificateVerify(ss, b, length, &hashes);
	break;
    case client_key_exchange:
	if (!ss->sec.isServer) {
	    (void)SSL3_SendAlert(ss, alert_fatal, unexpected_message);
	    PORT_SetError(SSL_ERROR_RX_UNEXPECTED_CLIENT_KEY_EXCH);
	    return SECFailure;
	}
	rv = ssl3_HandleClientKeyExchange(ss, b, length);
	break;
    case finished:
        rv = ssl3_HandleFinished(ss, b, length, &hashes);
	break;
    default:
	(void)SSL3_SendAlert(ss, alert_fatal, unexpected_message);
	PORT_SetError(SSL_ERROR_RX_UNKNOWN_HANDSHAKE);
	rv = SECFailure;
    }
    return rv;
}

/* Called only from ssl3_HandleRecord, for each (deciphered) ssl3 record.
 * origBuf is the decrypted ssl record content.
 * Caller must hold the handshake and RecvBuf locks.
 */
static SECStatus
ssl3_HandleHandshake(sslSocket *ss, sslBuffer *origBuf)
{
    /*
     * There may be a partial handshake message already in the handshake
     * state. The incoming buffer may contain another portion, or a
     * complete message or several messages followed by another portion.
     *
     * Each message is made contiguous before being passed to the actual
     * message parser.
     */
    sslBuffer *buf = &ss->ssl3.hs.msgState; /* do not lose the original buffer pointer */
    SECStatus rv;

    PORT_Assert( ss->opt.noLocks || ssl_HaveRecvBufLock(ss) );
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) );

    if (buf->buf == NULL) {
	*buf = *origBuf;
    }
    while (buf->len > 0) {
	if (ss->ssl3.hs.header_bytes < 4) {
	    uint8 t;
	    t = *(buf->buf++);
	    buf->len--;
	    if (ss->ssl3.hs.header_bytes++ == 0)
		ss->ssl3.hs.msg_type = (SSL3HandshakeType)t;
	    else
		ss->ssl3.hs.msg_len = (ss->ssl3.hs.msg_len << 8) + t;
	    if (ss->ssl3.hs.header_bytes < 4)
	    	continue;

#define MAX_HANDSHAKE_MSG_LEN 0x1ffff	/* 128k - 1 */
	    if (ss->ssl3.hs.msg_len > MAX_HANDSHAKE_MSG_LEN) {
		(void)ssl3_DecodeError(ss);
		PORT_SetError(SSL_ERROR_RX_RECORD_TOO_LONG);
		return SECFailure;
	    }
#undef MAX_HANDSHAKE_MSG_LEN

	    /* If msg_len is zero, be sure we fall through, 
	    ** even if buf->len is zero. 
	    */
	    if (ss->ssl3.hs.msg_len > 0) 
	    	continue;
	}

	/*
	 * Header has been gathered and there is at least one byte of new
	 * data available for this message. If it can be done right out
	 * of the original buffer, then use it from there.
	 */
	if (ss->ssl3.hs.msg_body.len == 0 && buf->len >= ss->ssl3.hs.msg_len) {
	    /* handle it from input buffer */
	    rv = ssl3_HandleHandshakeMessage(ss, buf->buf, ss->ssl3.hs.msg_len);
	    if (rv == SECFailure) {
		/* This test wants to fall through on either
		 * SECSuccess or SECWouldBlock.
		 * ssl3_HandleHandshakeMessage MUST set the error code.
		 */
		return rv;
	    }
	    buf->buf += ss->ssl3.hs.msg_len;
	    buf->len -= ss->ssl3.hs.msg_len;
	    ss->ssl3.hs.msg_len = 0;
	    ss->ssl3.hs.header_bytes = 0;
	    if (rv != SECSuccess) { /* return if SECWouldBlock. */
		return rv;
	    }
	} else {
	    /* must be copied to msg_body and dealt with from there */
	    unsigned int bytes;

	    PORT_Assert(ss->ssl3.hs.msg_body.len <= ss->ssl3.hs.msg_len);
	    bytes = PR_MIN(buf->len, ss->ssl3.hs.msg_len - ss->ssl3.hs.msg_body.len);

	    /* Grow the buffer if needed */
	    rv = sslBuffer_Grow(&ss->ssl3.hs.msg_body, ss->ssl3.hs.msg_len);
	    if (rv != SECSuccess) {
		/* sslBuffer_Grow has set a memory error code. */
		return SECFailure;
	    }

	    PORT_Memcpy(ss->ssl3.hs.msg_body.buf + ss->ssl3.hs.msg_body.len,
		        buf->buf, bytes);
	    ss->ssl3.hs.msg_body.len += bytes;
	    buf->buf += bytes;
	    buf->len -= bytes;

	    PORT_Assert(ss->ssl3.hs.msg_body.len <= ss->ssl3.hs.msg_len);

	    /* if we have a whole message, do it */
	    if (ss->ssl3.hs.msg_body.len == ss->ssl3.hs.msg_len) {
		rv = ssl3_HandleHandshakeMessage(
		    ss, ss->ssl3.hs.msg_body.buf, ss->ssl3.hs.msg_len);
		/*
		 * XXX This appears to be wrong.  This error handling
		 * should clean up after a SECWouldBlock return, like the
		 * error handling used 40 lines before/above this one,
		 */
		if (rv != SECSuccess) {
		    /* ssl3_HandleHandshakeMessage MUST set error code. */
		    return rv;
		}
		ss->ssl3.hs.msg_body.len = 0;
		ss->ssl3.hs.msg_len      = 0;
		ss->ssl3.hs.header_bytes = 0;
	    } else {
		PORT_Assert(buf->len == 0);
		break;
	    }
	}
    }	/* end loop */

    origBuf->len = 0;	/* So ssl3_GatherAppDataRecord will keep looping. */
    buf->buf = NULL;	/* not a leak. */
    return SECSuccess;
}

/* if cText is non-null, then decipher, check MAC, and decompress the
 * SSL record from cText->buf (typically gs->inbuf)
 * into databuf (typically gs->buf), and any previous contents of databuf
 * is lost.  Then handle databuf according to its SSL record type,
 * unless it's an application record.
 *
 * If cText is NULL, then the ciphertext has previously been deciphered and
 * checked, and is already sitting in databuf.  It is processed as an SSL
 * Handshake message.
 *
 * DOES NOT process the decrypted/decompressed application data.
 * On return, databuf contains the decrypted/decompressed record.
 *
 * Called from ssl3_GatherCompleteHandshake
 *             ssl3_RestartHandshakeAfterCertReq
 *             ssl3_RestartHandshakeAfterServerCert
 *
 * Caller must hold the RecvBufLock.
 *
 * This function aquires and releases the SSL3Handshake Lock, holding the
 * lock around any calls to functions that handle records other than
 * Application Data records.
 */
SECStatus
ssl3_HandleRecord(sslSocket *ss, SSL3Ciphertext *cText, sslBuffer *databuf)
{
const ssl3BulkCipherDef *cipher_def;
    ssl3CipherSpec *     crSpec;
    SECStatus            rv;
    unsigned int         hashBytes		= MAX_MAC_LENGTH + 1;
    unsigned int         padding_length;
    PRBool               isTLS;
    PRBool               padIsBad               = PR_FALSE;
    SSL3ContentType      rType;
    SSL3Opaque           hash[MAX_MAC_LENGTH];

    PORT_Assert( ss->opt.noLocks || ssl_HaveRecvBufLock(ss) );

    if (!ss->ssl3.initialized) {
	ssl_GetSSL3HandshakeLock(ss);
	rv = ssl3_InitState(ss);
	ssl_ReleaseSSL3HandshakeLock(ss);
	if (rv != SECSuccess) {
	    return rv;		/* ssl3_InitState has set the error code. */
    	}
    }

    /* check for Token Presence */
    if (!ssl3_ClientAuthTokenPresent(ss->sec.ci.sid)) {
	PORT_SetError(SSL_ERROR_TOKEN_INSERTION_REMOVAL);
	return SECFailure;
    }

    /* cText is NULL when we're called from ssl3_RestartHandshakeAfterXXX().
     * This implies that databuf holds a previously deciphered SSL Handshake
     * message.
     */
    if (cText == NULL) {
	SSL_DBG(("%d: SSL3[%d]: HandleRecord, resuming handshake",
		 SSL_GETPID(), ss->fd));
	rType = content_handshake;
	goto process_it;
    }

    databuf->len = 0; /* filled in by decode call below. */
    if (databuf->space < MAX_FRAGMENT_LENGTH) {
	rv = sslBuffer_Grow(databuf, MAX_FRAGMENT_LENGTH + 2048);
	if (rv != SECSuccess) {
	    SSL_DBG(("%d: SSL3[%d]: HandleRecord, tried to get %d bytes",
		     SSL_GETPID(), ss->fd, MAX_FRAGMENT_LENGTH + 2048));
	    /* sslBuffer_Grow has set a memory error code. */
	    /* Perhaps we should send an alert. (but we have no memory!) */
	    return SECFailure;
	}
    }

    PRINT_BUF(80, (ss, "ciphertext:", cText->buf->buf, cText->buf->len));

    ssl_GetSpecReadLock(ss); /******************************************/

    crSpec = ss->ssl3.crSpec;
    cipher_def = crSpec->cipher_def;
    isTLS = (PRBool)(crSpec->version > SSL_LIBRARY_VERSION_3_0);

    if (isTLS && cText->buf->len > (MAX_FRAGMENT_LENGTH + 2048)) {
	ssl_ReleaseSpecReadLock(ss);
	SSL3_SendAlert(ss, alert_fatal, record_overflow);
	PORT_SetError(SSL_ERROR_RX_RECORD_TOO_LONG);
	return SECFailure;
    }
    /* decrypt from cText buf to databuf. */
    rv = crSpec->decode(
	crSpec->decodeContext, databuf->buf, (int *)&databuf->len,
	databuf->space, cText->buf->buf, cText->buf->len);

    PRINT_BUF(80, (ss, "cleartext:", databuf->buf, databuf->len));
    if (rv != SECSuccess) {
	int err = ssl_MapLowLevelError(SSL_ERROR_DECRYPTION_FAILURE);
	ssl_ReleaseSpecReadLock(ss);
	SSL3_SendAlert(ss, alert_fatal,
	               isTLS ? decryption_failed : bad_record_mac);
	PORT_SetError(err);
	return SECFailure;
    }

    /* If it's a block cipher, check and strip the padding. */
    if (cipher_def->type == type_block) {
	padding_length = *(databuf->buf + databuf->len - 1);
	/* TLS permits padding to exceed the block size, up to 255 bytes. */
	if (padding_length + 1 + crSpec->mac_size > databuf->len)
	    padIsBad = PR_TRUE;
	/* if TLS, check value of first padding byte. */
	else if (padding_length && isTLS && 
	         padding_length != 
	                 *(databuf->buf + databuf->len - (padding_length + 1)))
	    padIsBad = PR_TRUE;
	else
	    databuf->len -= padding_length + 1;
    }

    /* Remove the MAC. */
    if (databuf->len >= crSpec->mac_size)
	databuf->len -= crSpec->mac_size;
    else
    	padIsBad = PR_TRUE;	/* really macIsBad */

    /* compute the MAC */
    rType = cText->type;
    rv = ssl3_ComputeRecordMAC( crSpec, (PRBool)(!ss->sec.isServer),
	rType, cText->version, crSpec->read_seq_num, 
	databuf->buf, databuf->len, hash, &hashBytes);
    if (rv != SECSuccess) {
	int err = ssl_MapLowLevelError(SSL_ERROR_MAC_COMPUTATION_FAILURE);
	ssl_ReleaseSpecReadLock(ss);
	SSL3_SendAlert(ss, alert_fatal, bad_record_mac);
	PORT_SetError(err);
	return rv;
    }

    /* Check the MAC */
    if (hashBytes != (unsigned)crSpec->mac_size || padIsBad || 
	PORT_Memcmp(databuf->buf + databuf->len, hash, crSpec->mac_size) != 0) {
	/* must not hold spec lock when calling SSL3_SendAlert. */
	ssl_ReleaseSpecReadLock(ss);
	SSL3_SendAlert(ss, alert_fatal, bad_record_mac);
	/* always log mac error, in case attacker can read server logs. */
	PORT_SetError(SSL_ERROR_BAD_MAC_READ);

	SSL_DBG(("%d: SSL3[%d]: mac check failed", SSL_GETPID(), ss->fd));

	return SECFailure;
    }

    ssl3_BumpSequenceNumber(&crSpec->read_seq_num);

    ssl_ReleaseSpecReadLock(ss); /*****************************************/

    /*
     * The decrypted data is now in databuf.
     *
     * the null decompression routine is right here
     */

    /*
    ** Having completed the decompression, check the length again. 
    */
    if (isTLS && databuf->len > (MAX_FRAGMENT_LENGTH + 1024)) {
	SSL3_SendAlert(ss, alert_fatal, record_overflow);
	PORT_SetError(SSL_ERROR_RX_RECORD_TOO_LONG);
	return SECFailure;
    }

    /* Application data records are processed by the caller of this
    ** function, not by this function.
    */
    if (rType == content_application_data) {
    	return SECSuccess;
    }

    /* It's a record that must be handled by ssl itself, not the application.
    */
process_it:
    /* XXX  Get the xmit lock here.  Odds are very high that we'll be xmiting
     * data ang getting the xmit lock here prevents deadlocks.
     */
    ssl_GetSSL3HandshakeLock(ss);

    /* All the functions called in this switch MUST set error code if
    ** they return SECFailure or SECWouldBlock.
    */
    switch (rType) {
    case content_change_cipher_spec:
	rv = ssl3_HandleChangeCipherSpecs(ss, databuf);
	break;
    case content_alert:
	rv = ssl3_HandleAlert(ss, databuf);
	break;
    case content_handshake:
	rv = ssl3_HandleHandshake(ss, databuf);
	break;
    /*
    case content_application_data is handled before this switch
    */
    default:
	SSL_DBG(("%d: SSL3[%d]: bogus content type=%d",
		 SSL_GETPID(), ss->fd, cText->type));
	/* XXX Send an alert ???  */
	PORT_SetError(SSL_ERROR_RX_UNKNOWN_RECORD_TYPE);
	rv = SECFailure;
	break;
    }

    ssl_ReleaseSSL3HandshakeLock(ss);
    return rv;

}

/*
 * Initialization functions
 */

/* Called from ssl3_InitState, immediately below. */
/* Caller must hold the SpecWriteLock. */
static void
ssl3_InitCipherSpec(sslSocket *ss, ssl3CipherSpec *spec)
{
    spec->cipher_def               = &bulk_cipher_defs[cipher_null];
    PORT_Assert(spec->cipher_def->cipher == cipher_null);
    spec->mac_def                  = &mac_defs[mac_null];
    PORT_Assert(spec->mac_def->mac == mac_null);
    spec->encode                   = Null_Cipher;
    spec->decode                   = Null_Cipher;
    spec->destroy                  = NULL;
    spec->mac_size                 = 0;
    spec->master_secret            = NULL;
    spec->bypassCiphers            = PR_FALSE;

    spec->msItem.data              = NULL;
    spec->msItem.len               = 0;

    spec->client.write_key         = NULL;
    spec->client.write_mac_key     = NULL;
    spec->client.write_mac_context = NULL;

    spec->server.write_key         = NULL;
    spec->server.write_mac_key     = NULL;
    spec->server.write_mac_context = NULL;

    spec->write_seq_num.high       = 0;
    spec->write_seq_num.low        = 0;

    spec->read_seq_num.high        = 0;
    spec->read_seq_num.low         = 0;

    spec->version                  = ss->opt.enableTLS
                                          ? SSL_LIBRARY_VERSION_3_1_TLS
                                          : SSL_LIBRARY_VERSION_3_0;
}

/* Called from:	ssl3_SendRecord
**		ssl3_StartHandshakeHash() <- ssl2_BeginClientHandshake()
**		ssl3_SendClientHello()
**		ssl3_HandleServerHello()
**		ssl3_HandleClientHello()
**		ssl3_HandleV2ClientHello()
**		ssl3_HandleRecord()
**
** This function should perhaps acquire and release the SpecWriteLock.
**
**
*/
static SECStatus
ssl3_InitState(sslSocket *ss)
{
    SECStatus    rv;
    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss));

    if (ss->ssl3.initialized)
    	return SECSuccess;	/* Function should be idempotent */

    ss->ssl3.policy = SSL_ALLOWED;

    ssl_GetSpecWriteLock(ss);
    ss->ssl3.crSpec = ss->ssl3.cwSpec = &ss->ssl3.specs[0];
    ss->ssl3.prSpec = ss->ssl3.pwSpec = &ss->ssl3.specs[1];
    ss->ssl3.hs.rehandshake = PR_FALSE;
    ssl3_InitCipherSpec(ss, ss->ssl3.crSpec);
    ssl3_InitCipherSpec(ss, ss->ssl3.prSpec);

    ss->ssl3.hs.ws = (ss->sec.isServer) ? wait_client_hello : wait_server_hello;
#ifdef NSS_ENABLE_ECC
    ss->ssl3.hs.negotiatedECCurves = SSL3_SUPPORTED_CURVES_MASK;
#endif
    ssl_ReleaseSpecWriteLock(ss);

    rv = ssl3_NewHandshakeHashes(ss);
    if (rv == SECSuccess) {
	ss->ssl3.initialized = PR_TRUE;
    }

    return rv;
}

/* Returns a reference counted object that contains a key pair.
 * Or NULL on failure.  Initial ref count is 1.
 * Uses the keys in the pair as input.
 */
ssl3KeyPair *
ssl3_NewKeyPair( SECKEYPrivateKey * privKey, SECKEYPublicKey * pubKey)
{
    ssl3KeyPair * pair;

    if (!privKey || !pubKey) {
	PORT_SetError(PR_INVALID_ARGUMENT_ERROR);
    	return NULL;
    }
    pair = PORT_ZNew(ssl3KeyPair);
    if (!pair)
    	return NULL;			/* error code is set. */
    pair->refCount = 1;
    pair->privKey  = privKey;
    pair->pubKey   = pubKey;
    return pair;			/* success */
}

ssl3KeyPair *
ssl3_GetKeyPairRef(ssl3KeyPair * keyPair)
{
    PR_AtomicIncrement(&keyPair->refCount);
    return keyPair;
}

void
ssl3_FreeKeyPair(ssl3KeyPair * keyPair)
{
    PRInt32 newCount =  PR_AtomicDecrement(&keyPair->refCount);
    if (!newCount) {
	if (keyPair->privKey)
	    SECKEY_DestroyPrivateKey(keyPair->privKey);
	if (keyPair->pubKey)
	    SECKEY_DestroyPublicKey( keyPair->pubKey);
    	PORT_Free(keyPair);
    }
}



/*
 * Creates the public and private RSA keys for SSL Step down.
 * Called from SSL_ConfigSecureServer in sslsecur.c
 */
SECStatus
ssl3_CreateRSAStepDownKeys(sslSocket *ss)
{
    SECStatus             rv  	 = SECSuccess;
    SECKEYPrivateKey *    privKey;		/* RSA step down key */
    SECKEYPublicKey *     pubKey;		/* RSA step down key */

    if (ss->stepDownKeyPair)
	ssl3_FreeKeyPair(ss->stepDownKeyPair);
    ss->stepDownKeyPair = NULL;
#ifndef HACKED_EXPORT_SERVER
    /* Sigh, should have a get key strength call for private keys */
    if (PK11_GetPrivateModulusLen(ss->serverCerts[kt_rsa].SERVERKEY) >
                                                     EXPORT_RSA_KEY_LENGTH) {
	/* need to ask for the key size in bits */
	privKey = SECKEY_CreateRSAPrivateKey(EXPORT_RSA_KEY_LENGTH * BPB,
					     &pubKey, NULL);
    	if (!privKey || !pubKey ||
	    !(ss->stepDownKeyPair = ssl3_NewKeyPair(privKey, pubKey))) {
	    ssl_MapLowLevelError(SEC_ERROR_KEYGEN_FAIL);
	    rv = SECFailure;
	}
    }
#endif
    return rv;
}


/* record the export policy for this cipher suite */
SECStatus
ssl3_SetPolicy(ssl3CipherSuite which, int policy)
{
    ssl3CipherSuiteCfg *suite;

    suite = ssl_LookupCipherSuiteCfg(which, cipherSuites);
    if (suite == NULL) {
	return SECFailure; /* err code was set by ssl_LookupCipherSuiteCfg */
    }
    suite->policy = policy;

    if (policy == SSL_RESTRICTED) {
	ssl3_global_policy_some_restricted = PR_TRUE;
    }

    return SECSuccess;
}

SECStatus
ssl3_GetPolicy(ssl3CipherSuite which, PRInt32 *oPolicy)
{
    ssl3CipherSuiteCfg *suite;
    PRInt32             policy;
    SECStatus           rv;

    suite = ssl_LookupCipherSuiteCfg(which, cipherSuites);
    if (suite) {
    	policy = suite->policy;
	rv     = SECSuccess;
    } else {
    	policy = SSL_NOT_ALLOWED;
	rv     = SECFailure;	/* err code was set by Lookup. */
    }
    *oPolicy = policy;
    return rv;
}

/* record the user preference for this suite */
SECStatus
ssl3_CipherPrefSetDefault(ssl3CipherSuite which, PRBool enabled)
{
    ssl3CipherSuiteCfg *suite;

    suite = ssl_LookupCipherSuiteCfg(which, cipherSuites);
    if (suite == NULL) {
	return SECFailure; /* err code was set by ssl_LookupCipherSuiteCfg */
    }
    suite->enabled = enabled;
    return SECSuccess;
}

/* return the user preference for this suite */
SECStatus
ssl3_CipherPrefGetDefault(ssl3CipherSuite which, PRBool *enabled)
{
    ssl3CipherSuiteCfg *suite;
    PRBool              pref;
    SECStatus           rv;

    suite = ssl_LookupCipherSuiteCfg(which, cipherSuites);
    if (suite) {
    	pref   = suite->enabled;
	rv     = SECSuccess;
    } else {
    	pref   = SSL_NOT_ALLOWED;
	rv     = SECFailure;	/* err code was set by Lookup. */
    }
    *enabled = pref;
    return rv;
}

SECStatus
ssl3_CipherPrefSet(sslSocket *ss, ssl3CipherSuite which, PRBool enabled)
{
    ssl3CipherSuiteCfg *suite;

    suite = ssl_LookupCipherSuiteCfg(which, ss->cipherSuites);
    if (suite == NULL) {
	return SECFailure; /* err code was set by ssl_LookupCipherSuiteCfg */
    }
    suite->enabled = enabled;
    return SECSuccess;
}

SECStatus
ssl3_CipherPrefGet(sslSocket *ss, ssl3CipherSuite which, PRBool *enabled)
{
    ssl3CipherSuiteCfg *suite;
    PRBool              pref;
    SECStatus           rv;

    suite = ssl_LookupCipherSuiteCfg(which, ss->cipherSuites);
    if (suite) {
    	pref   = suite->enabled;
	rv     = SECSuccess;
    } else {
    	pref   = SSL_NOT_ALLOWED;
	rv     = SECFailure;	/* err code was set by Lookup. */
    }
    *enabled = pref;
    return rv;
}

/* copy global default policy into socket. */
void
ssl3_InitSocketPolicy(sslSocket *ss)
{
    PORT_Memcpy(ss->cipherSuites, cipherSuites, sizeof cipherSuites);
}

/* ssl3_config_match_init must have already been called by
 * the caller of this function.
 */
SECStatus
ssl3_ConstructV2CipherSpecsHack(sslSocket *ss, unsigned char *cs, int *size)
{
    int i, count = 0;

    PORT_Assert(ss != 0);
    if (!ss) {
	PORT_SetError(PR_INVALID_ARGUMENT_ERROR);
	return SECFailure;
    }
    if (!ss->opt.enableSSL3 && !ss->opt.enableTLS) {
    	*size = 0;
	return SECSuccess;
    }
    if (cs == NULL) {
	*size = count_cipher_suites(ss, SSL_ALLOWED, PR_TRUE);
	return SECSuccess;
    }

    /* ssl3_config_match_init was called by the caller of this function. */
    for (i = 0; i < ssl_V3_SUITES_IMPLEMENTED; i++) {
	ssl3CipherSuiteCfg *suite = &ss->cipherSuites[i];
	if (config_match(suite, SSL_ALLOWED, PR_TRUE)) {
	    if (cs != NULL) {
		*cs++ = 0x00;
		*cs++ = (suite->cipher_suite >> 8) & 0xFF;
		*cs++ =  suite->cipher_suite       & 0xFF;
	    }
	    count++;
	}
    }
    *size = count;
    return SECSuccess;
}

/*
** If ssl3 socket has completed the first handshake, and is in idle state, 
** then start a new handshake.
** If flushCache is true, the SID cache will be flushed first, forcing a
** "Full" handshake (not a session restart handshake), to be done.
**
** called from SSL_RedoHandshake(), which already holds the handshake locks.
*/
SECStatus
ssl3_RedoHandshake(sslSocket *ss, PRBool flushCache)
{
    sslSessionID *   sid = ss->sec.ci.sid;
    SECStatus        rv;

    PORT_Assert( ss->opt.noLocks || ssl_HaveSSL3HandshakeLock(ss) );

    if (!ss->firstHsDone ||
        ((ss->version >= SSL_LIBRARY_VERSION_3_0) &&
	 ss->ssl3.initialized && 
	 (ss->ssl3.hs.ws != idle_handshake))) {
	PORT_SetError(SSL_ERROR_HANDSHAKE_NOT_COMPLETED);
	return SECFailure;
    }
    if (sid && flushCache) {
	ss->sec.uncache(sid);	/* remove it from whichever cache it's in. */
	ssl_FreeSID(sid);	/* dec ref count and free if zero. */
	ss->sec.ci.sid = NULL;
    }

    ssl_GetXmitBufLock(ss);	/**************************************/

    /* start off a new handshake. */
    rv = (ss->sec.isServer) ? ssl3_SendHelloRequest(ss)
                            : ssl3_SendClientHello(ss);

    ssl_ReleaseXmitBufLock(ss);	/**************************************/
    return rv;
}

/* Called from ssl_DestroySocketContents() in sslsock.c */
void
ssl3_DestroySSL3Info(sslSocket *ss)
{

    if (ss->ssl3.clientCertificate != NULL)
	CERT_DestroyCertificate(ss->ssl3.clientCertificate);

    if (ss->ssl3.clientPrivateKey != NULL)
	SECKEY_DestroyPrivateKey(ss->ssl3.clientPrivateKey);

    if (ss->ssl3.peerCertArena != NULL)
	ssl3_CleanupPeerCerts(ss);

    if (ss->ssl3.clientCertChain != NULL) {
       CERT_DestroyCertificateList(ss->ssl3.clientCertChain);
       ss->ssl3.clientCertChain = NULL;
    }

    /* clean up handshake */
    if (ss->opt.bypassPKCS11) {
	SHA1_DestroyContext((SHA1Context *)ss->ssl3.hs.sha_cx, PR_FALSE);
	MD5_DestroyContext((MD5Context *)ss->ssl3.hs.md5_cx, PR_FALSE);
    } 
    if (ss->ssl3.hs.md5) {
	PK11_DestroyContext(ss->ssl3.hs.md5,PR_TRUE);
    }
    if (ss->ssl3.hs.sha) {
	PK11_DestroyContext(ss->ssl3.hs.sha,PR_TRUE);
    }
    if (ss->ssl3.hs.messages.buf) {
    	PORT_Free(ss->ssl3.hs.messages.buf);
	ss->ssl3.hs.messages.buf = NULL;
	ss->ssl3.hs.messages.len = 0;
	ss->ssl3.hs.messages.space = 0;
    }

    /* free the SSL3Buffer (msg_body) */
    PORT_Free(ss->ssl3.hs.msg_body.buf);

    /* free up the CipherSpecs */
    ssl3_DestroyCipherSpec(&ss->ssl3.specs[0]);
    ssl3_DestroyCipherSpec(&ss->ssl3.specs[1]);

    ss->ssl3.initialized = PR_FALSE;
}

/* End of ssl3con.c */
