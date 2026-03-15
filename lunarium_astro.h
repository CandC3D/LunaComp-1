#pragma once
#include <Arduino.h>
#include <RTClib.h>

// Horizon altitude used for rise/set crossing (degrees)
#ifndef MOON_HORIZON_ALT
#define MOON_HORIZON_ALT (-0.8333)
#endif

struct MoonPhaseInfo {
  float illumPct;     // 0..100
  String phaseName;   // e.g., "Waxing Gibbous"
};

struct MoonCoordsInfo {
  double ra;    // degrees (0..360)
  double dec;   // degrees
  double dist;  // km
  double lon;   // ecliptic longitude degrees (0..360), used for phase
};

// --- Core math helpers ---
double rev(double x);
double deg2rad(double d);
double rad2deg(double r);

// --- Moon coordinates, phase, altitude ---
void getMoonCoords(double d, double &ra, double &dec, double &dist, double &lonOut);
MoonPhaseInfo getMoonPhase(double d);
double getAlt(double d, double userLatDeg, double userLonDeg);

// --- Rise/Set scan result ---
struct MoonRiseSet {
  bool foundRise;
  bool foundSet;
  DateTime riseLocal; // local time result (using tzOffsetHours)
  DateTime setLocal;  // local time result (using tzOffsetHours)
};

// Finds rise/set during the LOCAL calendar day containing localNow.
// The search window is local midnight .. ~25h, sampled at 10 min with bisection refine.
MoonRiseSet findMoonRiseSetForLocalDay(
  const DateTime &localNow,
  int tzOffsetHours,
  double userLatDeg,
  double userLonDeg,
  double horizonAltDeg = MOON_HORIZON_ALT
);
