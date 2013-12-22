/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file pathfinder_func.h General functions related to pathfinders. */

#ifndef PATHFINDER_FUNC_H
#define PATHFINDER_FUNC_H

#include "../waypoint_base.h"

/**
 * Calculates the tile of given station that is closest to a given tile
 * for this we assume the station is a rectangle,
 * as defined by its tile are (st->train_station)
 * @param station The station to calculate the distance to
 * @param tile The tile from where to calculate the distance
 * @param station_type the station type to get the closest tile of
 * @return The closest station tile to the given tile.
 */
static inline TileIndex CalcClosestStationTile(StationID station, TileIndex tile, StationType station_type)
{
	const BaseStation *st = BaseStation::Get(station);
	return st->GetClosestTile (tile, station_type);
}

#endif /* PATHFINDER_FUNC_H */
