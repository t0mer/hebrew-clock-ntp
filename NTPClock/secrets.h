#pragma once

// secrets.h is no longer required.
// WiFi credentials are entered through the on-device setup portal and stored
// in NVS (Preferences) — there is nothing to hardcode here.
//
// First boot (no stored creds): the device opens a setup Access Point.
//   AP SSID:  HebrewClock   (configurable in NTPClock.ino via AP_SSID)
//   Portal:   http://192.168.4.1  (captive portal opens automatically)
//   Pick your WiFi network, enter the password, tap Connect.
//
// After that the device joins your WiFi and pulls the time from NTP.
// To change networks, open the device's status page and tap "Reconfigure WiFi".
