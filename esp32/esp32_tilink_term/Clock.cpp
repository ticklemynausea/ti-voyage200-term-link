#include "Clock.h"
#include <WiFi.h>
#include <esp_netif.h>
#include <esp_sntp.h>
#include "Zones.h"

namespace Clock {

namespace {

// Slot 0 is DHCP's, which lwIP fills in itself; the gateway goes in slot 1.
const uint8_t SLOT_DHCP = 0, SLOT_GATEWAY = 1;
const uint32_t POLL_MS = 10000;
// Earlier than this, the ESP32's clock was never set (it starts at 1970).
const time_t SET_SINCE = 1700000000;

String tzString, tzName;
volatile bool isSynced, news;
volatile uint32_t lastAnswer;
uint32_t lastPoll;

// Runs in lwIP's own task.
void answered(struct timeval *) {
  lastAnswer = millis();
  isSynced = true;
  news = true;
}

bool sameAs(const ip_addr_t *a, const ip_addr_t &b) {
  return a && !ip_addr_isany(a) && ip_addr_cmp(a, &b);
}

// The gateway in slot 1, unless DHCP named it already.
void addGateway() {
  IPAddress g = WiFi.gatewayIP();
  if (g == IPAddress()) return;
  ip_addr_t gateway;
  IP_ADDR4(&gateway, g[0], g[1], g[2], g[3]);
  ip_addr_t none = {};
  if (sameAs(esp_sntp_getserver(SLOT_DHCP), gateway)) {
    if (!ip_addr_isany(esp_sntp_getserver(SLOT_GATEWAY))) esp_sntp_setserver(SLOT_GATEWAY, &none);
  } else if (!sameAs(esp_sntp_getserver(SLOT_GATEWAY), gateway)) {
    esp_sntp_setserver(SLOT_GATEWAY, &gateway);
  }
}

void joined(arduino_event_id_t, arduino_event_info_t) {
  if (esp_sntp_enabled()) esp_sntp_stop();
  addGateway();
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_init();
}

// The servers belong to the network left, and so does the time.
void left(arduino_event_id_t, arduino_event_info_t) {
  if (esp_sntp_enabled()) esp_sntp_stop();
  ip_addr_t none = {};
  esp_sntp_setserver(SLOT_DHCP, &none);
  esp_sntp_setserver(SLOT_GATEWAY, &none);
  isSynced = false;
}

// The zone's POSIX string after its name in ZONES, or nullptr. Spaces stand
// for the names' underscores.
const char *lookUp(const String &setting, String &name) {
  String wanted = setting;
  wanted.replace(' ', '_');
  const char *found = nullptr;
  int matches = 0;
  for (const char *z = ZONES; *z; z += strlen(z) + 1, z += strlen(z) + 1) {
    const char *slash = strrchr(z, '/');
    const char *city = slash ? slash + 1 : z;
    if (!strcasecmp(z, wanted.c_str())) {
      name = z;
      return z + strlen(z) + 1;
    }
    if (!strcasecmp(city, wanted.c_str())) {
      matches++;
      name = z;
      found = z + strlen(z) + 1;
    }
  }
  return matches == 1 ? found : nullptr;
}

}  // namespace

void begin() {
  // The esp_sntp calls go through lwIP's task, which Wi-Fi would start only
  // later: without it they fail an assertion and the ESP32 restarts. Doing
  // it twice is harmless.
  esp_netif_init();
  esp_sntp_servermode_dhcp(true);
  sntp_set_time_sync_notification_cb(answered);
  WiFi.onEvent(joined, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(left, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent(left, ARDUINO_EVENT_WIFI_STA_LOST_IP);
}

bool setZone(const String &setting) {
  String name;
  const char *tz = lookUp(setting, name);
  if (tz) {
    tzName = name;
    tzString = tz;
  } else {
    bool digits = false;
    for (char c : setting) digits |= isdigit((unsigned char)c);
    if (!digits) return false;
    tzName = "";
    tzString = setting;
  }
  setenv("TZ", tzString.c_str(), 1);
  tzset();
  return true;
}

const String &zoneName() {
  return tzName;
}

const String &zone() {
  return tzString;
}

const String &zoneSetting() {
  return tzName.length() ? tzName : tzString;
}

int findZones(const String &part, void (*each)(const char *name)) {
  String wanted = part;
  wanted.replace(' ', '_');
  wanted.toLowerCase();
  int n = 0;
  for (const char *z = ZONES; *z; z += strlen(z) + 1, z += strlen(z) + 1) {
    String name = z;
    name.toLowerCase();
    if (name.indexOf(wanted) < 0) continue;
    each(z);
    n++;
  }
  return n;
}

void poll() {
  uint32_t now = millis();
  if (now - lastPoll < POLL_MS) return;
  lastPoll = now;
  if (WiFi.status() == WL_CONNECTED && esp_sntp_enabled()) addGateway();
}

bool synced() {
  return isSynced;
}

uint32_t syncedAt() {
  return lastAnswer;
}

bool takeNews() {
  if (!news) return false;
  news = false;
  return true;
}

bool local(struct tm &out, int16_t &zoneMinutes) {
  time_t now = time(nullptr);
  if (now < SET_SINCE) return false;
  struct tm utc;
  localtime_r(&now, &out);
  gmtime_r(&now, &utc);
  // The two differ by at most a day, also across the end of a year.
  int days = out.tm_yday - utc.tm_yday;
  if (days > 1) days = -1;
  else if (days < -1) days = 1;
  zoneMinutes = days * 1440 + (out.tm_hour - utc.tm_hour) * 60 + (out.tm_min - utc.tm_min);
  return true;
}

uint8_t servers(Server out[2]) {
  static const char *const SOURCES[2] = {"DHCP", "gateway"};
  uint8_t n = 0;
  for (uint8_t slot : {SLOT_DHCP, SLOT_GATEWAY}) {
    const ip_addr_t *a = esp_sntp_getserver(slot);
    if (!a || ip_addr_isany(a)) continue;
    char text[48];
    ipaddr_ntoa_r(a, text, sizeof(text));
    out[n].address = text;
    out[n].source = SOURCES[slot];
    out[n].answered = esp_sntp_getreachability(slot) & 1;
    n++;
  }
  return n;
}

}  // namespace Clock
