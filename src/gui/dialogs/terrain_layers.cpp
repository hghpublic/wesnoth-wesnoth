/*
	Copyright (C) 2016 - 2025
	Part of the Battle for Wesnoth Project https://www.wesnoth.org/

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY.

	See the COPYING file for more details.
*/

#define GETTEXT_DOMAIN "wesnoth-lib"

#include "gui/dialogs/terrain_layers.hpp"

#include "display.hpp"
#include "formatter.hpp"
#include "gui/widgets/label.hpp"
#include "gui/widgets/listbox.hpp"
#include "gui/widgets/window.hpp"
#include "picture.hpp"

namespace gui2::dialogs
{

REGISTER_DIALOG(terrain_layers)

terrain_layers::terrain_layers(display_t& disp, const map_location& loc)
	: modal_dialog(window_id())
	, tile_(nullptr)
	, tile_logs_()
{
	terrain_builder& builder = disp.get_builder();
	tile_ = builder.get_tile(loc);

	assert(tile_);

	const std::string& tod_id = disp.get_time_of_day(loc).id;
	tile_->rebuild_cache(tod_id, &tile_logs_);
}

void terrain_layers::pre_show()
{
	//
	// List terrain flags
	//
	find_widget<label>("flags").set_label(utils::bullet_list(tile_->flags, 0));

	//
	// Generate terrain list
	//
	listbox& list = find_widget<listbox>("layer_list");

	int order = 1;
	for(const terrain_builder::tile::log_details& det : tile_logs_) {
		const terrain_builder::tile::rule_image_rand& ri   = *det.first;
		const terrain_builder::rule_image_variant& variant = *det.second;

		// TODO: also use random image variations (not just take 1st)
		const image::locator& img = variant.images.front().get_first_frame();
		const std::string& name = img.get_filename();
		// TODO: deal with (rarely used) ~modifications
		//const std::string& modif = img.get_modifications();
		const map_location& loc_cut = img.get_loc();

		std::ostringstream image_steam;

		const int tz = game_config::tile_size;
		rect r {0,0,tz,tz};

		const point img_size = image::get_size(img.get_filename());

		// calculate which part of the image the terrain engine uses
		if(loc_cut.valid()) {
			// copied from image.cpp : load_image_sub_file()
			r = {
				((tz * 3) / 4) * loc_cut.x
				, tz           * loc_cut.y + (tz / 2) * (loc_cut.x % 2)
				, tz
				, tz
			};

			if(img.get_center_x() >= 0 && img.get_center_y() >= 0) {
				r.x += img_size.x / 2 - img.get_center_x();
				r.y += img_size.y / 2 - img.get_center_y();
			}
		}

		image_steam << "terrain/foreground.png";

		// Cut and mask the image
		// ~CROP and ~BLIT have limitations, we do some math to avoid them
		// TODO: ^ eh? what limitations?
		rect r2{0, 0, img_size.x, img_size.y};
		r2.clip(r);
		if(!r2.empty()) {
			image_steam
				<< "~BLIT(" << name
					<< "~CROP("
						<< r2.x << "," << r2.y << ","
						<< r2.w << "," << r2.h << ")"
					<< "," << r2.x - r.x << "," << r2.y - r.y
				<< ")"
				<< "~MASK(" << "terrain/alphamask.png" << ")";
		}

		list.add_row(widget_data{
			{ "index", {
				{ "label", (formatter() << (ri->is_background() ? "B ": "F ") << order).str() }
			}},
			{ "image_used", {
				{ "label", image_steam.str() }
			}},
			{ "image_full", {
				{ "label", name + "~SCALE(72,72)" }
			}},
			{ "name", {
				{ "label", name }
			}},
			{ "loc", {
				{ "label", (formatter() << img.get_loc()).str() }
			}},
			{ "layer", {
				{ "label", std::to_string(ri->layer) }
			}},
			{ "base_x", {
				{ "label", std::to_string(ri->basex) }
			}},
			{ "base_y", {
				{ "label", std::to_string(ri->basey) }
			}},
			{ "center", {
				{ "label", (formatter() << ri->center_x << ", " << ri->center_y).str() }
			}}
		});

		++order;
	}
}

} // namespace dialogs
