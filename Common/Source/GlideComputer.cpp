/*
Copyright_License {

  XCSoar Glide Computer - http://www.xcsoar.org/
  Copyright (C) 2000 - 2009

	M Roberts (original release)
	Robin Birch <robinb@ruffnready.co.uk>
	Samuel Gisiger <samuel.gisiger@triadis.ch>
	Jeff Goodenough <jeff@enborne.f2s.com>
	Alastair Harrison <aharrison@magic.force9.co.uk>
	Scott Penrose <scottp@dd.com.au>
	John Wharington <jwharington@gmail.com>
	Lars H <lars_hn@hotmail.com>
	Rob Dunning <rob@raspberryridgesheepfarm.com>
	Russell King <rmk@arm.linux.org.uk>
	Paolo Ventafridda <coolwind@email.it>
	Tobias Lohner <tobias@lohner-net.de>
	Mirek Jezek <mjezek@ipplc.cz>
	Max Kellermann <max@duempel.org>

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

#include "GlideComputer.hpp"
#include "McReady.h"
#include "Protection.hpp"
#include "SettingsComputer.hpp"
#include "NMEA/Info.h"
#include "NMEA/Derived.hpp"
#include "Persist.hpp"
#include "ConditionMonitor.hpp"
#include "TeamCodeCalculation.h"

GlideComputer::GlideComputer()
{

}


void GlideComputer::ResetFlight(const bool full)
{
  ScopeLock protect(mutexGlideComputer);

  GlideComputerBlackboard::ResetFlight(full);
  GlideComputerAirData::ResetFlight(full);
  GlideComputerTask::ResetFlight(full);
  GlideComputerStats::ResetFlight(full);
}


void GlideComputer::StartTask(const bool do_advance,
			      const bool do_announce) {

  //  GlideComputerBlackboard::StartTask();
  GlideComputerStats::StartTask();

  if (do_announce) {
    AnnounceWayPointSwitch(do_advance);
  } else {
    GlideComputerTask::StartTask(do_advance, do_announce);
  }
}

void GlideComputer::Initialise()
{
  ScopeLock protect(mutexGlideComputer);

  GlideComputerBlackboard::Initialise();
  GlideComputerAirData::Initialise();
  GlideComputerTask::Initialise();
  GlideComputerStats::Initialise();
  ResetFlight(true);

  LoadCalculationsPersist(&SetCalculated());
  DeleteCalculationsPersist();
  // required to allow fail-safe operation
  // if the persistent file is corrupt and causes a crash

  ResetFlight(false);

}


void GlideComputer::DoLogging()
{
  if (GlideComputerStats::DoLogging()) {
    GlideComputerTask::DoLogging();
  }
}


bool GlideComputer::ProcessGPS()
{

  double mc = GlidePolar::GetMacCready();
  double ce = GlidePolar::GetCruiseEfficiency();

  ProcessBasic();

  ProcessBasicTask(mc, ce);

  if (!FlightTimes()) {
    return false;
  }
  ProcessVertical();

  CalculateOwnTeamCode();
  CalculateTeammateBearingRange();

  DoLogging();
  vegavoice.Update(&Basic(), &Calculated());
  ConditionMonitorsUpdate(*this);

  return true;
}


bool GlideComputer::ProcessVario()
{
  return GlideComputerAirData::ProcessVario();
}


void GlideComputer::SaveTaskSpeed(double val)
{
  GlideComputerStats::SaveTaskSpeed(val);
}


bool GlideComputer::ProcessIdle(const MapWindowProjection &map) {
  /*
  // VENTA3 Alternates
  if ( EnableAlternate1 == true ) DoAlternates(Basic, Calculated,Alternate1);
  if ( EnableAlternate2 == true ) DoAlternates(Basic, Calculated,Alternate2);
  if ( EnableBestAlternate == true ) DoAlternates(Basic, Calculated,BestAlternate);
  */

  if (!TaskIsTemporary()) {
    double mc = GlidePolar::GetMacCready();
    InSector();
    DoAutoMacCready(mc);
    IterateEffectiveMacCready();
  }

  return GlideComputerAirData::ProcessIdle(map);
}



const bool 
GlideComputer::InsideStartHeight(const DWORD Margin) const
{
  return GlideComputerTask::InsideStartHeight(Margin);
}

const bool 
GlideComputer::ValidStartSpeed(const DWORD Margin) const
{
  return GlideComputerTask::ValidStartSpeed(Margin);
}

bool
GlideComputer::IterateEffectiveMacCready()
{

}

void
GlideComputer::SetLegStart()
{
    GlideComputerTask::SetLegStart();
    GlideComputerStats::SetLegStart();
}


/////
#include "Math/NavFunctions.hpp" // used for team code
#include "InputEvents.h"
#include "SettingsComputer.hpp"
#include "Settings.hpp"
#include "WayPoint.hpp"
#include "PeriodClock.hpp"
#include "Math/Earth.hpp"

static PeriodClock last_team_code_update;
DWORD lastTeamCodeUpdateTime = GetTickCount();

void
GlideComputer::CalculateOwnTeamCode()
{
  if (!WayPointList) return;
  if (TeamCodeRefWaypoint < 0) return;

  if (!last_team_code_update.check_update(10000))
    return;

  // JMW TODO: locking
  double distance = 0;
  double bearing = 0;
  TCHAR code[10];

  LL_to_BearRange(WayPointList[TeamCodeRefWaypoint].Latitude,
                  WayPointList[TeamCodeRefWaypoint].Longitude,
                  Basic().Latitude,
                  Basic().Longitude,
                  &bearing, &distance);

  GetTeamCode(code, bearing, distance);

  SetCalculated().TeammateBearing = bearing;
  SetCalculated().TeammateRange = distance;

  _tcsncpy(SetCalculated().OwnTeamCode, code, 5);
}

void
GlideComputer::CalculateTeammateBearingRange()
{
  // JMW TODO: locking

  static bool InTeamSector = false;

  if (!WayPointList) return;
  if (TeamCodeRefWaypoint < 0) return;

  double ownDistance = 0;
  double ownBearing = 0;
  double mateDistance = 0;
  double mateBearing = 0;

  LL_to_BearRange(WayPointList[TeamCodeRefWaypoint].Latitude,
                  WayPointList[TeamCodeRefWaypoint].Longitude,
                  Basic().Latitude,
                  Basic().Longitude,
                  &ownBearing, &ownDistance);

  if (TeammateCodeValid)
    {

      CalcTeammateBearingRange(ownBearing, ownDistance,
                               TeammateCode,
                               &mateBearing, &mateDistance);

      // TODO code ....change the result of CalcTeammateBearingRange to do this !
      if (mateBearing > 180)
        {
          mateBearing -= 180;
        }
      else
        {
          mateBearing += 180;
        }


      SetCalculated().TeammateBearing = mateBearing;
      SetCalculated().TeammateRange = mateDistance;

      FindLatitudeLongitude(Basic().Latitude,
                            Basic().Longitude,
                            mateBearing,
                            mateDistance,
                            &TeammateLatitude,
                            &TeammateLongitude);

      if (mateDistance < 100 && InTeamSector==false)
        {
          InTeamSector=true;
          InputEvents::processGlideComputer(GCE_TEAM_POS_REACHED);
        }
      else if (mateDistance > 300)
        {
          InTeamSector = false;
        }
    }
  else
    {
      SetCalculated().TeammateBearing = 0;
      SetCalculated().TeammateRange = 0;
    }
}

