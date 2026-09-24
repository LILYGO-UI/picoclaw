#include "components/components.hpp"

#include <cm0/typography.h>

namespace components {

namespace {

// A shared 28 px drawing grid keeps the navigation independent of icon fonts.
// Draw descriptors are copied by LVGL; no image buffers or extra lifetime state
// are attached to the item.
struct NavigationDrawing {
    lv_layer_t *layer;
    lv_area_t bounds;
    lv_color_t color;
    lv_opa_t opacity;

    void line(int x1, int y1, int x2, int y2) const
    {
        lv_draw_line_dsc_t stroke;
        lv_draw_line_dsc_init(&stroke);
        stroke.p1.x = bounds.x1 + x1;
        stroke.p1.y = bounds.y1 + y1;
        stroke.p2.x = bounds.x1 + x2;
        stroke.p2.y = bounds.y1 + y2;
        stroke.color = color;
        stroke.opa = opacity;
        stroke.width = 2;
        stroke.round_start = true;
        stroke.round_end = true;
        lv_draw_line(layer, &stroke);
    }

    void outline(int x1, int y1, int x2, int y2, int radius) const
    {
        lv_draw_rect_dsc_t shape;
        lv_draw_rect_dsc_init(&shape);
        shape.bg_opa = LV_OPA_TRANSP;
        shape.border_color = color;
        shape.border_opa = opacity;
        shape.border_width = 2;
        shape.radius = radius;
        const lv_area_t area{bounds.x1 + x1, bounds.y1 + y1, bounds.x1 + x2, bounds.y1 + y2};
        lv_draw_rect(layer, &shape, &area);
    }

    void arc(int x, int y, int radius, int start, int end) const
    {
        lv_draw_arc_dsc_t curve;
        lv_draw_arc_dsc_init(&curve);
        curve.center = {bounds.x1 + x, bounds.y1 + y};
        curve.radius = radius;
        curve.start_angle = start;
        curve.end_angle = end;
        curve.width = 2;
        curve.color = color;
        curve.opa = opacity;
        lv_draw_arc(layer, &curve);
    }
};

template <NavigationIcon icon>
void draw_navigation_icon(lv_event_t *event)
{
    auto *object = lv_event_get_target_obj(event);
    NavigationDrawing draw{lv_event_get_layer(event), {},
                           lv_obj_get_style_text_color(lv_obj_get_parent(object), LV_PART_MAIN),
                           lv_obj_get_style_opa_recursive(object, LV_PART_MAIN)};
    lv_obj_get_coords(object, &draw.bounds);

    switch (icon) {
        case NavigationIcon::Chat:
            draw.line(9, 4, 19, 4);
            draw.arc(19, 10, 7, 270, 360);
            draw.line(25, 10, 25, 14);
            draw.arc(19, 14, 7, 0, 90);
            draw.line(19, 20, 13, 20);
            draw.line(13, 20, 7, 24);
            draw.line(7, 24, 8, 20);
            draw.line(8, 20, 9, 20);
            draw.arc(9, 14, 7, 90, 180);
            draw.line(3, 14, 3, 10);
            draw.arc(9, 10, 7, 180, 270);
            break;
        case NavigationIcon::Models: {
            draw.outline(6, 6, 21, 21, 4);
            draw.outline(10, 10, 17, 17, 2);
            static constexpr int pins[] = {9, 14, 18};
            for (int pin : pins) {
                draw.line(pin, 2, pin, 5);
                draw.line(pin, 22, pin, 25);
                draw.line(2, pin, 5, pin);
                draw.line(22, pin, 25, pin);
            }
            break;
        }
        case NavigationIcon::Channels:
            draw.line(11, 9, 7, 17);
            draw.line(17, 9, 21, 17);
            draw.line(9, 21, 19, 21);
            draw.outline(10, 1, 18, 9, LV_RADIUS_CIRCLE);
            draw.outline(1, 17, 9, 25, LV_RADIUS_CIRCLE);
            draw.outline(19, 17, 27, 25, LV_RADIUS_CIRCLE);
            break;
        case NavigationIcon::Settings: {
            static constexpr lv_point_t teeth[] = {
                {11, 5}, {11, 2}, {16, 2}, {16, 5}, {18, 5}, {20, 4}, {23, 7}, {22, 9},
                {22, 11}, {25, 11}, {25, 16}, {22, 16}, {22, 18}, {23, 20}, {20, 23}, {18, 22},
                {16, 22}, {16, 25}, {11, 25}, {11, 22}, {9, 22}, {7, 23}, {4, 20}, {5, 18},
                {5, 16}, {2, 16}, {2, 11}, {5, 11}, {5, 9}, {4, 7}, {7, 4}, {9, 5},
            };
            const auto *previous = &teeth[31];
            for (const auto &point : teeth) {
                draw.line(previous->x, previous->y, point.x, point.y);
                previous = &point;
            }
            draw.outline(9, 9, 18, 18, LV_RADIUS_CIRCLE);
            break;
        }
    }
}

}  // namespace

const lv_font_t *font(std::uint32_t size) noexcept
{
    if (size == 0) return nullptr;
    const auto *result = lilygo_ui_font_get(size);
    if (!result) LV_LOG_ERROR("AppKit font is unavailable at %u px", size);
    return result;
}

bool set_text_font(lv_obj_t *object, std::uint32_t size, lv_style_selector_t selector) noexcept
{
    if (!object) return false;
    const auto *requested_font = font(size);
    if (!requested_font) return false;
    lv_obj_set_style_text_font(object, requested_font, selector);
    return true;
}

lv_obj_t *create_page(lv_obj_t *parent)
{
    auto *page = lv_obj_create(parent);
    lv_obj_set_name_static(page, "app_surface");
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(color_page_background), 0);
    lv_obj_set_style_bg_grad_dir(page, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    return page;
}

lv_obj_t *create_header(lv_obj_t *parent, const char *title_text)
{
    auto *header = lv_obj_create(parent);
    lv_obj_set_name_static(header, "app_header");
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, 88);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(header, lv_color_hex(color_border), 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(color_container_background), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(header, 0, 0);
    lv_obj_set_style_pad_hor(header, 28, 0);
    lv_obj_set_style_pad_ver(header, 18, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    auto *title = lv_label_create(header);
    lv_obj_set_name_static(title, "app_title");
    lv_label_set_text(title, title_text);
    (void)set_text_font(title, 36);
    lv_obj_set_style_text_color(title, lv_color_hex(color_container_title), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);
    return header;
}

lv_obj_t *create_card(lv_obj_t *parent)
{
    auto *card = lv_obj_create(parent);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(color_container_background), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t *create_button(lv_obj_t *parent, const char *text, lv_event_cb_t callback, void *user_data)
{
    auto *button = lv_button_create(parent);
    lv_obj_set_height(button, 56);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(color_action), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x006FE6), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_outline_width(button, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(button, lv_color_hex(color_accent_soft), LV_STATE_FOCUSED);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);

    auto *label = lv_label_create(button);
    lv_label_set_text(label, text);
    (void)set_text_font(label, 22);
    lv_obj_set_style_text_color(label, lv_color_hex(color_on_action), 0);
    lv_obj_center(label);
    return button;
}

lv_obj_t *create_icon_button(lv_obj_t *parent, const char *symbol, const char *accessible_name,
                             std::uint32_t background, lv_event_cb_t callback, void *user_data)
{
    auto *button = lv_button_create(parent);
    lv_obj_set_name(button, accessible_name);
    lv_obj_set_size(button, 52, 52);
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(background), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x006FE6), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_outline_width(button, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(button, lv_color_hex(color_accent_soft), LV_STATE_FOCUSED);
    if (callback) lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);

    auto *label = lv_label_create(button);
    lv_label_set_text(label, symbol);
    (void)set_text_font(label, 22);
    lv_obj_set_style_text_color(label, lv_color_hex(color_on_action), 0);
    lv_obj_center(label);
    return button;
}

lv_obj_t *create_navigation_item(lv_obj_t *parent, NavigationIcon icon, const char *name,
                                 const char *text, lv_event_cb_t callback, void *user_data)
{
    auto *item = lv_button_create(parent);
    lv_obj_remove_style_all(item);
    lv_obj_set_name(item, name);
    lv_obj_set_size(item, 64, 60);
    lv_obj_set_style_min_width(item, 44, 0);
    lv_obj_set_style_min_height(item, 44, 0);
    lv_obj_set_style_radius(item, 12, 0);
    lv_obj_set_style_pad_all(item, 4, 0);
    lv_obj_set_style_pad_row(item, 3, 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(item, lv_color_hex(color_control_background), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(item, lv_color_hex(color_accent_soft), LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(item, lv_color_hex(color_muted_text), 0);
    lv_obj_set_style_text_color(item, lv_color_hex(color_accent), LV_STATE_CHECKED);
    lv_obj_set_style_outline_width(item, 2, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(item, lv_color_hex(color_accent_soft), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_opa(item, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
    (void)set_text_font(item, 12);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (callback) lv_obj_add_event_cb(item, callback, LV_EVENT_CLICKED, user_data);

    auto *image = lv_obj_create(item);
    lv_obj_remove_style_all(image);
    lv_obj_set_name_static(image, "navigation_icon");
    lv_obj_set_size(image, 28, 28);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_SCROLLABLE);
    switch (icon) {
        case NavigationIcon::Chat:
            lv_obj_add_event_cb(image, draw_navigation_icon<NavigationIcon::Chat>, LV_EVENT_DRAW_MAIN, nullptr);
            break;
        case NavigationIcon::Models:
            lv_obj_add_event_cb(image, draw_navigation_icon<NavigationIcon::Models>, LV_EVENT_DRAW_MAIN, nullptr);
            break;
        case NavigationIcon::Channels:
            lv_obj_add_event_cb(image, draw_navigation_icon<NavigationIcon::Channels>, LV_EVENT_DRAW_MAIN, nullptr);
            break;
        case NavigationIcon::Settings:
            lv_obj_add_event_cb(image, draw_navigation_icon<NavigationIcon::Settings>, LV_EVENT_DRAW_MAIN, nullptr);
            break;
    }

    auto *label = lv_label_create(item);
    lv_obj_remove_style_all(label);
    lv_obj_set_name_static(label, "navigation_label");
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return item;
}

void set_navigation_item_selected(lv_obj_t *item, bool selected) noexcept
{
    if (!item) return;
    if (selected) lv_obj_add_state(item, LV_STATE_CHECKED);
    else lv_obj_remove_state(item, LV_STATE_CHECKED);
    lv_obj_invalidate(item);
}

}  // namespace components
