#include "lunarium_astro.h"
#include <math.h>

// Keep behavior consistent with Arduino constrain()
#ifndef constrain
#define constrain(amt,low,high) ((amt)<(low)?(low):((amt)>(high)?(high):(amt)))
#endif

double rev(double x) { return x - 360.0 * floor(x / 360.0); }
double deg2rad(double d) { return d * (M_PI / 180.0); }
double rad2deg(double r) { return r * (180.0 / M_PI); }

void getMoonCoords(double d, double &ra, double &dec, double &dist, double &lonOut) {
  // Math preserved from your validated diagnostic sketch
  double T = d / 36525.0;
  double L_p = rev(218.3164477 + 481267.88123421 * T);
  double D   = rev(297.8501921 + 445267.1114034 * T);
  double M   = rev(357.5291092 + 35999.0502909 * T);
  double Mp  = rev(134.9633964 + 477198.8675055 * T);
  double F   = rev(93.2720950 + 483202.0175233 * T);
  double E   = 1.0 - 0.002516 * T;

  double Dr = deg2rad(D), Mr = deg2rad(M), Mpr = deg2rad(Mp), Fr = deg2rad(F);

  double Sl = 6288774 * sin(Mpr) + 1274027 * sin(2*Dr - Mpr) + 658314 * sin(2*Dr);
  Sl += 213618 * sin(2*Mpr) - 185116 * E * sin(Mr);

  double Sr = -20905355 * cos(Mpr) - 3699111 * cos(2*Dr - Mpr);
  double Sb = 5128122 * sin(Fr) + 280602 * sin(Mpr + Fr);

  double lon = rev(L_p + Sl / 1000000.0);
  lonOut = lon; // Used for phase
  double lat = Sb / 1000000.0;
  dist = 385000.56 + (Sr / 1000.0);

  double eps = deg2rad(23.43929 - 0.013 * T);
  ra = rev(rad2deg(atan2(
      sin(deg2rad(lon))*cos(eps) - tan(deg2rad(lat))*sin(eps),
      cos(deg2rad(lon))
  )));
  dec = rad2deg(asin(
      sin(deg2rad(lat))*cos(eps) + cos(deg2rad(lat))*sin(eps)*sin(deg2rad(lon))
  ));
}

MoonPhaseInfo getMoonPhase(double d) {
  MoonPhaseInfo out;
  double ra, dec, dist, m_lon;
  getMoonCoords(d, ra, dec, dist, m_lon);

  // Solar Mean Longitude
  double s_lon = rev(280.466 + 36000.77 * (d / 36525.0));
  // Elongation
  double phi = rev(m_lon - s_lon);

  // Phase Angle i
  double i = 180.0 - phi;
  out.illumPct = (1.0 + cos(deg2rad(i))) / 2.0 * 100.0;

  if (phi < 15 || phi > 345) out.phaseName = "New Moon";
  else if (phi < 75) out.phaseName = "Waxing Crescent";
  else if (phi < 105) out.phaseName = "First Quarter";
  else if (phi < 165) out.phaseName = "Waxing Gibbous";
  else if (phi < 195) out.phaseName = "Full Moon";
  else if (phi < 255) out.phaseName = "Waning Gibbous";
  else if (phi < 285) out.phaseName = "Last Quarter";
  else out.phaseName = "Waning Crescent";

  return out;
}

double getAlt(double d, double userLatDeg, double userLonDeg) {
  double ra, dec, dist, m_lon;
  getMoonCoords(d, ra, dec, dist, m_lon);

  double gmst = rev(280.46061837 + 360.98564736629 * d);
  double LST = rev(gmst + userLonDeg);
  double H = deg2rad(rev(LST - ra));

  double sinAlt = sin(deg2rad(dec)) * sin(deg2rad(userLatDeg))
                + cos(deg2rad(dec)) * cos(deg2rad(userLatDeg)) * cos(H);

  double alt = rad2deg(asin(constrain(sinAlt, -1.0, 1.0)));

  // Topocentric parallax correction term (preserved)
  return alt - rad2deg(asin(6378.14 / dist)) * cos(deg2rad(alt));
}

static DateTime addSeconds(const DateTime &t, int32_t deltaSeconds) {
  return DateTime((uint32_t)(t.unixtime() + deltaSeconds));
}

MoonRiseSet findMoonRiseSetForLocalDay(
  const DateTime &localNow,
  int tzOffsetHours,
  double userLatDeg,
  double userLonDeg,
  double horizonAltDeg
) {
  MoonRiseSet result;
  result.foundRise = false;
  result.foundSet  = false;

  // Define LOCAL midnight for the day of localNow
  DateTime localDayStart(localNow.year(), localNow.month(), localNow.day(), 0, 0, 0);

  // Convert LOCAL day start -> UTC by subtracting offset hours (EST offset = -5 means UTC = local + 5h)
  // So utc = local - (tzOffsetHours*3600).
  // Example: tzOffsetHours=-5 => utc = local + 5h.
  DateTime utcDayStart = addSeconds(localDayStart, (int32_t)(-tzOffsetHours * 3600L));

  // Convert UTC unix time to "d" used by the math:
  // d = unixUTC/86400 - 10957.5  (preserved epoch mapping)
  double d_start = (double)utcDayStart.unixtime() / 86400.0 - 10957.5;

  double altPrev = getAlt(d_start, userLatDeg, userLonDeg);

  // Scan ~25h in 10-minute steps (0..90000 seconds)
  for (uint32_t t = 0; t <= 90000UL; t += 600) {
    double d_curr = d_start + (t / 86400.0);
    double altCurr = getAlt(d_curr, userLatDeg, userLonDeg);

    bool crossedUp   = (altPrev <= horizonAltDeg && altCurr >  horizonAltDeg);
    bool crossedDown = (altPrev >  horizonAltDeg && altCurr <= horizonAltDeg);

    if (crossedUp || crossedDown) {
      // Bracket in the last 10 minutes
      double low  = d_curr - (600.0 / 86400.0);
      double high = d_curr;

      // Determine if this crossing is a rise (up) or set (down) using endpoint trend
      bool isRise = (altCurr > altPrev);

      // Binary refine
      for (int i = 0; i < 20; i++) {
        double mid = (low + high) / 2.0;
        double altMid = getAlt(mid, userLatDeg, userLonDeg);

        if (altMid < horizonAltDeg) {
          // Still below horizon
          if (isRise) low = mid; else high = mid;
        } else {
          // Above horizon
          if (isRise) high = mid; else low = mid;
        }
      }

      // Convert refined UTC d -> UTC unix seconds, then to LOCAL DateTime
      uint32_t utcUnix = (uint32_t)((high + 10957.5) * 86400.0 + 0.5);
      DateTime localEvent = addSeconds(DateTime(utcUnix), (int32_t)(tzOffsetHours * 3600L));

      if (isRise && !result.foundRise) {
        result.foundRise = true;
        result.riseLocal = localEvent;
      } else if (!isRise && !result.foundSet) {
        result.foundSet = true;
        result.setLocal = localEvent;
      }

      // If we have both, we can stop early
      if (result.foundRise && result.foundSet) break;
    }

    altPrev = altCurr;
  }

  return result;
}
