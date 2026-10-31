/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

// sv_webrtc_cert.c - the DTLS certificate WebRTC clients see
//
// Like FTE (cert.pem and privkey.pem in its base directory), the server keeps
// one self-signed certificate on disk, so its fingerprint stays the same from
// one start to the next. When neither file exists a new ECDSA P-256 pair is
// generated and written. Files that exist are never overwritten: a damaged or
// mismatched pair is reported and WebRTC stays off until it is fixed.

#ifdef WEBRTC

#include "qwsvdef.h"
#include "sv_webrtc.h"

#include <errno.h>
#ifndef _WIN32
#include <sys/types.h>
#include <sys/stat.h>
#endif

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#if defined(MBEDTLS_PSA_CRYPTO_C)
#include <psa/crypto.h>
#endif

#define CERT_MAX_FILE		(256 * 1024)	// far above any PEM chain
#define CERT_PEM_SIZE		4096			// one P-256 certificate or key as PEM
#define CERT_VALID_YEARS	10				// FTE's span
#define CERT_SUBJECT		"CN=mvdsv"
#define PEM_END_CERT		"-----END CERTIFICATE-----"

typedef enum { CERTFILE_OK, CERTFILE_MISSING, CERTFILE_BAD } certfile_t;

static certfile_t Cert_ReadFile (const char *name, char **out)
{
	vfsfile_t *f;
	vfserrno_t err;
	unsigned long len;
	char *text;

	*out = NULL;
	errno = 0;
	if (!(f = FS_OpenVFS(name, "rb", FS_BASE_OS)))
		return errno == ENOENT ? CERTFILE_MISSING : CERTFILE_BAD;

	len = VFS_GETLEN(f);
	if (len == 0 || len > CERT_MAX_FILE)
	{
		VFS_CLOSE(f);
		return CERTFILE_BAD;
	}

	text = (char *) Q_malloc(len + 1);
	if (VFS_READ(f, text, (int) len, &err) != (int) len)
	{
		VFS_CLOSE(f);
		Q_free(text);
		return CERTFILE_BAD;
	}
	VFS_CLOSE(f);

	text[len] = 0;
	*out = text;
	return CERTFILE_OK;
}

static qbool Cert_WriteFile (const char *name, const char *text, qbool secret)
{
	vfsfile_t *f;
	int len = (int) strlen(text);
	qbool ok;
#ifndef _WIN32
	mode_t mask = 0;

	if (secret)
		mask = umask(077); // the key: its owner only
#endif
	f = FS_OpenVFS(name, "wb", FS_BASE_OS);
#ifndef _WIN32
	if (secret)
		umask(mask);
#endif
	if (!f)
		return false;

	ok = VFS_WRITE(f, text, len) == len;
	VFS_CLOSE(f);
	return ok;
}

// "AB:CD:...", the form of an SDP's a=fingerprint:sha-256 line
static qbool Cert_Fingerprint (const mbedtls_x509_crt *crt, char *out, size_t size)
{
	unsigned char hash[32];
	size_t i;

	if (size < sizeof(hash) * 3 || mbedtls_sha256(crt->raw.p, crt->raw.len, hash, 0) != 0)
		return false;

	for (i = 0; i < sizeof(hash); i++)
		snprintf(out + i * 3, size - i * 3, "%02X%s", hash[i], i + 1 < sizeof(hash) ? ":" : "");
	return true;
}

// NULL when the pair is usable, else what is wrong with it
static const char *Cert_Check (const char *cert_pem, const char *key_pem, mbedtls_ctr_drbg_context *drbg,
	char *fingerprint, size_t fingerprint_size, qbool *chain, qbool *expired)
{
	mbedtls_x509_crt crt;
	mbedtls_pk_context key;
	const char *problem = NULL;

	mbedtls_x509_crt_init(&crt);
	mbedtls_pk_init(&key);

	if (mbedtls_x509_crt_parse(&crt, (const unsigned char *) cert_pem, strlen(cert_pem) + 1) != 0)
		problem = "the certificate file is not a PEM certificate";
	else if (mbedtls_pk_parse_key(&key, (const unsigned char *) key_pem, strlen(key_pem) + 1, NULL, 0,
			mbedtls_ctr_drbg_random, drbg) != 0)
		problem = "the key file is not an unencrypted PEM key";
	else if (mbedtls_pk_check_pair(&crt.pk, &key, mbedtls_ctr_drbg_random, drbg) != 0)
		problem = "the key does not belong to the certificate";
	else if (!Cert_Fingerprint(&crt, fingerprint, fingerprint_size))
		problem = "the certificate's fingerprint could not be computed";
	else
	{
		*chain = crt.next != NULL;
		*expired = mbedtls_x509_time_is_past(&crt.valid_to) != 0;
	}

	mbedtls_pk_free(&key);
	mbedtls_x509_crt_free(&crt);
	return problem;
}

// YYYYMMDDHHMMSS, years on from t
static void Cert_Time (time_t t, int years, char *out, size_t size)
{
	struct tm *tm = gmtime(&t);

	if (years && tm->tm_mon == 1 && tm->tm_mday == 29)
		tm->tm_mday = 28; // the year it ends may have no 29 February
	tm->tm_year += years;
	strftime(out, size, "%Y%m%d%H%M%S", tm);
}

static qbool Cert_Generate (mbedtls_ctr_drbg_context *drbg, char **cert_pem, char **key_pem)
{
	mbedtls_pk_context key;
	mbedtls_x509write_cert crt;
	unsigned char serial[16];
	unsigned char *pem;
	char not_before[16], not_after[16], error[128];
	time_t now = time(NULL);
	int ret;

	*cert_pem = *key_pem = NULL;
	mbedtls_pk_init(&key);
	mbedtls_x509write_crt_init(&crt);
	pem = (unsigned char *) Q_malloc(CERT_PEM_SIZE);

	Cert_Time(now - 3600, 0, not_before, sizeof(not_before)); // an hour's leeway for clocks
	Cert_Time(now, CERT_VALID_YEARS, not_after, sizeof(not_after));

	if ((ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))) != 0
		|| (ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
			mbedtls_ctr_drbg_random, drbg)) != 0
		|| (ret = mbedtls_ctr_drbg_random(drbg, serial, sizeof(serial))) != 0)
		goto fail;
	serial[0] = (serial[0] & 0x3f) | 0x40; // positive, without a leading zero byte

	mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
	mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
	mbedtls_x509write_crt_set_subject_key(&crt, &key);
	mbedtls_x509write_crt_set_issuer_key(&crt, &key);
	if ((ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial))) != 0
		|| (ret = mbedtls_x509write_crt_set_subject_name(&crt, CERT_SUBJECT)) != 0
		|| (ret = mbedtls_x509write_crt_set_issuer_name(&crt, CERT_SUBJECT)) != 0
		|| (ret = mbedtls_x509write_crt_set_validity(&crt, not_before, not_after)) != 0
		|| (ret = mbedtls_x509write_crt_pem(&crt, pem, CERT_PEM_SIZE, mbedtls_ctr_drbg_random, drbg)) != 0)
		goto fail;
	*cert_pem = Q_strdup((char *) pem);

	if ((ret = mbedtls_pk_write_key_pem(&key, pem, CERT_PEM_SIZE)) != 0)
		goto fail;
	*key_pem = Q_strdup((char *) pem);

	mbedtls_platform_zeroize(pem, CERT_PEM_SIZE);
	Q_free(pem);
	mbedtls_x509write_crt_free(&crt);
	mbedtls_pk_free(&key);
	return true;

fail:
	mbedtls_strerror(ret, error, sizeof(error));
	Con_Printf("WebRTC: generating a certificate failed: %s\n", error);
	mbedtls_platform_zeroize(pem, CERT_PEM_SIZE);
	Q_free(pem);
	Q_free(*cert_pem);
	mbedtls_x509write_crt_free(&crt);
	mbedtls_pk_free(&key);
	return false;
}

qbool SV_WebRTC_LoadCertificate (const char *certfile, const char *keyfile,
	char **cert_pem, char **key_pem, char *fingerprint, size_t fingerprint_size)
{
	static const char personalisation[] = "mvdsv webrtc certificate";
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;
	certfile_t certstate, keystate;
	char *cert = NULL, *key = NULL;
	const char *problem = NULL;
	qbool generated = false, chain = false, expired = false;

	*cert_pem = *key_pem = NULL;
	mbedtls_entropy_init(&entropy);
	mbedtls_ctr_drbg_init(&drbg);

#if defined(MBEDTLS_PSA_CRYPTO_C)
	// built with MBEDTLS_USE_PSA_CRYPTO, Mbed TLS does keys and signatures
	// through PSA, which has to be started first (libdatachannel starts it
	// only later, and starting it again does nothing)
	if (psa_crypto_init() != PSA_SUCCESS)
	{
		problem = "Mbed TLS's cryptography could not start";
		goto done;
	}
#endif

	if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
		(const unsigned char *) personalisation, sizeof(personalisation) - 1) != 0)
	{
		problem = "there is no source of random numbers";
		goto done;
	}

	certstate = Cert_ReadFile(certfile, &cert);
	keystate = Cert_ReadFile(keyfile, &key);

	if (certstate == CERTFILE_MISSING && keystate == CERTFILE_MISSING)
	{
		// the first start: make the server's identity, and keep it
		if (!Cert_Generate(&drbg, &cert, &key))
		{
			problem = "no certificate could be made";
			goto done;
		}
		generated = true;
		if (!Cert_WriteFile(keyfile, key, true) || !Cert_WriteFile(certfile, cert, false))
		{
			// neither was there before: take back what was written, so that
			// the next start makes a pair again rather than find half of one
			FS_RemoveBaseFile(keyfile);
			FS_RemoveBaseFile(certfile);
			Con_Printf("WebRTC: could not write %s and %s; this certificate lasts until the server stops\n",
				certfile, keyfile);
		}
	}
	else if (certstate == CERTFILE_MISSING || keystate == CERTFILE_MISSING)
	{
		problem = va("%s exists but %s does not; restore it, or remove both to have a new pair made",
			certstate == CERTFILE_MISSING ? keyfile : certfile,
			certstate == CERTFILE_MISSING ? certfile : keyfile);
		goto done;
	}
	else if (certstate != CERTFILE_OK || keystate != CERTFILE_OK)
	{
		problem = va("%s could not be read", certstate != CERTFILE_OK ? certfile : keyfile);
		goto done;
	}

	if ((problem = Cert_Check(cert, key, &drbg, fingerprint, fingerprint_size, &chain, &expired)) != NULL)
	{
		problem = va("%s and %s: %s", certfile, keyfile, problem);
		goto done;
	}

	if (chain)
	{
		// FTE hashes every certificate it is sent, so present the first only
		char *end = strstr(cert, PEM_END_CERT);

		if (end)
			end[sizeof(PEM_END_CERT) - 1] = 0;
		Con_Printf("WebRTC: %s holds a chain; only its first certificate is used\n", certfile);
	}
	if (expired)
		Con_Printf("WebRTC: %s has expired (peers check its fingerprint only, so it still works)\n", certfile);

	Con_Printf("WebRTC: %s certificate %s, key %s\n", generated ? "generated" : "loaded", certfile, keyfile);
	Con_Printf("WebRTC: certificate fingerprint sha-256 %s\n", fingerprint);

	*cert_pem = cert;
	*key_pem = key;
	cert = key = NULL;

done:
	if (problem)
		Con_Printf("WebRTC: %s\n", problem);
	if (key)
		mbedtls_platform_zeroize(key, strlen(key));
	Q_free(cert);
	Q_free(key);
	mbedtls_ctr_drbg_free(&drbg);
	mbedtls_entropy_free(&entropy);
	return *cert_pem != NULL;
}

#endif // WEBRTC
