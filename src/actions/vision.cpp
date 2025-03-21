/*
	Copyright (C) 2003 - 2025
	by David White <dave@whitevine.net>
	Part of the Battle for Wesnoth Project https://www.wesnoth.org/

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY.

	See the COPYING file for more details.
*/

/**
 * @file
 * Sighting.
 */

#include "actions/vision.hpp"

#include "actions/move.hpp"

#include "config.hpp"
#include "game_events/pump.hpp"
#include "log.hpp"
#include "map/map.hpp"
#include "map/label.hpp"
#include "map/location.hpp"
#include "pathfind/pathfind.hpp"
#include "play_controller.hpp"
#include "resources.hpp"
#include "team.hpp"
#include "units/unit.hpp"



static lg::log_domain log_engine("engine");
#define DBG_NG LOG_STREAM(debug, log_engine)
#define ERR_NG LOG_STREAM(err, log_engine)


static const std::string sighted_str("sighted");


void actions::create_jamming_map(std::map<map_location, int> & jamming,
                                 const team & view_team)
{
	// Reset the map.
	jamming.clear();

	// Build the map.
	for (const unit &u : resources::gameboard->units())
	{
		if ( u.jamming() < 1  ||  !view_team.is_enemy(u.side()) )
			continue;

		pathfind::jamming_path jam_path(u, u.get_location());
		for (const pathfind::paths::step& st : jam_path.destinations) {
			if ( jamming[st.curr] < st.move_left )
				jamming[st.curr] = st.move_left;
		}
	}
}


/**
 * Determines if @a loc is within @a viewer's visual range.
 * This is a moderately expensive function (vision is recalculated
 * with each call), so avoid using it heavily.
 * If @a jamming is left as nullptr, the jamming map is also calculated
 * with each invocation.
 */
static bool can_see(const unit & viewer, const map_location & loc,
                    const std::map<map_location, int> * jamming = nullptr)
{
	// Make sure we have a "jamming" map.
	std::map<map_location, int> local_jamming;
	if ( jamming == nullptr ) {
		actions::create_jamming_map(local_jamming, resources::gameboard->get_team(viewer.side()));
		jamming = &local_jamming;
	}

	// Determine which hexes this unit can see.
	pathfind::vision_path sight(viewer, viewer.get_location(), *jamming);

	return sight.destinations.contains(loc)  ||  sight.edges.count(loc) != 0;
}


namespace actions {


/**
 * Constructor from a unit.
 */
clearer_info::clearer_info(const unit & viewer) :
	underlying_id(viewer.underlying_id()),
	sight_range(viewer.vision()),
	slowed(viewer.get_state(unit::STATE_SLOWED)),
	costs(viewer.movement_type().get_vision().make_standalone())
{
}

/**
 * Constructor from a config.
 */
clearer_info::clearer_info(const config & cfg) :
	underlying_id(cfg["underlying_id"].to_size_t()),
	sight_range(cfg["vision"].to_int()),
	slowed(cfg.child_or_empty("status")["slowed"].to_bool()),
	costs(movetype::read_terrain_costs(cfg.child_or_empty("vision_costs")))
{
}

/**
 * Writes to a config.
 */
void clearer_info::write(config & cfg) const
{
	// The key and tag names are intended to mirror those used by [unit]
	// (so a clearer_info can be constructed from a unit's config).
	cfg["underlying_id"] = underlying_id;
	cfg["vision"] = sight_range;
	if ( slowed )
		cfg.add_child("status")["slowed"] = true;
	costs->write(cfg, "vision_costs");
}


/**
 * A record of a sighting event.
 * Records the unit doing a sighting, the location of that unit at the
 * time of the sighting, and the location of the sighted unit.
 */
struct shroud_clearer::sight_data {
	sight_data(std::size_t viewed_id, const map_location & viewed_loc,
	           std::size_t viewer_id, const map_location & viewer_loc) :
		seen_id(viewed_id),    seen_loc(viewed_loc),
		sighter_id(viewer_id), sighter_loc(viewer_loc)
	{}

	std::size_t       seen_id;
	map_location seen_loc;
	std::size_t       sighter_id;
	map_location sighter_loc;
};


/**
 * Convenience wrapper for adding sighting data to the sightings_ vector.
 */
inline void shroud_clearer::record_sighting(
	const unit & seen, const map_location & seen_loc,
	std::size_t sighter_id, const map_location & sighter_loc)
{
	sightings_.emplace_back(seen.underlying_id(), seen_loc, sighter_id, sighter_loc);
}


/**
 * Default constructor.
 */
shroud_clearer::shroud_clearer() : jamming_(), sightings_(), view_team_(nullptr)
{}


/**
 * Destructor.
 * The purpose of explicitly defining this is so we can log an error if the
 * sighted events were neither fired nor explicitly ignored.
 */
shroud_clearer::~shroud_clearer()
{
	if ( !sightings_.empty() ) {
		ERR_NG << sightings_.size() << " sighted events were ignored.";
	}
}

/**
 * Causes this object's "jamming" map to be recalculated.
 * This gets called as needed, and can also be manually invoked
 * via cache_units().
 * @param[in] new_team  The team whose vision will be used. If nullptr, the
 *                      jamming map will be cleared.
 */
void shroud_clearer::calculate_jamming(const team * new_team)
{
	// Reset data.
	jamming_.clear();
	view_team_ = new_team;

	if ( view_team_ == nullptr )
		return;

	// Build the map.
	create_jamming_map(jamming_, *view_team_);
}


/**
 * Clears shroud from a single location.
 * This also records sighted events for later firing.
 *
 * In a few cases, this will also clear corner hexes that otherwise would
 * not normally get cleared.
 * @param tm               The team whose fog/shroud is affected.
 * @param loc              The location to clear.
 * @param view_loc         The location viewer is assumed at (for sighted events).
 * @param event_non_loc    The unit at this location cannot be sighted
 *                         (used to prevent a unit from sighting itself).
 * @param viewer_id        The underlying ID of the unit doing the sighting (for events).
 * @param check_units      If false, there is no checking for an uncovered unit.
 * @param enemy_count      Incremented if an enemy is uncovered.
 * @param friend_count     Incremented if a friend is uncovered.
 * @param spectator        Will be told if a unit is uncovered.
 *
 * @return whether or not information was uncovered (i.e. returns true if
 *         the specified location was fogged/ shrouded under shared vision/maps).
 */
bool shroud_clearer::clear_loc(team &tm, const map_location &loc,
                               const map_location &view_loc,
                               const map_location &event_non_loc,
                               std::size_t viewer_id, bool check_units,
                               std::size_t &enemy_count, std::size_t &friend_count,
                               move_unit_spectator * spectator)
{
	const gamemap &map = resources::gameboard->map();
	// This counts as clearing a tile for the return value if it is on the
	// board and currently fogged under shared vision. (No need to explicitly
	// check for shrouded since shrouded implies fogged.)
	bool was_fogged = tm.fogged(loc);
	bool result = was_fogged && map.on_board(loc);

	// Clear the border as well as the board, so that the half-hexes
	// at the edge can also be cleared of fog/shroud.
	if ( map.on_board_with_border(loc) ) {
		// Both functions should be executed so don't use || which uses short-cut evaluation.
		// (This is different than the return value because shared vision does not apply here.)
		bool clear_shroud = tm.clear_shroud(loc);
		bool clear_fog = tm.clear_fog(loc);
		if ( clear_shroud || clear_fog ) {
			// If we are near a corner, the corner might also need to be cleared.
			// This happens at the lower-left corner and at either the upper- or
			// lower- right corner (depending on the width).

			// Lower-left corner:
			if ( loc.x == 0  &&  loc.y == map.h()-1 ) {
				const map_location corner(-1, map.h());
				tm.clear_shroud(corner);
				tm.clear_fog(corner);
			}
			// Lower-right corner, odd width:
			else if ( is_odd(map.w())  &&  loc.x == map.w()-1  &&  loc.y == map.h()-1 ) {
				const map_location corner(map.w(), map.h());
				tm.clear_shroud(corner);
				tm.clear_fog(corner);
			}
			// Upper-right corner, even width:
			else if ( is_even(map.w())  &&  loc.x == map.w()-1  &&  loc.y == 0) {
				const map_location corner(map.w(), -1);
				tm.clear_shroud(corner);
				tm.clear_fog(corner);
			}
		}
	}

	// Possible screen invalidation.
	if ( was_fogged ) {
		display::get_singleton()->invalidate(loc);
		// Need to also invalidate adjacent hexes to get rid of the "fog edge" graphics.
		for(const map_location& adj : get_adjacent_tiles(loc)) {
			display::get_singleton()->invalidate(adj);
		}
	}

	// Check for units?
	if ( result  &&  check_units  &&  loc != event_non_loc ) {
		// Uncovered a unit?
		unit_map::const_iterator sight_it = resources::gameboard->find_visible_unit(loc, tm);
		if ( sight_it.valid() ) {
			record_sighting(*sight_it, loc, viewer_id, view_loc);

			// Track this?
			if ( !sight_it->get_state(unit::STATE_PETRIFIED) ) {
				if ( tm.is_enemy(sight_it->side()) ) {
					++enemy_count;
					if ( spectator )
						spectator->add_seen_enemy(sight_it);
				} else {
					++friend_count;
					if ( spectator )
						spectator->add_seen_friend(sight_it);
				}
			}
		}
	}

	return result;
}


/**
 * Clears shroud (and fog) around the provided location for @a view_team
 * based on @a sight_range, @a costs, and @a slowed.
 * This will also record sighted events, which should be either fired or
 * explicitly dropped. (The sighter is the unit with underlying id @a viewer_id.)
 *
 * This should only be called if delayed shroud updates is off.
 * It is wasteful to call this if view_team uses neither fog nor shroud.
 *
 * @param view_loc         The location to clear fog from.
 * @param view_team        The team who will have the fog cleared from their map.
 * @param viewer_id        The underlying ID of the unit doing the sighting (for events).
 * @param sight_range
 * @param slowed           Whether the unit is slowed.
 * @param costs            The terrain costs for the unit.
 * @param real_loc         The actual location of the viewing unit.
 *                         (This is used to avoid having a unit sight itself.)
 * @param known_units      These locations are not checked for uncovered units.
 * @param enemy_count      Incremented for each enemy uncovered (excluding known_units).
 * @param friend_count     Incremented for each friend uncovered (excluding known_units).
 * @param spectator        Will be told of uncovered units (excluding known_units).
 * @param instant          If false, then drawing delays (used to make movement look better) are allowed.
 *
 * @return whether or not information was uncovered (i.e. returns true if any
 *         locations in visual range were fogged/shrouded under shared vision/maps).
 */
bool shroud_clearer::clear_unit(const map_location &view_loc, team &view_team,
                                std::size_t viewer_id, int sight_range, bool slowed,
                                const movetype::terrain_costs & costs,
                                const map_location & real_loc,
                                const std::set<map_location>* known_units,
                                std::size_t * enemy_count, std::size_t * friend_count,
                                move_unit_spectator * spectator, bool /*instant*/)
{
	bool cleared_something = false;
	// Dummy variables to make some logic simpler.
	std::size_t enemies=0, friends=0;
	if ( enemy_count == nullptr )
		enemy_count = &enemies;
	if ( friend_count == nullptr )
		friend_count = &friends;

	// Make sure the jamming map is up-to-date.
	if ( view_team_ != &view_team ) {
		calculate_jamming(&view_team);
	}

	// Determine the hexes to clear.
	pathfind::vision_path sight(costs, slowed, sight_range, view_loc, jamming_);

	// Clear the fog.
	for (const pathfind::paths::step &dest : sight.destinations) {
		bool known = known_units  &&  known_units->count(dest.curr) != 0;
		if ( clear_loc(view_team, dest.curr, view_loc, real_loc, viewer_id, !known,
		               *enemy_count, *friend_count, spectator) )
			cleared_something = true;
	}
	//TODO guard with game_config option
	for (const map_location &dest : sight.edges) {
		bool known = known_units  &&  known_units->count(dest) != 0;
		if ( clear_loc(view_team, dest, view_loc, real_loc, viewer_id, !known,
		               *enemy_count, *friend_count, spectator) )
			cleared_something = true;
	}

	return cleared_something;
}


/**
 * Clears shroud (and fog) around the provided location for @a view_team
 * as if @a viewer was standing there.
 * This will also record sighted events, which should be either fired or
 * explicitly dropped.
 *
 * This should only be called if delayed shroud updates is off.
 * It is wasteful to call this if view_team uses neither fog nor shroud.
 *
 * @param view_loc         The location to clear fog from.
 * @param viewer           The unit whose vision range will be used to clear the fog.
 * @param view_team        The team who will have the fog cleared from their map.
 * @param known_units      These locations are not checked for uncovered units.
 * @param enemy_count      Incremented for each enemy uncovered (excluding known_units).
 * @param friend_count     Incremented for each friend uncovered (excluding known_units).
 * @param spectator        Will be told of uncovered units (excluding known_units).
 * @param instant          If false, then drawing delays (used to make movement look better) are allowed.
 *
 * @return whether or not information was uncovered (i.e. returns true if any
 *         locations in visual range were fogged/shrouded under shared vision/maps).
 */
bool shroud_clearer::clear_unit(const map_location &view_loc,
                                const unit &viewer, team &view_team,
                                const std::set<map_location>* known_units,
                                std::size_t * enemy_count, std::size_t * friend_count,
                                move_unit_spectator * spectator, bool instant)
{
	// This is just a translation to the more general interface. It is
	// not inlined so that vision.hpp does not have to include unit.hpp.
	return clear_unit(view_loc, view_team, viewer.underlying_id(),
	                  viewer.vision(), viewer.get_state(unit::STATE_SLOWED),
	                  viewer.movement_type().get_vision(), viewer.get_location(),
	                  known_units, enemy_count, friend_count, spectator, instant);
}


/**
 * Clears shroud (and fog) around the provided location for @a view_team
 * as if @a viewer was standing there.
 * This will also record sighted events, which should be either fired or
 * explicitly dropped.
 *
 * This should only be called if delayed shroud updates is off.
 * It is wasteful to call this if view_team uses neither fog nor shroud.
 *
 * @param view_loc         The location to clear fog from.
 * @param viewer           The unit whose vision range will be used to clear the fog.
 * @param view_team        The team who will have the fog cleared from their map.
 * @param instant          If false, then drawing delays (used to make movement look better) are allowed.
 *
 * @return whether or not information was uncovered (i.e. returns true if any
 *         locations in visual range were fogged/shrouded under shared vision/maps).
 */
bool shroud_clearer::clear_unit(const map_location &view_loc, team &view_team,
                                const clearer_info &viewer, bool instant)
{
	// Locate the unit in question.
	unit_map::const_iterator find_it = resources::gameboard->units().find(viewer.underlying_id);
	const map_location & real_loc = find_it == resources::gameboard->units().end() ?
		                                map_location::null_location() :
		                                find_it->get_location();

	return clear_unit(view_loc, view_team, viewer.underlying_id,
	                  viewer.sight_range, viewer.slowed, *viewer.costs,
	                  real_loc, nullptr, nullptr, nullptr, nullptr, instant);
}


/**
 * Clears shroud (and fog) around the provided location as if @a viewer
 * was standing there.
 * This version of shroud_clearer::clear_unit() will abort if the viewer's
 * team uses neither fog nor shroud. If @a can_delay is left as true, then
 * this function also aborts on the viewing team's turn if delayed shroud
 * updates is on. (Not supplying a team suggests that it would be inconvenient
 * for the caller to check these.)
 * In addition, if @a invalidate is left as true, invalidate_after_clear()
 * will be called.
 * Setting @a instant to false allows some drawing delays that are used to
 * make movement look better.
 *
 * @return whether or not information was uncovered (i.e. returns true if any
 *         locations in visual range were fogged/shrouded under shared vision/maps).
 */
bool shroud_clearer::clear_unit(const map_location &view_loc, const unit &viewer,
                                bool can_delay, bool invalidate, bool instant)
{
	team & viewing_team = resources::gameboard->get_team(viewer.side());

	// Abort if there is nothing to clear.
	if ( !viewing_team.fog_or_shroud() )
		return false;
	if ( can_delay  &&  !viewing_team.auto_shroud_updates()  &&
	     viewer.side() == resources::controller->current_side()  )
		return false;

	if ( !clear_unit(view_loc, viewer, viewing_team, instant) )
		// Nothing uncovered.
		return false;

	if ( invalidate )
		invalidate_after_clear();

	return true;
}


/**
 * Clears shroud (and fog) at the provided location and its immediate neighbors.
 * This is an aid for the [teleport] action, allowing the destination to be
 * cleared before teleporting, while the unit's full visual range gets cleared
 * after.
 * The @a viewer is needed for correct firing of sighted events.
 *
 * @return whether or not information was uncovered (i.e. returns true if the
 *         locations in question were fogged/shrouded under shared vision/maps).
 */
bool shroud_clearer::clear_dest(const map_location &dest, const unit &viewer)
{
	team & viewing_team = resources::gameboard->get_team(viewer.side());
	// A pair of dummy variables needed to simplify some logic.
	std::size_t enemies, friends;

	// Abort if there is nothing to clear.
	if ( !viewing_team.fog_or_shroud() )
		return false;

	// Cache some values.
	const map_location & real_loc = viewer.get_location();
	const std::size_t viewer_id = viewer.underlying_id();

	// Clear the destination.
	bool cleared_something = clear_loc(viewing_team, dest, dest, real_loc,
	                                   viewer_id, true, enemies, friends);

	// Clear the adjacent hexes (will be seen even if vision is 0, and the
	// graphics do not work so well for an isolated cleared hex).
	for(const map_location& adj : get_adjacent_tiles(dest)) {
		if(clear_loc(viewing_team, adj, dest, real_loc, viewer_id, true, enemies, friends)) {
			cleared_something = true;
		}
	}

	if ( cleared_something )
		invalidate_after_clear();

	return cleared_something;
}


/**
 * Clears the record of sighted events from earlier fog/shroud clearing.
 * This should be called if the events are to be ignored and not fired.
 * (Non-cleared, non-fired events will be logged as an error.)
 */
void shroud_clearer::drop_events()
{
	if ( !sightings_.empty() ) {
		DBG_NG << sightings_.size() << " sighted events were dropped.";
	}
	sightings_.clear();
}


/**
 * Fires the sighted events that were recorded by earlier fog/shroud clearing.
 * @return true if the events have mutated the game state.
 */
game_events::pump_result_t shroud_clearer::fire_events()
{
	const unit_map & units = resources::gameboard->units();

	// Possible/probable quick abort.
	if ( sightings_.empty() )
		return game_events::pump_result_t();

	// In case of exceptions, clear sightings_ before processing events.
	std::vector<sight_data> sight_list;
	sight_list.swap(sightings_);

	for (const sight_data & event : sight_list) {
		// Try to locate the sighting unit.
		unit_map::const_iterator find_it = units.find(event.sighter_id);
		const map_location & sight_loc =
			find_it == units.end() ? map_location::null_location() :
			                         find_it->get_location();

		{	// Raise the event based on the latest data.
			resources::game_events->pump().raise(sighted_str,
			      game_events::entity_location(event.seen_loc, event.seen_id),
			      game_events::entity_location(sight_loc, event.sighter_id, event.sighter_loc));
		}
	}

	return resources::game_events->pump()();
}


/**
 * The invalidations that should occur after invoking clear_unit().
 * This is separate since clear_unit() might be invoked several
 * times in a row, and the invalidations might only need to be done once.
 */
void shroud_clearer::invalidate_after_clear()
{
	display::get_singleton()->invalidate_game_status();
	display::get_singleton()->recalculate_minimap();
	display::get_singleton()->labels().recalculate_shroud();
	// The tiles are invalidated as they are cleared, so no need
	// to invalidate them here.
}


/**
 * Returns the sides that cannot currently see @a target.
 * (Used to cache visibility before a move.)
 */
std::vector<int> get_sides_not_seeing(const unit & target)
{
	std::vector<int> not_seeing;
	for(const team& t : resources::gameboard->teams()) {
		if(!target.is_visible_to_team(t, false)) {
			not_seeing.push_back(t.side());
		}
	}

	return not_seeing;
}

/**
 * Fires sighted events for the sides that can see @a target.
 * If @a cache is supplied, only those sides might get events.
 * If @a cache is nullptr, all sides might get events.
 * This function is for the sighting *of* units that clear the shroud; it is
 * the complement of shroud_clearer::fire_events(), which handles sighting *by*
 * units that clear the shroud.
 *
 * See get_sides_not_seeing() for a way to obtain a cache.
 *
 * @returns true if an event has mutated the game state.
 */
game_events::pump_result_t actor_sighted(const unit & target, const std::vector<int> * cache)
/* Current logic:
 * 1) One event is fired per side that can see the target.
 * 2) The second unit for the event is one that can see the target, if possible.
 * 3) If no units on a side can see the target, a second unit is chosen as
 *    close as possible (but this behavior should not be relied on; it is
 *    subject to change at any time, should it become inconvenient).
 * 4) A side with no units at all will not get a sighted event.
 * 5) Sides that do not use fog or shroud CAN get sighted events.
 */
{
	const std::vector<team> & teams = resources::gameboard->teams();
	const std::size_t teams_size = teams.size();
	const map_location & target_loc = target.get_location();

	// Determine the teams that (probably) should get events.
	boost::dynamic_bitset<> needs_event;
	needs_event.resize(teams_size, cache == nullptr);
	if ( cache != nullptr ) {
		// Flag just the sides in the cache as needing events.
		for (int side : *cache)
			needs_event[side-1] = true;
	}
	// Exclude the target's own team.
	needs_event[target.side()-1] = false;
	// Exclude those teams that cannot see the target.
	for ( std::size_t i = 0; i != teams_size; ++i )
		needs_event[i] = needs_event[i] && target.is_visible_to_team(teams[i], false);

	// Cache "jamming".
	std::vector< std::map<map_location, int>> jamming_cache(teams_size);
	for ( std::size_t i = 0; i != teams_size; ++i )
		if ( needs_event[i] )
			create_jamming_map(jamming_cache[i], teams[i]);

	// Look for units that can be used as the second unit in sighted events.
	std::vector<const unit *> second_units(teams_size, nullptr);
	std::vector<std::size_t> distances(teams_size, std::numeric_limits<unsigned>::max());
	for (const unit & viewer : resources::gameboard->units()) {
		const std::size_t index = viewer.side() - 1;
		// Does viewer belong to a team for which we still need a unit?
		if ( needs_event[index]  &&  distances[index] != 0 ) {
			if ( can_see(viewer, target_loc, &jamming_cache[index]) ) {
				// Definitely use viewer as the second unit.
				second_units[index] = &viewer;
				distances[index] = 0;
			}
			else {
				// Consider viewer as a backup if it is close.
				std::size_t viewer_distance =
					distance_between(target_loc, viewer.get_location());
				if ( viewer_distance < distances[index] ) {
					second_units[index] = &viewer;
					distances[index] = viewer_distance;
				}
			}
		}
	}

	// Raise events for the appropriate teams.
	const game_events::entity_location target_entity(target);
	for ( std::size_t i = 0; i != teams_size; ++i )
		if ( second_units[i] != nullptr ) {
			resources::game_events->pump().raise(sighted_str, target_entity, game_events::entity_location(*second_units[i]));
		}

	// Fire the events and return.
	return resources::game_events->pump()();
}


/**
 * Function that recalculates the fog of war.
 *
 * This is used at the end of a turn and for the defender at the end of
 * combat. As a back-up, it is also called when clearing shroud at the
 * beginning of a turn.
 * This function does nothing if the indicated side does not use fog.
 * This function ignores the "delayed shroud updates" setting.
 * The display is invalidated as needed.
 *
 * @param[in] side The side whose fog will be recalculated.
 */
void recalculate_fog(int side)
{
	team &tm = resources::gameboard->get_team(side);

	if (!tm.uses_fog())
		return;

	// Exclude currently seen units from sighted events.
	std::set<map_location> visible_locs;
	for (const unit &u : resources::gameboard->units()) {
		const map_location & u_location = u.get_location();

		if ( !tm.fogged(u_location) )
			visible_locs.insert(u_location);
	}

	tm.refog();
	// Invalidate the screen before clearing the shroud.
	// This speeds up the invalidations within clear_shroud_unit().
	display::get_singleton()->invalidate_all();

	shroud_clearer clearer;
	for (const unit &u : resources::gameboard->units())
	{
		if ( u.side() == side )
			clearer.clear_unit(u.get_location(), u, tm, &visible_locs);
	}
	// Update the screen.
	clearer.invalidate_after_clear();

	// Fire any sighted events we picked up.
	clearer.fire_events();
}


/**
 * Function that will clear shroud (and fog) based on current unit positions.
 *
 * This will not re-fog hexes unless reset_fog is set to true.
 * This function will do nothing if the side uses neither shroud nor fog.
 * This function ignores the "delayed shroud updates" setting.
 * The display is invalidated as needed.
 *
 * @param[in] side         The side whose shroud (and fog) will be cleared.
 * @param[in] reset_fog    If set to true, the fog will also be recalculated
 *                         (refogging hexes that can no longer be seen).
 * @param[in] fire_events  If set to false, sighted events will not be fired.
 * @returns true if some shroud/fog is actually cleared away.
 */
bool clear_shroud(int side, bool reset_fog, bool fire_events)
{
	team &tm = resources::gameboard->get_team(side);
	if (!tm.uses_shroud() && !tm.uses_fog())
		return false;

	bool result = false;

	shroud_clearer clearer;
	for (const unit &u : resources::gameboard->units())
	{
		if ( u.side() == side )
			result |= clearer.clear_unit(u.get_location(), u, tm);
	}
	// Update the screen.
	if ( result )
		clearer.invalidate_after_clear();

	// Sighted events.
	if ( fire_events )
		clearer.fire_events();
	else
		clearer.drop_events();

	if ( reset_fog ) {
		// Note: This will not reveal any new tiles, so result is not affected.
		//       Also, we do not have to check fire_events at this point.
		recalculate_fog(side);
	}

	return result;
}

}//namespace actions
