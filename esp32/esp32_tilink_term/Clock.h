/*
 * Clock.h - the time of day, from an NTP server on the network joined
 *
 * Only servers on the current network are asked: the one its DHCP server
 * announces (option 42), and its gateway. No public server is, so on a
 * network without NTP the time is never counted as set. Time learned on
 * another network does not count either: leaving a network forgets that the
 * time was set.
 *
 * The time zone is a name from the tz database, such as Europe/Lisbon, which
 * Zones.h turns into a POSIX TZ string, or such a string itself. Daylight
 * saving then changes by itself. SNTP asks again every hour.
 */
#pragma once
#include <Arduino.h>
#include <time.h>

namespace Clock {

// Before Wi-Fi is joined: DHCP's NTP server is only taken from then on.
void begin();

// Takes a zone's name, in any case: whole ("Europe/Lisbon") or only its
// city ("lisbon", "new york") when only one zone has it. Or a POSIX TZ
// string, such as "WET0WEST,M3.5.0/1,M10.5.0", told apart by its digits.
// False, and nothing changes, for a name it does not know.
bool setZone(const String &setting);
// The zone's name, or "" when a POSIX TZ string was given.
const String &zoneName();
// The POSIX TZ string in use.
const String &zone();
// What to store to get this zone back: the name, or the string.
const String &zoneSetting();

// Calls each with the names of the zones that contain part, in any case,
// and returns how many there were.
int findZones(const String &part, void (*each)(const char *name));

// Keeps the gateway among the servers: a DHCP lease that names a server
// removes the others. Call it now and then.
void poll();

// The time has come from a server on the network joined now.
bool synced();
// millis() of the last answer on this network; valid while synced().
uint32_t syncedAt();
// True once after each answer.
bool takeNews();

// The local time and its offset from Greenwich in minutes, east positive.
// False while the ESP32's clock has never been set.
bool local(struct tm &out, int16_t &zoneMinutes);

// The NTP servers asked: up to two, each an address and where it came from
// ("DHCP" or "gateway"), and whether it answered the last time it was asked.
struct Server {
  String address;
  const char *source;
  bool answered;
};
uint8_t servers(Server out[2]);

}  // namespace Clock
