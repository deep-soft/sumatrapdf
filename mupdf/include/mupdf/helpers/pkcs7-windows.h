// Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
// License: Simplified BSD (see COPYING.BSD)
//
// Windows native crypto backend for mupdf PDF signature handling.
// Implemented on top of the Win32 CryptoAPI (crypt32.dll). Uses only
// stock Windows facilities; no external dependencies.
//
// This first cut covers verification only:
//   - pkcs7_windows_new_verifier       — pdf_pkcs7_verifier factory
//   - pkcs7_windows_check_certificate  — is the signer's certificate trusted?
//   - pkcs7_windows_check_digest       — has the signed byte range been modified?
//   - pkcs7_windows_distinguished_name — who signed it?
// Signing (read_pfx + signer vtable) is not yet implemented.

#ifndef MUPDF_PKCS7_WINDOWS_H
#define MUPDF_PKCS7_WINDOWS_H

#include "mupdf/pdf/document.h"
#include "mupdf/pdf/form.h"

#ifdef __cplusplus
extern "C" {
#endif

pdf_signature_error pkcs7_windows_check_digest(fz_context* ctx, fz_stream* stm, char* sig, size_t sig_len);
pdf_signature_error pkcs7_windows_check_certificate(char* sig, size_t sig_len);
pdf_pkcs7_distinguished_name* pkcs7_windows_distinguished_name(fz_context* ctx, char* sig, size_t sig_len);

pdf_pkcs7_verifier* pkcs7_windows_new_verifier(fz_context* ctx);

#ifdef __cplusplus
}
#endif

#endif
