/*
	Copyright (C) 2008 - 2025
	by Fabian Mueller <fabianmueller5@gmx.de>
	Part of the Battle for Wesnoth Project https://www.wesnoth.org/

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY.

	See the COPYING file for more details.
*/

#pragma once

#include "editor/action/mouse/mouse_action.hpp"
#include "editor/palette/unit_palette.hpp"

class CKey;

namespace editor {

/**
 * Unit placement action class
 */
class mouse_action_unit : public mouse_action
{
public:
	mouse_action_unit(const CKey& key, unit_palette& palette)
		: mouse_action(palette, key)
		, click_(false)
		, start_hex_()
		, unit_palette_(palette)
	{
	}

	bool has_context_menu() const override {
		return true;
	}

	void move(editor_display& disp, const map_location& hex) override;

	/**
	 * TODO
	 */
	std::unique_ptr<editor_action> click_left(editor_display& disp, int x, int y) override;

	/**
	 * TODO
	 */
	std::unique_ptr<editor_action> up_left(editor_display& disp, int x, int y) override;

	std::unique_ptr<editor_action> drag_left(editor_display& disp, int x, int y, bool& partial, editor_action* last_undo) override;

	/**
	 * Drag end replaces the unit when clicked left, or adjusts
	 * the facing when clicked right.
	 */
	std::unique_ptr<editor_action> drag_end_left(editor_display& disp, int x, int y) override;

	std::unique_ptr<editor_action> click_right(editor_display& /*disp*/, int /*x*/, int /*y*/) override {
		return nullptr;
	}

	virtual void set_mouse_overlay(editor_display& disp) override;
	void set_unit_mouse_overlay(editor_display& disp, const unit_type& u);

private:
	bool click_;

	map_location start_hex_;
	unit_palette& unit_palette_;
};


} //end namespace editor
