/*
Copyright_License {

  XCSoar Glide Computer - http://www.xcsoar.org/
  Copyright (C) 2000-2011 The XCSoar Project
  A detailed list of copyright holders can be found in the file "AUTHORS".

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
}
*/


// ToDo

// adding baro alt sentance parser to support baro source priority  if (d == pDevPrimaryBaroSource){...}

#include "Device/Driver/Volkslogger.hpp"
#include "Device/Parser.hpp"
#include "Device/Driver.hpp"
#include "Device/Port.hpp"
#include "ProgressGlue.hpp"
#include "UtilsText.hpp"
#include "Device/Volkslogger/vlapi2.h"
#include "Device/Volkslogger/vlapihlp.h"
#include "Components.hpp"
#include "NMEA/Info.hpp"
#include "NMEA/InputLine.hpp"
#include "Waypoint/Waypoint.hpp"
#include "Engine/Waypoint/Waypoints.hpp"

#ifdef _UNICODE
#include <windows.h>
#endif

#include <algorithm>

class VolksloggerDevice : public AbstractDevice {
private:
  Port *port;

public:
  VolksloggerDevice(Port *_port):port(_port) {}

protected:
  bool DeclareInner(VLAPI &vl, const struct Declaration *declaration);

public:
  virtual bool ParseNMEA(const char *line, struct NMEA_INFO *info,
                         bool enable_baro);
  virtual bool Declare(const Declaration *declaration);
};

// RMN: Volkslogger
// Source data:
// $PGCS,1,0EC0,FFF9,0C6E,02*61
// $PGCS,1,0EC0,FFFA,0C6E,03*18
static bool
vl_PGCS1(NMEAInputLine &line, NMEA_INFO *GPS_INFO, bool enable_baro)
{
  GPS_STATE &gps = GPS_INFO->gps;

  if (line.read(1) != 1)
    return false;

  /* pressure sensor */
  line.skip();

  // four characers, hex, barometric altitude
  long altitude = line.read_hex(0L);

  if (enable_baro) {
    if (altitude > 60000)
      /* Assuming that altitude has wrapped around.  60 000 m occurs
         at QNH ~2000 hPa */
      altitude -= 65535;

    GPS_INFO->ProvideBaroAltitude1013(NMEA_INFO::BARO_ALTITUDE_VOLKSLOGGER,
                                      fixed(altitude));
  }

  // ExtractParameter(String,ctemp,3);
  // four characters, hex, constant.  Value 1371 (dec)

  // nSatellites = (int)(min(12,HexStrToDouble(ctemp, NULL)));

  if (gps.SatellitesUsed <= 0) {
    gps.SatellitesUsed = 4;
    // just to make XCSoar quit complaining.  VL doesn't tell how many
    // satellites it uses.  Without this XCSoar won't do wind
    // measurements.
  }

  return false;
}

bool
VolksloggerDevice::ParseNMEA(const char *String, NMEA_INFO *GPS_INFO,
                             bool enable_baro)
{
  if (!NMEAParser::NMEAChecksum(String))
    return false;

  NMEAInputLine line(String);
  char type[16];
  line.read(type, 16);

  if (strcmp(type, "$PGCS") == 0)
    return vl_PGCS1(line, GPS_INFO, enable_baro);
  else
    return false;
}

static void
CopyToNarrowBuffer(char *dest, size_t max_size, const TCHAR *src)
{
#ifdef _UNICODE
    if (WideCharToMultiByte(CP_ACP, 0, src, -1,
                            dest, max_size,
                            NULL, NULL) <= 0)
      dest[0] = 0;
#else
    strncpy(dest, src, max_size - 1);
    dest[max_size - 1] = 0;
#endif
}

static void
CopyWaypoint(VLAPI_DATA::WPT &dest, const Waypoint &src)
{
  CopyToNarrowBuffer(dest.name, sizeof(dest.name), src.Name.c_str());
  dest.lon = src.Location.Longitude.value_degrees();
  dest.lat = src.Location.Latitude.value_degrees();
}

static void
CopyTurnPoint(VLAPI_DATA::DCLWPT &dest, const Declaration::TurnPoint &src)
{
  CopyWaypoint(dest, src.waypoint);

  switch (src.shape) {
  case Declaration::TurnPoint::CYLINDER:
    dest.oztyp = VLAPI_DATA::DCLWPT::OZTYP_CYLSKT;
    dest.lw = dest.rz = src.radius;
    dest.rs = 0;
    break;

  case Declaration::TurnPoint::SECTOR:
    dest.oztyp = VLAPI_DATA::DCLWPT::OZTYP_CYLSKT;
    dest.lw = dest.rs = src.radius;
    dest.rz = 0;
    break;

  case Declaration::TurnPoint::LINE:
    dest.oztyp = VLAPI_DATA::DCLWPT::OZTYP_LINE;
    dest.lw = src.radius;
    dest.rs = dest.rz = 0;
    break;
  }

  /* auto direction */
  dest.ws = 360;
}

bool
VolksloggerDevice::DeclareInner(VLAPI &vl, const Declaration *decl)
{
  assert(decl->size() >= 2);

  if (vl.open(1, 20, 1, 38400L) != VLA_ERR_NOERR ||
      vl.read_info() != VLA_ERR_NOERR)
    return false;

  CopyToNarrowBuffer(vl.declaration.flightinfo.pilot,
		     sizeof(vl.declaration.flightinfo.pilot),
		     decl->PilotName);

  CopyToNarrowBuffer(vl.declaration.flightinfo.competitionid,
		     sizeof(vl.declaration.flightinfo.competitionid),
		     decl->AircraftRego);

  CopyToNarrowBuffer(vl.declaration.flightinfo.competitionclass,
		     sizeof(vl.declaration.flightinfo.competitionclass),
		     decl->AircraftType);

  const Waypoint *home = way_points.find_home();
  if (home != NULL)
    CopyWaypoint(vl.declaration.flightinfo.homepoint, *home);

  // start..
  CopyTurnPoint(vl.declaration.task.startpoint, decl->TurnPoints.front());

  // rest of task...
  const unsigned n = std::min(decl->size() - 2, 12u);
  for (unsigned i = 0; i < n; ++i)
    CopyTurnPoint(vl.declaration.task.turnpoints[i], decl->TurnPoints[i + 1]);

  // Finish
  CopyTurnPoint(vl.declaration.task.finishpoint, decl->TurnPoints.back());

  vl.declaration.task.nturnpoints = n;

  return vl.write_db_and_declaration() == VLA_ERR_NOERR;
}

bool
VolksloggerDevice::Declare(const Declaration *decl)
{
  if (decl->size() < 2)
    return false;

  ProgressGlue::Create(_T("Comms with Volkslogger"));

  port->StopRxThread();
  port->SetRxTimeout(500);

  // change to IO mode baud rate
  unsigned long lLastBaudrate = port->SetBaudrate(9600L);

  ProgressGlue::SetStep(1);

  VLAPI vl;
  vl.set_port(port);

  bool success = DeclareInner(vl, decl);

  vl.close(1);

  port->SetBaudrate(lLastBaudrate); // restore baudrate
  port->SetRxTimeout(0); // clear timeout
  port->StartRxThread(); // restart RX thread

  ProgressGlue::Close();

  return success;
}

static Device *
VolksloggerCreateOnPort(Port *com_port)
{
  return new VolksloggerDevice(com_port);
}

const struct DeviceRegister vlDevice = {
  _T("Volkslogger"),
  drfGPS | drfLogger | drfBaroAlt,
  VolksloggerCreateOnPort,
};
