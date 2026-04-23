// Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
// License: Simplified BSD (see COPYING.BSD)
//
// Verify-only pkcs7 backend built on Windows CryptoAPI. Mirrors the
// pdf_pkcs7_verifier vtable defined in mupdf/pdf/form.h.

#include "mupdf/fitz.h"
#include "mupdf/pdf.h"

#include "mupdf/helpers/pkcs7-windows.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>

#pragma comment(lib, "crypt32.lib")

// ---- envelope parsing helpers --------------------------------------------

// Parse a PKCS#7 envelope for metadata queries only (signer info + certs).
// We deliberately don't use CMSG_DETACHED_FLAG here: the envelope itself is
// self-describing (SignedData with an optional encapsulated content that's
// absent for detached sigs), so a single finalizing CryptMsgUpdate is enough
// to make CMSG_SIGNER_COUNT_PARAM / CMSG_SIGNER_INFO_PARAM / CertOpenStore
// work. This path cannot verify the digest; see windows_check_digest.
static HCRYPTMSG open_msg_for_metadata(unsigned char* sig, size_t sig_len) {
    HCRYPTMSG hMsg = CryptMsgOpenToDecode(PKCS_7_ASN_ENCODING | X509_ASN_ENCODING, 0, 0, 0, NULL, NULL);
    if (!hMsg) {
        return NULL;
    }
    if (!CryptMsgUpdate(hMsg, sig, (DWORD)sig_len, TRUE)) {
        CryptMsgClose(hMsg);
        return NULL;
    }
    return hMsg;
}

// Fetch CMSG_SIGNER_INFO at the given signer index. Caller LocalFrees.
static PCMSG_SIGNER_INFO get_signer_info(HCRYPTMSG hMsg, DWORD idx) {
    DWORD cb = 0;
    if (!CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, idx, NULL, &cb)) {
        return NULL;
    }
    PCMSG_SIGNER_INFO si = (PCMSG_SIGNER_INFO)LocalAlloc(LPTR, cb);
    if (!si) {
        return NULL;
    }
    if (!CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, idx, si, &cb)) {
        LocalFree(si);
        return NULL;
    }
    return si;
}

static DWORD get_signer_count(HCRYPTMSG hMsg) {
    DWORD count = 0;
    DWORD cb = sizeof(count);
    if (!CryptMsgGetParam(hMsg, CMSG_SIGNER_COUNT_PARAM, 0, &count, &cb)) {
        return 0;
    }
    return count;
}

// Locate the signer's certificate inside the envelope's embedded cert set.
static PCCERT_CONTEXT find_signer_cert(HCERTSTORE hStore, PCMSG_SIGNER_INFO si) {
    CERT_INFO ci;
    ZeroMemory(&ci, sizeof(ci));
    ci.Issuer = si->Issuer;
    ci.SerialNumber = si->SerialNumber;
    return CertFindCertificateInStore(hStore, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT, &ci,
                                      NULL);
}

// ---- check_certificate ---------------------------------------------------

static pdf_signature_error windows_check_certificate(fz_context* ctx, pdf_pkcs7_verifier* vf, unsigned char* sig,
                                                     size_t sig_len) {
    (void)ctx;
    (void)vf;
    pdf_signature_error rc = PDF_SIGNATURE_ERROR_UNKNOWN;
    HCRYPTMSG hMsg = NULL;
    HCERTSTORE hStore = NULL;
    PCMSG_SIGNER_INFO si = NULL;
    PCCERT_CONTEXT cert = NULL;
    PCCERT_CHAIN_CONTEXT chain = NULL;

    hMsg = open_msg_for_metadata(sig, sig_len);
    if (!hMsg) {
        goto done;
    }
    if (get_signer_count(hMsg) == 0) {
        rc = PDF_SIGNATURE_ERROR_NO_SIGNATURES;
        goto done;
    }
    hStore = CertOpenStore(CERT_STORE_PROV_MSG, 0, 0, 0, hMsg);
    if (!hStore) {
        goto done;
    }
    si = get_signer_info(hMsg, 0);
    if (!si) {
        goto done;
    }
    cert = find_signer_cert(hStore, si);
    if (!cert) {
        rc = PDF_SIGNATURE_ERROR_NO_CERTIFICATE;
        goto done;
    }

    CERT_CHAIN_PARA chainPara;
    ZeroMemory(&chainPara, sizeof(chainPara));
    chainPara.cbSize = sizeof(chainPara);
    // No revocation check: matches openssl helper behavior (and Adobe)
    if (!CertGetCertificateChain(NULL, cert, NULL, hStore, &chainPara, 0, NULL, &chain)) {
        goto done;
    }

    DWORD status = chain->TrustStatus.dwErrorStatus;
    if (status == CERT_TRUST_NO_ERROR) {
        rc = PDF_SIGNATURE_ERROR_OKAY;
    } else if (status & CERT_TRUST_IS_UNTRUSTED_ROOT) {
        // Distinguish pure self-signed (one-element chain) from a chain
        // whose root we simply don't trust.
        BOOL isSelfSigned = FALSE;
        if (chain->cChain > 0 && chain->rgpChain[0]->cElement == 1) {
            isSelfSigned = TRUE;
        }
        rc = isSelfSigned ? PDF_SIGNATURE_ERROR_SELF_SIGNED : PDF_SIGNATURE_ERROR_SELF_SIGNED_IN_CHAIN;
    } else {
        rc = PDF_SIGNATURE_ERROR_NOT_TRUSTED;
    }

done:
    if (chain) {
        CertFreeCertificateChain(chain);
    }
    if (cert) {
        CertFreeCertificateContext(cert);
    }
    if (hStore) {
        CertCloseStore(hStore, 0);
    }
    if (si) {
        LocalFree(si);
    }
    if (hMsg) {
        CryptMsgClose(hMsg);
    }
    return rc;
}

// ---- check_digest --------------------------------------------------------

static pdf_signature_error windows_check_digest(fz_context* ctx, pdf_pkcs7_verifier* vf, fz_stream* stm,
                                                unsigned char* sig, size_t sig_len) {
    (void)vf;
    pdf_signature_error rc = PDF_SIGNATURE_ERROR_UNKNOWN;
    HCRYPTMSG hMsg = NULL;
    HCERTSTORE hStore = NULL;
    PCMSG_SIGNER_INFO si = NULL;
    PCCERT_CONTEXT cert = NULL;
    BOOL streamFailed = FALSE;

    hMsg = CryptMsgOpenToDecode(PKCS_7_ASN_ENCODING | X509_ASN_ENCODING, CMSG_DETACHED_FLAG, 0, 0, NULL, NULL);
    if (!hMsg) {
        goto done;
    }

    // Feed envelope first; detached content chunks follow.
    if (!CryptMsgUpdate(hMsg, sig, (DWORD)sig_len, FALSE)) {
        goto done;
    }

    // Stream the detached content (the signed byte range of the PDF;
    // callers hand us pdf_signature_hash_bytes).
    fz_try(ctx) {
        unsigned char buf[4096];
        for (;;) {
            size_t n = fz_read(ctx, stm, buf, sizeof(buf));
            if (n == 0) {
                break;
            }
            if (!CryptMsgUpdate(hMsg, buf, (DWORD)n, FALSE)) {
                streamFailed = TRUE;
                break;
            }
        }
    }
    fz_catch(ctx) {
        streamFailed = TRUE;
    }
    if (streamFailed) {
        goto done;
    }

    // Finalize with an empty chunk.
    if (!CryptMsgUpdate(hMsg, NULL, 0, TRUE)) {
        goto done;
    }

    if (get_signer_count(hMsg) == 0) {
        rc = PDF_SIGNATURE_ERROR_NO_SIGNATURES;
        goto done;
    }
    hStore = CertOpenStore(CERT_STORE_PROV_MSG, 0, 0, 0, hMsg);
    if (!hStore) {
        goto done;
    }
    si = get_signer_info(hMsg, 0);
    if (!si) {
        goto done;
    }
    cert = find_signer_cert(hStore, si);
    if (!cert) {
        rc = PDF_SIGNATURE_ERROR_NO_CERTIFICATE;
        goto done;
    }

    if (CryptMsgControl(hMsg, 0, CMSG_CTRL_VERIFY_SIGNATURE, cert->pCertInfo)) {
        rc = PDF_SIGNATURE_ERROR_OKAY;
    } else {
        DWORD err = GetLastError();
        // NTE_BAD_SIGNATURE / CRYPT_E_HASH_VALUE — integrity failure
        if (err == (DWORD)NTE_BAD_SIGNATURE || err == (DWORD)CRYPT_E_HASH_VALUE) {
            rc = PDF_SIGNATURE_ERROR_DIGEST_FAILURE;
        }
    }

done:
    if (cert) {
        CertFreeCertificateContext(cert);
    }
    if (hStore) {
        CertCloseStore(hStore, 0);
    }
    if (si) {
        LocalFree(si);
    }
    if (hMsg) {
        CryptMsgClose(hMsg);
    }
    return rc;
}

// ---- get_signatory -------------------------------------------------------

// Copy a named attribute from the cert Subject into an fz-owned C string.
// Returns NULL when the attribute is empty or missing.
static char* get_name_string(fz_context* ctx, PCCERT_CONTEXT cert, LPCSTR oid) {
    DWORD n = CertGetNameStringA(cert, CERT_NAME_ATTR_TYPE, 0, (void*)oid, NULL, 0);
    if (n <= 1) {
        return NULL;
    }
    char* buf = fz_malloc(ctx, n);
    CertGetNameStringA(cert, CERT_NAME_ATTR_TYPE, 0, (void*)oid, buf, n);
    return buf;
}

static pdf_pkcs7_distinguished_name* windows_get_signatory(fz_context* ctx, pdf_pkcs7_verifier* vf, unsigned char* sig,
                                                           size_t sig_len) {
    (void)vf;
    HCRYPTMSG hMsg = NULL;
    HCERTSTORE hStore = NULL;
    PCMSG_SIGNER_INFO si = NULL;
    PCCERT_CONTEXT cert = NULL;
    pdf_pkcs7_distinguished_name* dn = NULL;

    hMsg = open_msg_for_metadata(sig, sig_len);
    if (!hMsg) {
        goto done;
    }
    if (get_signer_count(hMsg) == 0) {
        goto done;
    }
    hStore = CertOpenStore(CERT_STORE_PROV_MSG, 0, 0, 0, hMsg);
    if (!hStore) {
        goto done;
    }
    si = get_signer_info(hMsg, 0);
    if (!si) {
        goto done;
    }
    cert = find_signer_cert(hStore, si);
    if (!cert) {
        goto done;
    }

    dn = fz_malloc_struct(ctx, pdf_pkcs7_distinguished_name);
    fz_try(ctx) {
        dn->cn = get_name_string(ctx, cert, szOID_COMMON_NAME);
        dn->o = get_name_string(ctx, cert, szOID_ORGANIZATION_NAME);
        dn->ou = get_name_string(ctx, cert, szOID_ORGANIZATIONAL_UNIT_NAME);
        dn->email = get_name_string(ctx, cert, szOID_RSA_emailAddr);
        dn->c = get_name_string(ctx, cert, szOID_COUNTRY_NAME);
    }
    fz_catch(ctx) {
        pdf_signature_drop_distinguished_name(ctx, dn);
        dn = NULL;
    }

done:
    if (cert) {
        CertFreeCertificateContext(cert);
    }
    if (hStore) {
        CertCloseStore(hStore, 0);
    }
    if (si) {
        LocalFree(si);
    }
    if (hMsg) {
        CryptMsgClose(hMsg);
    }
    return dn;
}

// ---- verifier vtable -----------------------------------------------------

static void windows_drop_verifier(fz_context* ctx, pdf_pkcs7_verifier* vf) {
    fz_free(ctx, vf);
}

pdf_pkcs7_verifier* pkcs7_windows_new_verifier(fz_context* ctx) {
    pdf_pkcs7_verifier* vf = fz_malloc_struct(ctx, pdf_pkcs7_verifier);
    vf->drop = windows_drop_verifier;
    vf->check_certificate = windows_check_certificate;
    vf->check_digest = windows_check_digest;
    vf->get_signatory = windows_get_signatory;
    return vf;
}

// ---- flat convenience wrappers (mirror the openssl helper surface) -------

pdf_signature_error pkcs7_windows_check_certificate(char* sig, size_t sig_len) {
    return windows_check_certificate(NULL, NULL, (unsigned char*)sig, sig_len);
}

pdf_signature_error pkcs7_windows_check_digest(fz_context* ctx, fz_stream* stm, char* sig, size_t sig_len) {
    return windows_check_digest(ctx, NULL, stm, (unsigned char*)sig, sig_len);
}

pdf_pkcs7_distinguished_name* pkcs7_windows_distinguished_name(fz_context* ctx, char* sig, size_t sig_len) {
    return windows_get_signatory(ctx, NULL, (unsigned char*)sig, sig_len);
}
