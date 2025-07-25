/*
	Copyright (C) 2008 - 2025
	by Mark de Wever <koraq@xs4all.nl>
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

#include "gui/widgets/container_base.hpp"

#include "gui/core/widget_definition.hpp"
#include "gui/core/window_builder.hpp"

namespace gui2
{

// ------------ WIDGET -----------{

class panel : public container_base
{
public:
	/**
	 * Constructor.
	 */
	panel(const implementation::builder_styled_widget& builder, const std::string& control_type = "");

	/** See @ref container_base::get_client_rect. */
	virtual rect get_client_rect() const override;

	/** See @ref styled_widget::get_active. */
	virtual bool get_active() const override;

	/** See @ref styled_widget::get_state. */
	virtual unsigned get_state() const override;

private:
	/** See @ref widget::impl_draw_background. */
	virtual bool impl_draw_background() override;

	/** See @ref widget::impl_draw_foreground. */
	virtual bool impl_draw_foreground() override;

public:
	/** Static type getter that does not rely on the widget being constructed. */
	static const std::string& type();

private:
	/** Inherited from styled_widget, implemented by REGISTER_WIDGET. */
	virtual const std::string& get_control_type() const override;

	/** See @ref container_base::border_space. */
	virtual point border_space() const override;

	/** See @ref container_base::set_self_active. */
	virtual void set_self_active(const bool active) override;
};

// }---------- DEFINITION ---------{

struct panel_definition : public styled_widget_definition
{
	explicit panel_definition(const config& cfg);

	struct resolution : public resolution_definition
	{
		explicit resolution(const config& cfg);

		unsigned top_border;
		unsigned bottom_border;

		unsigned left_border;
		unsigned right_border;
	};
};

// }---------- BUILDER -----------{

namespace implementation
{

struct builder_panel : public builder_styled_widget
{
	explicit builder_panel(const config& cfg);

	using builder_styled_widget::build;

	virtual std::unique_ptr<widget> build() const override;

	builder_grid_ptr grid;
};

} // namespace implementation

// }------------ END --------------

} // namespace gui2
