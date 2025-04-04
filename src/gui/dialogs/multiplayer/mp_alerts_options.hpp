/*
	Copyright (C) 2014 - 2025
	by Chris Beck <render787@gmail.com>
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

#include "gui/dialogs/modal_dialog.hpp"

namespace gui2::dialogs
{

class mp_alerts_options : public modal_dialog
{
public:
	/** Constructor. */
	mp_alerts_options();

	/**
	 * The display function.
	 *
	 * See @ref modal_dialog for more information.
	 */
	DEFINE_SIMPLE_DISPLAY_WRAPPER(mp_alerts_options)

private:
	virtual const std::string& window_id() const override;

	virtual void pre_show() override;

	virtual void post_show() override;
};

} // namespace dialogs
