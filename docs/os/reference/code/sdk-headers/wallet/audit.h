/* src/wallet/audit.h - append-only audit log (/wallet/audit.log, rotated to /wallet/audit.1 at 64 KB).
   Every line is also written to badge_log with tag "wallet". */
#ifndef AUDIT_H
#define AUDIT_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define AUDIT_LINE_MAX 160

void audit_begin(void);

/* Writes one line:
     <seq> <uptime_ms> <event> <app_id> <result> <amount_raw> <subject> <identity> <presence> <sig16>
   event:    TX REQ PROOF RCPT BROKER CONFIG
   app_id:   calling app id; "push", "serial" or "ble" for CONFIG; "-" when no app is involved
   result:   "ok" or a badge_err_t name
   subject:  TX: recipient wallet base58, or "ta:" + token account base58 when the wallet is unknown;
             PROOF, RCPT: payer public key base58; CONFIG: the config key that changed; else "-"
   identity: verified unverified mismatch revoked expired unknown, or "-"
   presence: present not_checked not_present, or "-"
   sig16:    first 16 base58 characters of the signature, or "-"
   Pass NULL for any field that is absent; it is written as "-". */
void audit_log(const char *event, const char *app_id, const char *result, const char *amount_raw,
               const char *subject, const char *identity, const char *presence, const char *sig16);

/* Copies the newest max_lines lines (oldest first, each ending in '\n') into out, NUL-terminated.
   Returns the number of lines copied. Used by Settings > Wallet > Audit and GET /api/wallet/audit. */
size_t audit_tail(char *out, size_t cap, size_t max_lines);

#ifdef __cplusplus
}
#endif
#endif
