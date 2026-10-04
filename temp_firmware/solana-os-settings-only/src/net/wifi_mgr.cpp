#include "wifi_mgr.h"

#include <WiFi.h>
#include <esp_eap_client.h>
#include <esp_sntp.h>
#include <time.h>

#include "../badge_log.h"
#include "../config.h"
#include "../settings.h"
#include "cert_store.h"
#include "espnow_mgr.h"

namespace wifi_mgr {
namespace {

Mode sMode = Mode::Off;
bool sEnterpriseActive = false;
bool sScanning = false;
int sScanResults = 0;
uint32_t sConnectStartedAt = 0;
uint32_t sLastChannel = 0;

// Why the last join attempt failed, from the driver's STA_DISCONNECTED event
// (0 = no failure since the last connect). Without it every failure - wrong
// password, network out of range, RADIUS rejecting the login - showed up as the
// same endless "connecting". Written on the Wi-Fi event task, read and logged
// from update() on the main loop.
volatile uint8_t sLastReason = 0;
uint8_t sLoggedReason = 0;

// The EAP CA certificate PEM, kept alive for the whole lifetime of the
// association. esp_eap_client_set_ca_cert() (reached via WiFi.begin's
// enterprise overload) stores the pointer WITHOUT copying - the core keeps
// g_wpa_ca_cert = ca_cert - so a function-local String would be freed on return
// and the asynchronous TLS handshake would then validate against freed heap.
// Holding the PEM here outlives connectEnterprise(); clearEnterpriseState()
// releases it and resets the core's pointer so a later no-CA join cannot reuse
// a stale certificate.
String sCaCertPem;

// Give up on a join after this long and drop back to idle rather than sitting
// in "connecting" forever.
constexpr uint32_t CONNECT_TIMEOUT_MS = 20000;

void noteChannelChange() {
  const uint8_t channel = WiFi.channel();
  if (channel != sLastChannel) {
    sLastChannel = channel;
    espnow_mgr::notifyChannelChanged(channel);
  }
}

// The badge has no RTC. Seeding the clock from the build timestamp gets it into
// the right decade, which is enough for a TLS stack to behave sanely; the
// validity-window check is disabled separately (see EAP_DISABLE_TIME_CHECK).
void seedClockFromBuild() {
  if (time(nullptr) > 1600000000) return;  // already set by something else

  struct tm parts = {};
  char month[4] = {0};
  int day = 0, year = 0, hour = 0, minute = 0, second = 0;
  if (sscanf(__DATE__, "%3s %d %d", month, &day, &year) != 3) return;
  if (sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3) return;

  static const char MONTHS[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char *at = strstr(MONTHS, month);
  if (at == nullptr) return;

  parts.tm_mon = (int)(at - MONTHS) / 3;
  parts.tm_mday = day;
  parts.tm_year = year - 1900;
  parts.tm_hour = hour;
  parts.tm_min = minute;
  parts.tm_sec = second;

  const time_t when = mktime(&parts);
  if (when <= 0) return;
  const struct timeval now = {.tv_sec = when, .tv_usec = 0};
  settimeofday(&now, nullptr);
  badge_log::tagf("wifi", "clock seeded from build date (%s)", __DATE__);
}

// SNTP. The build-date seed above is only good enough for TLS; anything that
// compares against real time (payment expiry, the R2 probe's clock row) needs
// the network clock. Started on the first successful join and left running:
// lwIP keeps polling across reconnects. sTimeSynced is set from the lwIP task
// by the sync callback and only ever goes false -> true.
volatile bool sTimeSynced = false;
volatile bool sTimeSyncLogged = false;

void onTimeSync(struct timeval *) { sTimeSynced = true; }

void startSntp() {
  static bool started = false;
  if (started) return;
  started = true;
  sntp_set_time_sync_notification_cb(onTimeSync);
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  badge_log::tagf("wifi", "SNTP started");
}

// Clears any EAP state left over from a previous association. Without this a
// PSK network attempted after an enterprise one still carries the old EAP
// config and fails to associate for no visible reason.
void clearEnterpriseState() {
  // Always clear the stored CA, even if no enterprise association is currently
  // active: the core keeps the last ca_cert pointer indefinitely, so a stale
  // pointer left here would be reused (or dangle) on the next join. Resetting it
  // to nullptr guarantees a later no-CA join does not silently validate against
  // a previous network's certificate.
  esp_eap_client_set_ca_cert(nullptr, 0);
  sCaCertPem = "";

  // Unconditional: the driver can still have enterprise mode on when our flag
  // says otherwise, and a PSK join on top of it fails with no useful reason.
  esp_wifi_sta_enterprise_disable();
  esp_eap_client_set_domain_name(nullptr);
  sEnterpriseActive = false;
}

// Short enough for the Settings row. Codes from esp_wifi_types_generic.h.
const char *reasonText(uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_AUTH_FAIL: return "auth failed";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "wrong password?";
    case WIFI_REASON_802_1X_AUTH_FAILED: return "login rejected (802.1X)";
    case WIFI_REASON_NO_AP_FOUND: return "network not found";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: return "security unsupported";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return "security too weak";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD: return "signal too weak";
    case WIFI_REASON_ASSOC_FAIL: return "association failed";
    case WIFI_REASON_BEACON_TIMEOUT: return "lost the network";
    case WIFI_REASON_CONNECTION_FAIL: return "connection failed";
    default: return "join failed";
  }
}

}  // namespace

Eap eapFromString(const String &text) {
  String lower = text;
  lower.toLowerCase();
  if (lower == "ttls") return Eap::Ttls;
  if (lower == "tls") return Eap::Tls;
  return Eap::Peap;
}

const char *eapToString(Eap method) {
  switch (method) {
    case Eap::Ttls: return "ttls";
    case Eap::Tls: return "tls";
    default: return "peap";
  }
}

namespace {

esp_eap_ttls_phase2_types ttlsPhase2FromString(const String &text) {
  String lower = text;
  lower.toLowerCase();
  if (lower == "pap") return ESP_EAP_TTLS_PHASE2_PAP;
  if (lower == "chap") return ESP_EAP_TTLS_PHASE2_CHAP;
  if (lower == "mschap") return ESP_EAP_TTLS_PHASE2_MSCHAP;
  if (lower == "eap") return ESP_EAP_TTLS_PHASE2_EAP;
  return ESP_EAP_TTLS_PHASE2_MSCHAPV2;
}

}  // namespace

void begin() {
  WiFi.persistent(false);  // we keep credentials in our own NVS namespace
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    sLastReason = info.wifi_sta_disconnected.reason;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.setHostname(settings::deviceName().c_str());
  WiFi.mode(WIFI_OFF);
  sMode = Mode::Off;

  if (settings::wifiAutoConnect()) {
    const String ssid = settings::wifiSsid();
    if (ssid.length() == 0) return;

    if (settings::wifiIsEnterprise()) {
      badge_log::tagf("wifi", "auto-connecting to '%s' (enterprise)", ssid.c_str());
      Enterprise config = settings::enterpriseConfig();
      // A saved profile with no CA was stored by a deliberate user action, so
      // honour it on auto-connect rather than failing the reconnect closed.
      config.allowNoCa = true;
      connectEnterprise(ssid, config, false);
    } else {
      badge_log::tagf("wifi", "auto-connecting to '%s'", ssid.c_str());
      connect(ssid, settings::wifiPassword(), false);
    }
  }
}

bool connect(const String &ssid, const String &password, bool save) {
  if (ssid.length() == 0) return false;

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(settings::deviceName().c_str());
  clearEnterpriseState();
  sLastReason = sLoggedReason = 0;
  WiFi.begin(ssid.c_str(), password.length() ? password.c_str() : nullptr);

  sMode = Mode::Station;
  sConnectStartedAt = millis();
  if (save) settings::setWifiCredentials(ssid, password);
  badge_log::tagf("wifi", "connecting to '%s'", ssid.c_str());
  return true;
}

bool connectEnterprise(const String &ssid, const Enterprise &config, bool save) {
  if (ssid.length() == 0) return false;
  if (!config.valid()) {
    badge_log::tagf("wifi", "enterprise profile has no username");
    return false;
  }

  // Reset any CA left over from a previous association before deciding this
  // one's, so a failure below never leaves a stale certificate armed. This also
  // resets the core's ca_cert pointer to nullptr.
  clearEnterpriseState();

  // Resolve the CA before touching the radio. A named-but-missing certificate
  // is a hard failure: quietly associating without validation would hand any
  // rogue AP the MSCHAPv2 exchange. The PEM is stored in sCaCertPem (module
  // lifetime) rather than a local, because the core keeps the pointer we pass
  // and the handshake runs asynchronously after this function returns.
  if (config.caCertName.length()) {
    sCaCertPem = cert_store::read(config.caCertName);
    if (sCaCertPem.length() == 0) {
      badge_log::tagf("wifi", "certificate '%s' is missing - refusing to connect unvalidated",
                      config.caCertName.c_str());
      return false;
    }
  } else if (!config.allowNoCa) {
    // No CA and the caller did not explicitly opt in: refuse rather than
    // silently downgrading to an unvalidated association.
    badge_log::tagf("wifi",
                    "no CA certificate set and no explicit no-CA opt-in - refusing to connect");
    return false;
  } else {
    badge_log::tagf("wifi",
                    "WARNING: connecting with NO CA certificate - the server will NOT be "
                    "validated and credentials go to whoever answers (caller opted in)");
  }

  seedClockFromBuild();

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(settings::deviceName().c_str());

  // Without a real clock the certificate's validity window cannot be judged;
  // the chain and the domain name still are. See EAP_DISABLE_TIME_CHECK in
  // config.h (owned by another module). CAVEAT: with the time check disabled an
  // expired or not-yet-valid CA still passes, so a leaked-but-expired
  // conference CA cannot be relied on for revocation - the domain-name pin
  // below is what actually binds the connection to the expected RADIUS server.
  esp_eap_client_set_disable_time_check(EAP_DISABLE_TIME_CHECK);

  // The Arduino wrapper does not expose this, and it is the check that actually
  // pins the connection to DEF CON's RADIUS server rather than to anyone
  // holding a certificate from the same CA. Equivalent to wpa_supplicant's
  // altsubject_match=DNS:wifireg.defcon.org.
  esp_eap_client_set_domain_name(config.domain.length() ? config.domain.c_str() : nullptr);

  // DEF CON's published config puts the real username in the outer identity, so
  // that is what an empty identity falls back to.
  const String identity = config.identity.length() ? config.identity : config.username;

  const int ttlsPhase2 =
      config.method == Eap::Ttls ? (int)ttlsPhase2FromString(config.ttlsPhase2) : -1;

  // The const char* overload, so an unset CA can be passed as nullptr - the
  // String overload would hand the driver a zero-length certificate instead.
  //
  // STA.connect() rather than WiFi.begin(): begin() returns STA.status(), and
  // after a failed login that is still the previous attempt's WL_CONNECT_FAILED
  // even when this attempt started fine. Reading that as "could not be started"
  // wiped the CA mid-handshake and never saved the profile, so one bad attempt
  // broke every retry until reboot.
  sLastReason = sLoggedReason = 0;
  const bool started =
      WiFi.STA.begin() &&
      WiFi.STA.connect(ssid.c_str(), (wpa2_auth_method_t)config.method, identity.c_str(),
                       config.username.c_str(), config.password.c_str(),
                       sCaCertPem.length() ? sCaCertPem.c_str() : nullptr, nullptr, nullptr,
                       ttlsPhase2);
  if (!started) {
    badge_log::tagf("wifi", "enterprise association could not be started");
    clearEnterpriseState();  // drop the CA we just staged
    return false;
  }

  sEnterpriseActive = true;
  sMode = Mode::Station;
  sConnectStartedAt = millis();
  if (save) settings::setEnterpriseCredentials(ssid, config);

  badge_log::tagf("wifi", "connecting to '%s' via EAP-%s as '%s'%s", ssid.c_str(),
                  eapToString(config.method), config.username.c_str(),
                  sCaCertPem.length() ? " (validated)" : " (UNVALIDATED)");
  return true;
}

bool usingEnterprise() {
  return sEnterpriseActive || (sMode == Mode::Off && settings::wifiIsEnterprise());
}

void disconnect() {
  WiFi.disconnect(true);
  clearEnterpriseState();
  sMode = Mode::Off;
  WiFi.mode(WIFI_OFF);
  badge_log::tagf("wifi", "disconnected");
}

bool startAccessPoint(const String &password) {
  const String name = settings::deviceName();
  const String pass = password.length() ? password : String(DEFAULT_AP_PASSWORD);

  clearEnterpriseState();
  WiFi.mode(WIFI_AP);
  // The AP is pinned to the ESP-NOW channel so enabling the hotspot does not
  // silently move every ESP-NOW peer out from under running apps.
  const bool ok = WiFi.softAP(name.c_str(), pass.c_str(), settings::espnowChannel());
  if (ok) {
    sMode = Mode::AccessPoint;
    noteChannelChange();
    badge_log::tagf("wifi", "hotspot '%s' up at %s (channel %u)", name.c_str(),
                    WiFi.softAPIP().toString().c_str(), (unsigned)WiFi.channel());
  } else {
    badge_log::tagf("wifi", "hotspot failed to start");
  }
  return ok;
}

void stop() {
  WiFi.disconnect(true);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  sMode = Mode::Off;
}

void update() {
  if (sScanning) {
    const int result = WiFi.scanComplete();
    if (result >= 0) {
      sScanResults = result;
      sScanning = false;
      badge_log::tagf("wifi", "scan found %d network(s)", result);
    } else if (result == WIFI_SCAN_FAILED) {
      sScanning = false;
      sScanResults = 0;
      badge_log::tagf("wifi", "scan failed");
    }
  }

  if (sTimeSynced && !sTimeSyncLogged) {
    sTimeSyncLogged = true;
    char text[24];
    const time_t now = time(nullptr);
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", gmtime(&now));
    badge_log::tagf("wifi", "clock set by SNTP: %s UTC", text);
  }

  if (sMode == Mode::Station) {
    static bool wasConnected = false;
    const bool now = WiFi.status() == WL_CONNECTED;
    if (now && !wasConnected) {
      sLastReason = sLoggedReason = 0;
      noteChannelChange();
      badge_log::tagf("wifi", "connected, ip %s rssi %d channel %u",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                      (unsigned)WiFi.channel());
      startSntp();
    } else if (!now && wasConnected) {
      badge_log::tagf("wifi", "link lost");
    }
    wasConnected = now;

    const uint8_t reason = sLastReason;
    if (!now && reason && reason != sLoggedReason) {
      sLoggedReason = reason;
      badge_log::tagf("wifi", "join failed: %s (reason %u)", reasonText(reason),
                      (unsigned)reason);
    }

    if (!now && sConnectStartedAt && (millis() - sConnectStartedAt) > CONNECT_TIMEOUT_MS) {
      sConnectStartedAt = 0;
      badge_log::tagf("wifi", "connect timed out");
    }
  }
}

Mode mode() { return sMode; }
uint8_t lastReason() { return sLastReason; }
bool timeSynced() { return sTimeSynced; }

bool connected() {
  if (sMode == Mode::AccessPoint) return true;
  return sMode == Mode::Station && WiFi.status() == WL_CONNECTED;
}

const char *statusText() {
  switch (sMode) {
    case Mode::Off: return "off";
    case Mode::AccessPoint: return "hotspot";
    case Mode::Station:
      if (WiFi.status() == WL_CONNECTED) return "connected";
      // The driver keeps retrying and reports "disconnected" throughout, so the
      // last failure reason is the only thing that says what is going wrong.
      if (sLastReason) return reasonText(sLastReason);
      switch (WiFi.status()) {
        case WL_NO_SSID_AVAIL: return "no such network";
        case WL_CONNECT_FAILED: return "auth failed";
        case WL_IDLE_STATUS: return "idle";
        case WL_DISCONNECTED: return "connecting";
        default: return "connecting";
      }
  }
  return "off";
}

String ssid() {
  if (sMode == Mode::AccessPoint) return settings::deviceName();
  return WiFi.SSID();
}

IPAddress ip() {
  if (sMode == Mode::AccessPoint) return WiFi.softAPIP();
  return WiFi.localIP();
}

int rssi() { return sMode == Mode::Station ? WiFi.RSSI() : 0; }
String macAddress() { return WiFi.macAddress(); }
uint8_t channel() { return WiFi.channel(); }

bool startScan() {
  if (sScanning) return true;
  // Scanning needs the station interface up even when we are not joined.
  if (sMode == Mode::Off) WiFi.mode(WIFI_STA);
  WiFi.scanDelete();
  sScanResults = 0;
  const int started = WiFi.scanNetworks(true, false);
  sScanning = (started == WIFI_SCAN_RUNNING);
  return sScanning;
}

bool scanning() { return sScanning; }
int scanResultCount() { return sScanResults; }
String scanSsid(int index) { return index < sScanResults ? WiFi.SSID(index) : String(); }
int scanRssi(int index) { return index < sScanResults ? WiFi.RSSI(index) : 0; }
bool scanEncrypted(int index) {
  return index < sScanResults && WiFi.encryptionType(index) != WIFI_AUTH_OPEN;
}

void clearScan() {
  WiFi.scanDelete();
  sScanResults = 0;
}

}  // namespace wifi_mgr
