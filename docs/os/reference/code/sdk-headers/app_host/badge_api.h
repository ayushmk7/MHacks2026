#ifndef BADGE_API_H
#define BADGE_API_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define BADGE_ABI_VERSION 1                   /* layout of badge_app_desc_t */
#define BADGE_API_VERSION 2                   /* == badge.api_version / badge.app.api() in Lua */

/* Conventions
   - Functions returning badge_err_t return BADGE_OK or one of the codes listed at the function or in the
     table "Error codes of the upstream-mirroring functions" at the end of this header.
   - A function that needs a permission the app lacks returns BADGE_ERR_DENIED if it returns badge_err_t.
     If it returns anything else it returns its empty value (false, 0, "", BADGE_PRESENCE_NONE, a zeroed
     struct with state BADGE_PAY_IDLE) and logs "[app] <id>: <function> needs permission <name>" once per
     launch. Check badge_app_has() first when the difference matters.
   - Public keys are 32 raw bytes, signatures 64 raw bytes, amounts uint64_t raw units.
   - Where a parameter is documented "NULL = this badge", NULL selects the badge's own public key. */
typedef int32_t badge_err_t;
enum { BADGE_OK = 0, BADGE_ERR_BAD_ARG = 1, BADGE_ERR_DENIED = 2, BADGE_ERR_NOT_READY = 3, BADGE_ERR_BUSY = 4,
       BADGE_ERR_RATE_LIMITED = 5, BADGE_ERR_NO_NETWORK = 6, BADGE_ERR_TIMEOUT = 7, BADGE_ERR_IO = 8,
       BADGE_ERR_RPC = 9, BADGE_ERR_PARSE = 10, BADGE_ERR_NO_ACCOUNT = 11, BADGE_ERR_TOO_LONG = 12,
       BADGE_ERR_NO_MEMORY = 13, BADGE_ERR_UNKNOWN_INSTRUCTION = 20, BADGE_ERR_WRONG_SIGNER = 21,
       BADGE_ERR_UNKNOWN_MINT = 22, BADGE_ERR_DECIMALS = 23, BADGE_ERR_BAD_SOURCE = 24, BADGE_ERR_OVER_LIMIT = 25,
       BADGE_ERR_BLOCKED = 26, BADGE_ERR_REJECTED = 27, BADGE_ERR_APPROVAL_TIMEOUT = 28,
       BADGE_ERR_SIGN_FAILED = 29, BADGE_ERR_NO_DISPLAY = 30, BADGE_ERR_NO_SESSION = 31 };
enum { BADGE_CAP_SIGN = 0x01, BADGE_CAP_NET = 0x02, BADGE_CAP_RADIO = 0x04, BADGE_CAP_WALLET = 0x08, BADGE_CAP_SYSTEM = 0x10 };
typedef enum { BADGE_KEY_UP = 0, BADGE_KEY_LEFT = 1, BADGE_KEY_RIGHT = 2, BADGE_KEY_DOWN = 3,
               BADGE_KEY_A = 4 /* SELECT */, BADGE_KEY_B = 5 /* CANCEL */ } badge_key_t;   /* == BTN_* in config.h */

/* ---- app descriptor: what a native app exports ---- */
typedef struct badge_app_desc {
  uint32_t abi;                               /* BADGE_ABI_VERSION */
  const char *id, *name, *version, *author, *description;
  uint32_t caps;                              /* BADGE_CAP_* */
  void (*on_start)(void);
  void (*on_update)(float dt);
  void (*on_draw)(void);
  void (*on_button)(badge_key_t key, bool pressed);
  void (*on_espnow)(const uint8_t mac[6], const uint8_t *data, size_t len, int rssi);
  void (*on_ble)(const char *line);
  void (*on_stop)(void);
} badge_app_desc_t;

/* ---- app ---- */
const char *badge_app_id(void);
bool        badge_app_has(uint32_t cap);
void        badge_app_fail(const char *message);          /* stop this app and show the error screen */
void       *badge_alloc(size_t bytes);                    /* PSRAM, counted against the 1 MiB app cap; NULL when over */
void        badge_free(void *p);
void        badge_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- gfx (colours are RGB565) ---- */
int      badge_gfx_width(void);
int      badge_gfx_height(void);
void     badge_gfx_clear(uint16_t c);
void     badge_gfx_pixel(int x, int y, uint16_t c);
void     badge_gfx_line(int x0, int y0, int x1, int y1, uint16_t c);
void     badge_gfx_rect(int x, int y, int w, int h, uint16_t c);
void     badge_gfx_fill_rect(int x, int y, int w, int h, uint16_t c);
void     badge_gfx_round_rect(int x, int y, int w, int h, int r, uint16_t c);
void     badge_gfx_fill_round_rect(int x, int y, int w, int h, int r, uint16_t c);
void     badge_gfx_circle(int x, int y, int r, uint16_t c);
void     badge_gfx_fill_circle(int x, int y, int r, uint16_t c);
void     badge_gfx_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c);
void     badge_gfx_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c);
void     badge_gfx_text(const char *s, int x, int y, uint16_t c, int size);
void     badge_gfx_text_center(const char *s, int cx, int y, uint16_t c, int size);
void     badge_gfx_text_right(const char *s, int rx, int y, uint16_t c, int size);
int      badge_gfx_text_width(const char *s, int size);
int      badge_gfx_text_height(int size);
uint16_t badge_gfx_color(uint8_t r, uint8_t g, uint8_t b);
uint16_t badge_gfx_hsv(float h, float s, float v);
uint16_t badge_gfx_gradient(float t);
bool     badge_gfx_image(const char *path, int x, int y, float scale);
bool     badge_gfx_image_size(const char *path, int *w, int *h);
uint8_t  badge_gfx_brightness(int set_or_minus1);
void     badge_gfx_flush(void);
#define BADGE_BLACK 0x0000
#define BADGE_WHITE 0xFFFF
#define BADGE_SOLANA_PURPLE 0x9A3F   /* rgb565(0x99,0x45,0xFF) */
#define BADGE_SOLANA_GREEN  0x1792   /* rgb565(0x14,0xF1,0x95) */
#define BADGE_RED    0xFA28          /* rgb565(0xFF,0x45,0x45) */
#define BADGE_ORANGE 0xFD84          /* rgb565(0xFF,0xB0,0x20) */
#define BADGE_MUTED  0x9495          /* rgb565(0x93,0x93,0xA8) */
#define BADGE_BG     0x0842          /* rgb565(0x0B,0x0B,0x12) */

/* ---- input ---- */
bool        badge_input_down(badge_key_t k);
bool        badge_input_pressed(badge_key_t k);
bool        badge_input_released(badge_key_t k);
bool        badge_input_repeated(badge_key_t k);
uint32_t    badge_input_held_ms(badge_key_t k);
bool        badge_input_any(void);
const char *badge_input_label(badge_key_t k);             /* "SELECT", "CANCEL", ... */
bool        badge_input_present(void);

/* ---- led ---- */
void    badge_led_set(int index, uint8_t r, uint8_t g, uint8_t b);
void    badge_led_all(uint8_t r, uint8_t g, uint8_t b);
void    badge_led_gradient(int index, float t, float intensity);
void    badge_led_show(void);
void    badge_led_off(void);
void    badge_led_pulse(uint8_t r, uint8_t g, uint8_t b, uint16_t ms);
uint8_t badge_led_brightness(int set_or_minus1);
void    badge_led_take(void);

/* ---- system ---- */
uint32_t    badge_system_millis(void);
void        badge_system_sleep(uint32_t ms);              /* <= 2000 */
uint32_t    badge_system_heap(void);
uint32_t    badge_system_psram(void);
const char *badge_system_name(void);
badge_err_t badge_system_set_name(const char *name);      /* CAP_SYSTEM; 1..23 printable ASCII */
void        badge_system_exit(void);                      /* deferred */
badge_err_t badge_system_launch(const char *app_id);      /* CAP_SYSTEM; deferred */
badge_err_t badge_system_reboot(void);                    /* CAP_SYSTEM; does not return on success */
void        badge_system_random_bytes(uint8_t *out, size_t n);

/* ---- storage (paths relative to /apps/<id>/) ---- */
badge_err_t badge_storage_read(const char *path, uint8_t *buf, size_t cap, size_t *len);
badge_err_t badge_storage_write(const char *path, const uint8_t *data, size_t len);
badge_err_t badge_storage_append(const char *path, const uint8_t *data, size_t len);
bool        badge_storage_exists(const char *path);
int32_t     badge_storage_size(const char *path);         /* -1 if missing */
bool        badge_storage_remove(const char *path);
bool        badge_storage_mkdir(const char *path);
bool        badge_storage_kv_get(const char *key, char *out, size_t cap);
bool        badge_storage_kv_set(const char *key, const char *value);
bool        badge_storage_kv_remove(const char *key);

/* ---- battery / mic / se050 ---- */
float badge_battery_volts(void);
float badge_battery_percent(void);
bool  badge_battery_charging(void);
bool  badge_mic_enable(bool on);
void  badge_mic_level(float *left, float *right);
bool  badge_se050_present(void);
bool  badge_se050_random(uint8_t *out, size_t n);         /* n <= 64 */

/* ---- wifi / http (CAP_NET) ---- */
bool        badge_wifi_connected(void);
const char *badge_wifi_status(void);
const char *badge_wifi_ssid(void);
int         badge_wifi_rssi(void);
int         badge_wifi_channel(void);
badge_err_t badge_http_get(const char *url, uint32_t timeout_ms, int *status, char *body, size_t cap, size_t *len);
badge_err_t badge_http_post(const char *url, const char *body_in, size_t body_len, const char *content_type,
                            uint32_t timeout_ms, int *status, char *body, size_t cap, size_t *len);

/* ---- espnow / ble (CAP_RADIO) ---- */
typedef struct { uint8_t mac[6]; char name[24]; int8_t rssi; uint32_t age_ms; uint32_t packets; } badge_peer_t;
bool        badge_espnow_enable(bool on);
bool        badge_espnow_enabled(void);
int         badge_espnow_channel(void);
badge_err_t badge_espnow_broadcast(const uint8_t *data, size_t len);            /* len <= 240 */
badge_err_t badge_espnow_send(const uint8_t mac[6], const uint8_t *data, size_t len);
size_t      badge_espnow_peers(badge_peer_t *out, size_t max);                  /* strongest RSSI first */
bool        badge_ble_enable(bool on);
bool        badge_ble_connected(void);
bool        badge_ble_send(const char *line);
void        badge_ble_listen(bool on);

/* ---- identity ---- */
typedef struct { const uint8_t *recipient; const char *claimed_name; const char *claimed_amount; const uint8_t *request_id; } badge_sign_hint_t;
bool        badge_identity_pubkey(uint8_t out[32]);
size_t      badge_identity_pubkey_b58(char out[45]);
size_t      badge_identity_badge_id(char out[9]);         /* first 8 base58 characters; returns 8, or 0 if not ready */
uint8_t     badge_identity_source(void);                  /* 0 none, 1 se050, 2 software */
typedef struct {                                          /* identity.decode(): preview only, never trusted by the wallet */
  uint8_t  version;                                       /* 0xFF legacy, 0 v0 */
  uint8_t  payer[32], source[32], destination[32], mint[32];
  uint64_t amount;
  uint8_t  decimals;
  char     amount_ui[24];                                 /* "10.00" */
  char     symbol[8];                                     /* "" when the mint is not the configured one */
  bool     mint_known, source_is_own, payer_is_self;
} badge_decoded_t;
/* BADGE_OK when the decoder accepts the bytes (local checks only in the three booleans);
   BADGE_ERR_UNKNOWN_INSTRUCTION with *detail = decoder reason ("ix_data", ...) otherwise. detail may be NULL. */
badge_err_t badge_identity_decode(const uint8_t *message, size_t len, badge_decoded_t *out, const char **detail);
badge_err_t badge_identity_sign(const uint8_t *message, size_t len, const badge_sign_hint_t *hint, uint8_t sig_out[64]);  /* CAP_SIGN */

/* ---- wallet ---- */
typedef struct {
  bool     ready;                                         /* identity ready and mint configured */
  uint8_t  pubkey[32];
  uint8_t  source;                                        /* 0 none, 1 se050, 2 software */
  uint8_t  mint[32], token_account[32];
  char     symbol[8];
  uint8_t  decimals;
  uint64_t cap, max;
  uint16_t deadline_ms;
  bool     block_red, tls_pinned, clock_synced;
  char     dash_url[64];                                  /* "" when unset */
} badge_wallet_info_t;
void        badge_wallet_info(badge_wallet_info_t *out);

/* ---- sol / codec ---- */
size_t      badge_sol_transfer_message(const uint8_t to_owner[32], uint64_t amount, const uint8_t blockhash[32], uint8_t *out, size_t cap);
size_t      badge_sol_wire(const uint8_t *message, size_t len, const uint8_t sig[64], uint8_t *out, size_t cap);
bool        badge_sol_ata(const uint8_t owner[32], uint8_t out[32]);
size_t      badge_sol_short(const uint8_t key[32], char out[12]);
size_t      badge_wallet_format(uint64_t raw, char *out, size_t cap);
bool        badge_wallet_parse(const char *text, uint64_t *raw);
size_t      badge_codec_b58encode(const uint8_t *in, size_t len, char *out, size_t cap);
bool        badge_codec_b58decode(const char *in, uint8_t *out, size_t len);
size_t      badge_codec_b64encode(const uint8_t *in, size_t len, char *out, size_t cap);
bool        badge_codec_b64decode(const char *in, uint8_t *out, size_t cap, size_t *len);
size_t      badge_codec_hex(const uint8_t *in, size_t len, char *out, size_t cap);       /* lowercase; returns 2*len, 0 if cap too small */
bool        badge_codec_unhex(const char *in, uint8_t *out, size_t cap, size_t *len);

/* ---- rpc (CAP_NET) ---- */
typedef enum { BADGE_TX_PENDING = 0, BADGE_TX_PROCESSED, BADGE_TX_CONFIRMED, BADGE_TX_FINALIZED, BADGE_TX_FAILED } badge_tx_status_t;
#define BADGE_RPC_TIMEOUT_MS 4000
/* All: denied, not_ready, no_network, timeout, io, rpc, parse, too_long (response over 8 KB). */
badge_err_t badge_rpc_balance(const uint8_t *pubkey_or_null, uint64_t *lamports);        /* NULL = this badge */
/* A token account that does not exist is BADGE_OK with *raw = 0 and the configured decimals. */
badge_err_t badge_rpc_token_balance(const uint8_t *owner_or_null, uint64_t *raw, uint8_t *decimals);   /* NULL = this badge */
badge_err_t badge_rpc_blockhash(uint8_t out[32]);
badge_err_t badge_rpc_send(const uint8_t *wire, size_t len, uint8_t sig_out[64], char *errmsg, size_t errcap);
badge_err_t badge_rpc_status(const uint8_t sig[64], badge_tx_status_t *status);
/* no_account when the account does not exist; parse when it is not a 165-byte SPL token account. */
badge_err_t badge_rpc_token_owner(const uint8_t token_account[32], uint8_t owner[32], uint8_t mint[32]);
/* Generic JSON-RPC. params_json is the JSON array text. Writes the text of "result" to out (NUL-terminated).
   too_long when the result does not fit cap. */
badge_err_t badge_rpc_call(const char *method, const char *params_json, char *out, size_t cap, size_t *len);

/* ---- attest (CAP_NET) ---- */
typedef enum { BADGE_ATTEST_VERIFIED = 0, BADGE_ATTEST_UNVERIFIED = 1, BADGE_ATTEST_MISMATCH = 2, BADGE_ATTEST_REVOKED = 3,
               BADGE_ATTEST_EXPIRED = 4, BADGE_ATTEST_UNKNOWN = 5 } badge_attest_status_t;   /* == wallet_identity_t */
typedef struct { uint8_t status;   /* badge_attest_status_t */
                 char name[33]; int64_t expiry; uint32_t age_ms; uint8_t flags; } badge_attest_t;
#define BADGE_ATTEST_F_CACHED 0x01
#define BADGE_ATTEST_F_TLS_UNPINNED 0x02
#define BADGE_ATTEST_F_VIA_BRIDGE 0x04
/* denied, bad_arg, not_ready (cred/schema unset). A network failure is BADGE_OK with status BADGE_ATTEST_UNKNOWN. */
badge_err_t badge_attest_check(const uint8_t subject[32], const char *claimed_name, bool force, badge_attest_t *out);
badge_err_t badge_attest_self(bool force, badge_attest_t *out);
bool        badge_attest_cached(const uint8_t subject[32], badge_attest_t *out);

/* ---- pay (CAP_RADIO; request/receive also CAP_SIGN) ---- */
typedef struct { uint8_t id[8]; uint8_t payee[32]; char name[33]; uint64_t amount; uint8_t mac[6]; int8_t rssi; uint32_t age_ms; bool sig_ok; } badge_pay_req_t;
typedef struct { uint8_t pubkey[32]; char name[33]; uint32_t age_ms; } badge_pay_peer_t;
typedef enum { BADGE_PAY_IDLE = 0, BADGE_PAY_OPEN, BADGE_PAY_RECEIVE, BADGE_PAY_PAID, BADGE_PAY_EXPIRED } badge_pay_state_t;
typedef enum { BADGE_PRESENCE_NONE = 0, BADGE_PRESENCE_PENDING, BADGE_PRESENCE_PRESENT, BADGE_PRESENCE_LATE,
               BADGE_PRESENCE_BAD_SIG, BADGE_PRESENCE_TIMEOUT } badge_presence_t;
typedef struct { badge_pay_state_t state; uint8_t id[8]; uint64_t amount; uint32_t remaining_ms; uint8_t proofs;
                 bool has_payer; uint8_t payer[32]; bool has_tx; uint8_t tx_sig[64]; } badge_pay_status_t;
#define BADGE_PAY_TIMEOUT_MS 1500             /* == badge.pay.TIMEOUT_MS; the deadline is badge_wallet_info().deadline_ms */
/* ttl_s above 120 is clamped to 120. bad_arg (amount or ttl_s 0), over_limit, busy, denied, not_ready,
   no_display, rate_limited, rejected, approval_timeout, sign_failed. */
badge_err_t badge_pay_request(uint64_t amount, uint16_t ttl_s, uint8_t id_out[8]);
/* ttl_s above 600 is clamped to 600. bad_arg, busy, denied, not_ready, no_display, rate_limited, rejected, approval_timeout. */
badge_err_t badge_pay_receive(uint16_t ttl_s);
/* F19 (stretch): sign and send the receipt to the badge that sent the PAID hint. Once per session.
   denied, no_session (session not in state paid), rate_limited (already sent), sign_failed, io. */
badge_err_t badge_pay_receipt(void);
void        badge_pay_cancel(void);
void        badge_pay_status(badge_pay_status_t *out);
size_t      badge_pay_inbox(badge_pay_req_t *out, size_t max);
void        badge_pay_dismiss(const uint8_t id[8]);
/* hello, challenge, paid: denied, bad_arg, not_ready, io (ESP-NOW off or send refused);
   challenge also rate_limited after 3 attempts for one request. */
badge_err_t badge_pay_hello(const uint8_t mac[6]);
bool        badge_pay_peer(const uint8_t mac[6], badge_pay_peer_t *out);
badge_err_t badge_pay_challenge(const uint8_t mac[6], const uint8_t *id_or_null);
badge_presence_t badge_pay_presence(const uint8_t mac[6], uint32_t *elapsed_ms);
badge_err_t badge_pay_paid(const uint8_t mac[6], const uint8_t id[8], const uint8_t tx_sig[64]);

/* ---- history (CAP_WALLET) ---- */
typedef struct { uint8_t dir; uint8_t status; bool verified; bool has_sig; uint8_t peer[32]; char name[33];
                 uint64_t amount; uint8_t sig[64]; uint32_t uptime_s;
                 bool this_boot;      /* false: written before the last reboot, uptime_s is not comparable with now */
                 bool receipt;        /* F19: a valid co-signed receipt was received for this payment */
               } badge_history_t;   /* dir 0 out, 1 in; status 0 signed 1 submitted 2 confirmed 3 failed */
size_t      badge_history_list(badge_history_t *out, size_t max);                /* newest first */
badge_err_t badge_history_mark(const uint8_t sig[64], uint8_t status);           /* denied, bad_arg (no such signature, bad status), io */
badge_err_t badge_history_add(const badge_history_t *entry);                     /* denied, bad_arg, io; this_boot and receipt are ignored */

/* Error codes of the upstream-mirroring functions
     badge_storage_read            bad_arg (path leaves the app directory), io (missing or unreadable), too_long (file larger than cap)
     badge_storage_write/append    bad_arg, io
     badge_http_get/post           denied, bad_arg (URL is not http:// or https://), no_network, timeout, io,
                                   too_long (body larger than cap; the first cap-1 bytes are written).
                                   An HTTP status of 400 or above is BADGE_OK; read *status.
     badge_espnow_broadcast/send   denied, bad_arg, too_long (len > 240), io (ESP-NOW off or the send was refused)
     badge_system_set_name         denied, bad_arg (not 1..23 printable ASCII)
     badge_system_launch           denied, bad_arg (no such app)
     badge_system_reboot           denied */

#ifdef __cplusplus
}
#endif
#endif
