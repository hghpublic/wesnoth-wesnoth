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

#define GETTEXT_DOMAIN "wesnoth-lib"

#include "font/text.hpp"

#include "font/attributes.hpp"
#include "font/cairo.hpp"
#include "font/font_config.hpp"

#include "font/pango/escape.hpp"
#include "font/pango/font.hpp"
#include "font/pango/hyperlink.hpp"
#include "font/pango/stream_ops.hpp"

#include "gettext.hpp"
#include "gui/widgets/helper.hpp"
#include "gui/core/log.hpp"
#include "sdl/point.hpp"
#include "serialization/unicode.hpp"
#include "preferences/preferences.hpp"
#include "video.hpp"

#include <cassert>
#include <cstring>
#include <stdexcept>

static lg::log_domain log_font("font");
#define DBG_FT LOG_STREAM(debug, log_font)

namespace font
{

namespace
{
void render_image_shape(cairo_t* cr, PangoAttrShape* pShape, int /* do_path */, void* /* data */)
{
	// NOTE: this data is owned by the underlying SDL_Surface. See add_attribute_image_shape
	cairo_surface_t* img = static_cast<cairo_surface_t*>(pShape->data);

	cairo_rel_move_to (cr,
		pShape->ink_rect.x/PANGO_SCALE,
		pShape->ink_rect.y/PANGO_SCALE);
	double x, y;
	cairo_get_current_point (cr, &x, &y);
	cairo_translate (cr, x, y);
	cairo_scale(cr, video::get_pixel_scale(), video::get_pixel_scale());

	cairo_set_source_surface(cr, img, 0, 0);
	cairo_rectangle(cr,
		0,
		0,
		pShape->ink_rect.width/PANGO_SCALE,
		pShape->ink_rect.height/PANGO_SCALE);
	cairo_fill(cr);
}
}

pango_text::pango_text()
	: context_(pango_font_map_create_context(pango_cairo_font_map_get_default()), g_object_unref)
	, layout_(pango_layout_new(context_.get()), g_object_unref)
	, rect_()
	, text_()
	, markedup_text_(false)
	, link_aware_(false)
	, link_color_()
	, font_class_(font::family_class::sans_serif)
	, font_size_(14)
	, font_style_(STYLE_NORMAL)
	, foreground_color_() // solid white
	, add_outline_(false)
	, maximum_width_(-1)
	, characters_per_line_(0)
	, maximum_height_(-1)
	, ellipse_mode_(PANGO_ELLIPSIZE_END)
	, alignment_(PANGO_ALIGN_LEFT)
	, maximum_length_(std::string::npos)
	, calculation_dirty_(true)
	, length_(0)
	, pixel_scale_(1)
	, surface_buffer_()
{
	// With 72 dpi the sizes are the same as with SDL_TTF so hardcoded.
	pango_cairo_context_set_resolution(context_.get(), 72.0);

	pango_layout_set_ellipsize(layout_.get(), ellipse_mode_);
	pango_layout_set_alignment(layout_.get(), alignment_);
	pango_layout_set_wrap(layout_.get(), PANGO_WRAP_WORD_CHAR);

	// TODO: phase this out in favor of a global line height attribute.
	pango_layout_set_line_spacing(layout_.get(), get_line_spacing_factor());

	cairo_font_options_t *fo = cairo_font_options_create();
	cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_FULL);
	cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_ON);
	cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_DEFAULT);

	pango_cairo_context_set_font_options(context_.get(), fo);
	cairo_font_options_destroy(fo);

	pango_cairo_context_set_shape_renderer(context_.get(), render_image_shape, nullptr, nullptr);
}

texture pango_text::render_texture(const rect& viewport)
{
	return with_draw_scale(texture(render_surface(viewport)));
}

texture pango_text::render_and_get_texture()
{
	update_pixel_scale(); // TODO: this should be in recalculate()
	recalculate();
	return with_draw_scale(texture(create_surface()));
}

surface pango_text::render_surface(const rect& viewport)
{
	update_pixel_scale(); // TODO: this should be in recalculate()
	recalculate();
	return create_surface(viewport);
}

texture pango_text::with_draw_scale(const texture& t) const
{
	texture res(t);
	res.set_draw_size(to_draw_scale(t.get_raw_size()));
	return res;
}

int pango_text::to_draw_scale(int i) const
{
	return (i + pixel_scale_ - 1) / pixel_scale_;
}

point pango_text::to_draw_scale(const point& p) const
{
	// Round up, rather than truncating.
	return {to_draw_scale(p.x), to_draw_scale(p.y)};
}

point pango_text::get_size()
{
	update_pixel_scale(); // TODO: this should be in recalculate()
	recalculate();

	return to_draw_scale({rect_.width, rect_.height});
}

bool pango_text::is_truncated() const
{
	recalculate();

	return (pango_layout_is_ellipsized(layout_.get()) != 0);
}

unsigned pango_text::insert_text(const unsigned offset, const std::string& text, const bool use_markup)
{
	if (text.empty() || length_ == maximum_length_) {
		return 0;
	}

	// do we really need that assert? utf8::insert will just append in this case, which seems fine
	assert(offset <= length_);

	unsigned len = utf8::size(text);
	if (length_ + len > maximum_length_) {
		len = maximum_length_ - length_;
	}
	const std::string insert = text.substr(0, utf8::index(text, len));
	std::string tmp = text_;
	set_text(utf8::insert(tmp, offset, insert), use_markup);
	// report back how many characters were actually inserted (e.g. to move the cursor selection)
	return len;
}

unsigned pango_text::get_byte_index(const unsigned offset, const unsigned line) const
{
	// Determing byte offset
	std::unique_ptr<PangoLayoutIter, std::function<void(PangoLayoutIter*)>> itor(
		pango_layout_get_iter(layout_.get()), pango_layout_iter_free);

	// Go the wanted line.
	if(line != 0) {

		if(static_cast<int>(line) >= pango_layout_get_line_count(layout_.get())) {
			return 0;
		}

		for(std::size_t i = 0; i < line; ++i) {
			pango_layout_iter_next_line(itor.get());
		}
	}

	// Go the wanted column.
	for(std::size_t i = 0; i < offset; ++i) {
		if(!pango_layout_iter_next_char(itor.get())) {
			// It seems that the documentation is wrong and causes and off by
			// one error... the result should be false if already at the end of
			// the data when started.
			if(i + 1 == offset) {
				break;
			}
			// Beyond data.
			return 0;
		}
	}

	// Get the byte offset
	return pango_layout_iter_get_index(itor.get());
}

point pango_text::get_cursor_position(const unsigned offset, const unsigned line) const
{
	recalculate();
	return get_cursor_pos_from_index(get_byte_index(offset, line));
}

point pango_text::get_cursor_pos_from_index(const unsigned offset) const
{
	// Convert the byte offset to a position.
	PangoRectangle rect;
	pango_layout_get_cursor_pos(layout_.get(), offset, &rect, nullptr);

	return to_draw_scale({PANGO_PIXELS(rect.x), PANGO_PIXELS(rect.y)});
}

std::size_t pango_text::get_maximum_length() const
{
	return maximum_length_;
}

std::string pango_text::get_token(const point& position, const std::string_view delim) const
{
	// Get the index of the character.
	const auto [index, _, inside_bounds] = xy_to_index(position);
	std::string txt = pango_layout_get_text(layout_.get());

	if (!inside_bounds || index < 0 || (static_cast<std::size_t>(index) >= txt.size()) || delim.find(txt.at(index)) != std::string::npos) {
		return ""; // if the index is out of bounds, or the index character is a delimiter, return nothing
	}

	std::size_t l = index;
	while (l > 0 && (delim.find(txt.at(l-1)) == std::string::npos)) {
		--l;
	}

	std::size_t r = index + 1;
	while (r < txt.size() && (delim.find(txt.at(r)) == std::string::npos)) {
		++r;
	}

	return txt.substr(l,r-l);
}

std::string pango_text::get_link(const point& position) const
{
	if (!link_aware_) {
		return "";
	}

	std::string tok = get_token(position);
	return looks_like_url(tok) ? tok : "";
}

point pango_text::get_column_line(const point& position) const
{
	// Get the index of the character.
	const auto [index, trailing, _] = xy_to_index(position);

	// Extract the line and the offset in pixels in that line.
	auto [line, offset] = index_to_line_x(index, trailing);
	offset = PANGO_PIXELS(offset);

	// Now convert this offset to a column, this way is a bit hacky but haven't
	// found a better solution yet.

	/**
	 * @todo There's still a bug left. When you select a text which is in the
	 * ellipses on the right side the text gets reformatted with ellipses on
	 * the left and the selected character is not the one under the cursor.
	 * Other widget toolkits don't show ellipses and have no indication more
	 * text is available. Haven't found what the best thing to do would be.
	 * Until that time leave it as is.
	 */
	for(std::size_t i = 0; ;++i) {
		const int pos = get_cursor_position(i, line).x;

		if(pos == offset) {
			// FIXME: return statement only inside if block.
			return point(i, line);
		}
	}
}

std::tuple<int, int, bool> pango_text::xy_to_index(const point& position) const
{
	recalculate();

	// Get the index of the character.
	int index, trailing;
	int res = pango_layout_xy_to_index(layout_.get(), position.x * PANGO_SCALE, position.y * PANGO_SCALE, &index, &trailing);
	// res is gboolean
	return { index, trailing, res != 0 };
}

void pango_text::clear_attributes()
{
	pango_layout_set_attributes(layout_.get(), nullptr);
}

void pango_text::apply_attributes(const font::attribute_list& attrs)
{
	if(PangoAttrList* current_attrs = pango_layout_get_attributes(layout_.get())) {
		attrs.splice_into(current_attrs);
	} else {
		attrs.apply_to(layout_.get());
	}
}

bool pango_text::set_text(const std::string& text, const bool markedup)
{
	if(markedup != markedup_text_ || text != text_) {
		const std::u32string wide = unicode_cast<std::u32string>(text);
		std::string narrow = unicode_cast<std::string>(wide);
		if(text != narrow) {
			ERR_GUI_L
				<< "pango_text::" << __func__
				<< " text '" << text
				<< "' contains invalid utf-8, trimmed the invalid parts.";
		}

		if(!markedup || !set_markup(narrow, *layout_)) {
			pango_layout_set_text(layout_.get(), narrow.c_str(), narrow.size());
			clear_attributes();
		}

		text_ = std::move(narrow);
		length_ = wide.size();
		markedup_text_ = markedup;
		calculation_dirty_ = true;
	}

	return true;
}

pango_text& pango_text::set_family_class(font::family_class fclass)
{
	if(fclass != font_class_) {
		font_class_ = fclass;
		calculation_dirty_ = true;
	}

	return *this;
}

pango_text& pango_text::set_font_size(unsigned font_size)
{
	font_size = prefs::get().font_scaled(font_size) * pixel_scale_;

	if(font_size != font_size_) {
		font_size_ = font_size;
		calculation_dirty_ = true;
	}

	return *this;
}

pango_text& pango_text::set_font_style(const pango_text::FONT_STYLE font_style)
{
	if(font_style != font_style_) {
		font_style_ = font_style;
		calculation_dirty_ = true;
	}

	return *this;
}

pango_text& pango_text::set_foreground_color(const color_t& color)
{
	if(color != foreground_color_) {
		foreground_color_ = color;
	}

	return *this;
}

pango_text& pango_text::set_maximum_width(int width)
{
	width *= pixel_scale_;

	if(width <= 0) {
		width = -1;
	}

	if(width != maximum_width_) {
		maximum_width_ = width;
		calculation_dirty_ = true;
	}

	return *this;
}

pango_text& pango_text::set_characters_per_line(const unsigned characters_per_line)
{
	if(characters_per_line != characters_per_line_) {
		characters_per_line_ = characters_per_line;

		calculation_dirty_ = true;
	}

	return *this;
}

pango_text& pango_text::set_maximum_height(int height, bool multiline)
{
	height *= pixel_scale_;

	if(height <= 0) {
		height = -1;
		multiline = false;
	}

	if(height != maximum_height_) {
		// assert(context_);

		// The maximum height is handled in this class' calculate_size() method.
		//
		// Although we also pass it to PangoLayout if multiline is true, the documentation of pango_layout_set_height
		// makes me wonder whether we should avoid that function completely. For example, "at least one line is included
		// in each paragraph regardless" and "may be changed in future, file a bug if you rely on the current behavior".
		pango_layout_set_height(layout_.get(), !multiline ? -1 : height * PANGO_SCALE);
		maximum_height_ = height;
		calculation_dirty_ = true;
	}

	return *this;
}

pango_text& pango_text::set_ellipse_mode(const PangoEllipsizeMode ellipse_mode)
{
	if(ellipse_mode != ellipse_mode_) {
		// assert(context_);

		pango_layout_set_ellipsize(layout_.get(), ellipse_mode);
		ellipse_mode_ = ellipse_mode;
		calculation_dirty_ = true;
	}

	// According to the docs of pango_layout_set_height, the behavior is undefined if a height other than -1 is combined
	// with PANGO_ELLIPSIZE_NONE. Wesnoth's code currently always calls set_ellipse_mode after set_maximum_height, so do
	// the cleanup here. The code in calculate_size() will still apply the maximum height after Pango's calculations.
	if(ellipse_mode_ == PANGO_ELLIPSIZE_NONE) {
		pango_layout_set_height(layout_.get(), -1);
	}

	return *this;
}

pango_text &pango_text::set_alignment(const PangoAlignment alignment)
{
	if (alignment != alignment_) {
		pango_layout_set_alignment(layout_.get(), alignment);
		alignment_ = alignment;
	}

	return *this;
}

pango_text& pango_text::set_maximum_length(const std::size_t maximum_length)
{
	if(maximum_length != maximum_length_) {
		maximum_length_ = maximum_length;
		if(length_ > maximum_length_) {
			std::string tmp = text_;
			set_text(utf8::truncate(tmp, maximum_length_), false);
		}
	}

	return *this;
}

pango_text& pango_text::set_link_aware(bool b)
{
	if (link_aware_ != b) {
		calculation_dirty_ = true;
		link_aware_ = b;
	}
	return *this;
}

pango_text& pango_text::set_link_color(const color_t& color)
{
	if(color != link_color_) {
		link_color_ = color;
		calculation_dirty_ = true;
	}

	return *this;
}

pango_text& pango_text::set_add_outline(bool do_add)
{
	if(do_add != add_outline_) {
		add_outline_ = do_add;
		//calculation_dirty_ = true;
	}

	return *this;
}

int pango_text::get_max_glyph_height() const
{
	p_font font{ get_font_families(font_class_), font_size_, font_style_ };

	PangoFont* f = pango_font_map_load_font(
		pango_cairo_font_map_get_default(),
		context_.get(),
		font.get());

	PangoFontMetrics* m = pango_font_get_metrics(f, nullptr);

	auto ascent = pango_font_metrics_get_ascent(m);
	auto descent = pango_font_metrics_get_descent(m);

	pango_font_metrics_unref(m);
	g_object_unref(f);

	return ceil(pango_units_to_double(ascent + descent) / pixel_scale_);
}

void pango_text::update_pixel_scale()
{
	const int ps = video::get_pixel_scale();
	if (ps == pixel_scale_) {
		return;
	}

	font_size_ = (font_size_ / pixel_scale_) * ps;

	if (maximum_width_ != -1) {
		maximum_width_ = (maximum_width_ / pixel_scale_) * ps;
	}

	if (maximum_height_ != -1) {
		maximum_height_ = (maximum_height_ / pixel_scale_) * ps;
	}

	calculation_dirty_ = true;
	pixel_scale_ = ps;
}

void pango_text::recalculate() const
{
	// TODO: clean up this "const everything then mutable everything" mess.
	// update_pixel_scale() should go in here. But it can't. Because things
	// are declared const which are not const.

	if(calculation_dirty_) {
		assert(layout_ != nullptr);

		calculation_dirty_ = false;
		rect_ = calculate_size(*layout_);
	}
}

PangoRectangle pango_text::calculate_size(PangoLayout& layout) const
{
	PangoRectangle size;

	p_font font{ get_font_families(font_class_), font_size_, font_style_ };
	pango_layout_set_font_description(&layout, font.get());

	int maximum_width = 0;
	if(characters_per_line_ != 0) {
		PangoFont* f = pango_font_map_load_font(
			pango_cairo_font_map_get_default(),
			context_.get(),
			font.get());

		PangoFontMetrics* m = pango_font_get_metrics(f, nullptr);

		int w = pango_font_metrics_get_approximate_char_width(m);
		w *= characters_per_line_;

		maximum_width = ceil(pango_units_to_double(w));

		pango_font_metrics_unref(m);
		g_object_unref(f);
	} else {
		maximum_width = maximum_width_;
	}

	if(maximum_width_ != -1) {
		maximum_width = std::min(maximum_width, maximum_width_);
	}

	pango_layout_set_width(&layout, maximum_width == -1
		? -1
		: maximum_width * PANGO_SCALE);
	pango_layout_get_pixel_extents(&layout, nullptr, &size);

	DBG_GUI_L << "pango_text::" << __func__
		<< " text '" << gui2::debug_truncate(text_)
		<< "' maximum_width " << maximum_width
		<< " width " << size.x + size.width
		<< ".";

	DBG_GUI_L << "pango_text::" << __func__
		<< " text '" << gui2::debug_truncate(text_)
		<< "' font_size " << font_size_
		<< " markedup_text " << markedup_text_
		<< " font_style " << std::hex << font_style_ << std::dec
		<< " maximum_width " << maximum_width
		<< " maximum_height " << maximum_height_
		<< " result " << size
		<< ".";

	if(maximum_width != -1 && size.x + size.width > maximum_width) {
		DBG_GUI_L << "pango_text::" << __func__
			<< " text '" << gui2::debug_truncate(text_)
			<< " ' width " << size.x + size.width
			<< " greater as the wanted maximum of " << maximum_width
			<< ".";
	}

	// The maximum height is handled here instead of using the library - see the comments in set_maximum_height()
	if(maximum_height_ != -1 && size.y + size.height > maximum_height_) {
		DBG_GUI_L << "pango_text::" << __func__
			<< " text '" << gui2::debug_truncate(text_)
			<< " ' height " << size.y + size.height
			<< " greater as the wanted maximum of " << maximum_height_
			<< ".";
		size.height = maximum_height_ - std::max(0, size.y);
	}

	return size;
}

/***
 * Inverse table
 *
 * Holds a high-precision inverse for each number i, that is, a number x such that x * i / 256 is close to 255.
 */
struct inverse_table
{
	unsigned values[256] {};

	constexpr inverse_table()
	{
		values[0] = 0;
		for (int i = 1; i < 256; ++i) {
			values[i] = (255 * 256) / i;
		}
	}

	unsigned operator[](uint8_t i) const { return values[i]; }
};

static constexpr inverse_table inverse_table_;

/***
 * Helper function for un-premultiplying alpha
 * Div should be the high-precision inverse for the alpha value.
 */
static void unpremultiply(uint8_t & value, const unsigned div) {
	unsigned temp = (value * div) / 256u;
	// Note: It's always the case that alpha * div < 256 if div is the inverse
	// for alpha, so if cairo is computing premultiplied alpha by rounding down,
	// this min is not necessary. However, if cairo generates illegal output,
	// the min may be selected.
	// It's probably not worth removing the min, since branch prediction will
	// make it essentially free if one of the branches is never actually
	// selected.
	value = std::min(255u, temp);
}

/**
 * Converts from cairo-format ARGB32 premultiplied alpha to plain alpha.
 * @param c a uint32 representing the color
 */
static void from_cairo_format(uint32_t & c)
{
	uint8_t a = (c >> 24) & 0xff;
	uint8_t r = (c >> 16) & 0xff;
	uint8_t g = (c >> 8) & 0xff;
	uint8_t b = c & 0xff;

	const unsigned div = inverse_table_[a];
	unpremultiply(r, div);
	unpremultiply(g, div);
	unpremultiply(b, div);

	c = (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

void pango_text::render(PangoLayout& layout, const rect& viewport)
{
	auto cairo_surface = cairo::create_surface(&surface_buffer_[0], point{ viewport.w, viewport.h }); // TODO: use rect::size
	auto cairo_context = cairo::create_context(cairo_surface);

	// Convenience pointer
	cairo_t* cr = cairo_context.get();

	if(cairo_status(cr) == CAIRO_STATUS_INVALID_SIZE) {
		throw std::length_error("Text is too long to render");
	}

	// The top-left of the text, which can be outside the area to be rendered
	cairo_move_to(cr, -viewport.x, -viewport.y);

	//
	// TODO: the outline may be slightly cut off around certain text if it renders too
	// close to the surface's edge. That causes the outline to extend just slightly
	// outside the surface's borders. I'm not sure how best to deal with this. Obviously,
	// we want to increase the surface size, but we also don't want to invalidate all
	// the placement and size calculations. Thankfully, it's not very noticeable.
	//
	// -- vultraz, 2018-03-07
	//
	if(add_outline_) {
		// Add a path to the cairo context tracing the current text.
		pango_cairo_layout_path(cr, &layout);

		// Set color for background outline (black).
		cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);

		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
		cairo_set_line_width(cr, 3.0); // Adjust as necessary

		// Stroke path to draw outline.
		cairo_stroke(cr);
	}

	// Set main text color.
	cairo_set_source_rgba(cr,
		foreground_color_.r / 255.0,
		foreground_color_.g / 255.0,
		foreground_color_.b / 255.0,
		foreground_color_.a / 255.0
	);

	if(font_style_ & pango_text::STYLE_UNDERLINE) {
		font::attribute_list list;
		list.insert(pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
		apply_attributes(list);
	}

	pango_cairo_show_layout(cr, &layout);
}

surface pango_text::create_surface()
{
	return create_surface({0, 0, rect_.x + rect_.width, rect_.y + rect_.height});
}

surface pango_text::create_surface(const rect& viewport)
{
	assert(layout_.get());

	cairo_format_t format = CAIRO_FORMAT_ARGB32;
	const int stride = cairo_format_stride_for_width(format, viewport.w);

	// The width and stride can be zero if the text is empty or the stride can be negative to indicate an error from
	// Cairo. Width isn't tested here because it's implied by stride.
	if(stride <= 0 || viewport.h <= 0) {
		surface_buffer_.clear();
		return nullptr;
	}

	DBG_FT << "creating new text surface";

	// Check to prevent arithmetic overflow when calculating (stride * height).
	// The size of the viewport should already provide a far lower limit on the
	// maximum size, but this is left in as a sanity check.
	if(viewport.h > std::numeric_limits<int>::max() / stride) {
		throw std::length_error("Text is too long to render");
	}

	// Resize buffer appropriately and set all pixel values to 0.
	surface_buffer_.assign(viewport.h * stride, 0);

	// Try rendering the whole text in one go. If this throws a length_error
	// then leave it to the caller to handle; one reason it may throw is that
	// cairo surfaces are limited to approximately 2**15 pixels in height.
	render(*layout_, viewport);

	// The cairo surface is in CAIRO_FORMAT_ARGB32 which uses
	// pre-multiplied alpha. SDL doesn't use that so the pixels need to be
	// decoded again.
	for(int y = 0; y < viewport.h; ++y) {
		uint32_t* pixels = reinterpret_cast<uint32_t*>(&surface_buffer_[y * stride]);
		for(int x = 0; x < viewport.w; ++x) {
			from_cairo_format(pixels[x]);
		}
	}

	return SDL_CreateRGBSurfaceWithFormatFrom(
		&surface_buffer_[0], viewport.w, viewport.h, 32, stride, SDL_PIXELFORMAT_ARGB8888);
}

bool pango_text::set_markup(std::string_view text, PangoLayout& layout)
{
	char* raw_text;
	std::string semi_escaped;
	bool valid = validate_markup(text, &raw_text, semi_escaped);
	if(!semi_escaped.empty()) {
		text = semi_escaped;
	}

	if(valid) {
		if(link_aware_) {
			std::string formatted_text = format_links(text);
			pango_layout_set_markup(&layout, formatted_text.c_str(), formatted_text.size());
		} else {
			pango_layout_set_markup(&layout, text.data(), text.size());
		}
	}

	return valid;
}

/**
 * Replaces all instances of URLs in a given string with formatted links
 * and returns the result.
 */
std::string pango_text::format_links(std::string_view text) const
{
	static const std::string delim = " \n\r\t";
	std::ostringstream result;

	std::size_t tok_start = 0;
	for(std::size_t pos = 0; pos < text.length(); ++pos) {
		if(delim.find(text[pos]) == std::string::npos) {
			continue;
		}

		if(const auto tok_length = pos - tok_start) {
			// Token starts from after the last delimiter up to (but not including) this delimiter
			auto token = text.substr(tok_start, tok_length);
			if(looks_like_url(token)) {
				result << format_as_link(std::string{token}, link_color_);
			} else {
				result << token;
			}
		}

		result << text[pos];
		tok_start = pos + 1;
	}

	// Deal with the remainder token
	if(tok_start < text.length()) {
		auto token = text.substr(tok_start);
		if(looks_like_url(token)) {
			result << format_as_link(std::string{token}, link_color_);
		} else {
			result << token;
		}
	}

	return result.str();
}

bool pango_text::validate_markup(std::string_view text, char** raw_text, std::string& semi_escaped) const
{
	if(pango_parse_markup(text.data(), text.size(),
		0, nullptr, raw_text, nullptr, nullptr)) {
		return true;
	}

	/*
	 * The markup is invalid. Try to recover.
	 *
	 * The pango engine tested seems to accept stray single quotes »'« and
	 * double quotes »"«. Stray ampersands »&« seem to give troubles.
	 * So only try to recover from broken ampersands, by simply replacing them
	 * with the escaped version.
	 */
	semi_escaped = semi_escape_text(text);

	/*
	 * If at least one ampersand is replaced the semi-escaped string
	 * is longer than the original. If this isn't the case then the
	 * markup wasn't (only) broken by ampersands in the first place.
	 */
	if(text.size() == semi_escaped.size()
			|| !pango_parse_markup(semi_escaped.c_str(), semi_escaped.size()
				, 0, nullptr, raw_text, nullptr, nullptr)) {

		/* Fixing the ampersands didn't work. */
		return false;
	}

	/* Replacement worked, still warn the user about the error. */
	WRN_GUI_L << "pango_text::" << __func__
			<< " text '" << text
			<< "' has unescaped ampersands '&', escaped them.";

	return true;
}

void pango_text::copy_layout_properties(PangoLayout& src, PangoLayout& dst)
{
	pango_layout_set_alignment(&dst, pango_layout_get_alignment(&src));
	pango_layout_set_height(&dst, pango_layout_get_height(&src));
	pango_layout_set_ellipsize(&dst, pango_layout_get_ellipsize(&src));
}

std::vector<std::string> pango_text::get_lines() const
{
	recalculate();

	PangoLayout* const layout = layout_.get();
	std::vector<std::string> res;
	int count = pango_layout_get_line_count(layout);

	if(count < 1) {
		return res;
	}

	using layout_iterator = std::unique_ptr<PangoLayoutIter, std::function<void(PangoLayoutIter*)>>;
	layout_iterator i{pango_layout_get_iter(layout), pango_layout_iter_free};

	res.reserve(count);

	do {
		PangoLayoutLine* ll = pango_layout_iter_get_line_readonly(i.get());
		const char* begin = &pango_layout_get_text(layout)[ll->start_index];
		res.emplace_back(begin, ll->length);
	} while(pango_layout_iter_next_line(i.get()));

	return res;
}

PangoLayoutLine* pango_text::get_line(int index)
{
	return pango_layout_get_line_readonly(layout_.get(), index);
}

std::pair<int, int> pango_text::index_to_line_x(const unsigned offset, const bool trailing) const
{
	int line_num = 0, x_pos = 0;
	pango_layout_index_to_line_x(layout_.get(), offset, trailing, &line_num, &x_pos);
	return { line_num, x_pos };
}

pango_text& get_text_renderer()
{
	static pango_text text_renderer;
	return text_renderer;
}

int get_max_height(unsigned size, font::family_class fclass, pango_text::FONT_STYLE style)
{
	// Reset metrics to defaults
	return get_text_renderer()
		.set_family_class(fclass)
		.set_font_style(style)
		.set_font_size(size)
		.get_max_glyph_height();
}

} // namespace font
