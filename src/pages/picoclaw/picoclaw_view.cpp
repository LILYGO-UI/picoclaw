#include "pages/picoclaw/picoclaw_view.hpp"

#include "components/components.hpp"

#include <cm0/typography.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace {

struct ModelProvider {
    const char *name;
    const char *default_model;
    const char *api_base;
    const char *key;
};

constexpr ModelProvider model_providers[] = {
    {"OpenAI", "openai/gpt-4o-mini", "https://api.openai.com/v1", "openai"},
    {"Anthropic", "anthropic/claude-3-5-sonnet-latest", "https://api.anthropic.com/v1", "anthropic"},
    {"Google Gemini", "gemini/gemini-2.0-flash", "https://generativelanguage.googleapis.com/v1beta", "gemini"},
    {"DeepSeek", "deepseek/deepseek-chat", "https://api.deepseek.com/v1", "deepseek"},
    {"Ollama", "ollama/llama3.2", "http://127.0.0.1:11434/v1", "ollama"},
};
constexpr std::size_t model_provider_count = sizeof(model_providers) / sizeof(model_providers[0]);
constexpr std::int32_t model_delete_width = 88;
constexpr std::int32_t navigation_rail_width = 84;
constexpr std::int32_t navigation_item_height = 60;

std::size_t model_provider_index(const PicoClawProfile &profile)
{
    const auto provider = PicoClawModel::profile_provider(profile);
    for (std::size_t index = 1; index < model_provider_count; ++index)
        if (provider == model_providers[index].key) return index;
    return 0;
}

void make_plain(lv_obj_t *object)
{
    lv_obj_remove_style_all(object);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
}

lv_obj_t *make_label(lv_obj_t *parent, const char *text, std::uint32_t size, std::uint32_t color)
{
    auto *label = lv_label_create(parent);
    lv_label_set_text(label, text ? text : "");
    (void)components::set_text_font(label, size);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

std::string dropdown_caption(lv_obj_t *dropdown, std::string text)
{
    const auto *font = lv_obj_get_style_text_font(dropdown, LV_PART_MAIN);
    if (!font) return text;
    const auto measure = [dropdown, font](const std::string &value) {
        lv_point_t size{};
        lv_text_get_size(&size, value.c_str(), font, lv_obj_get_style_text_letter_space(dropdown, LV_PART_MAIN),
                         0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        return size.x;
    };
    const auto available = lv_obj_get_content_width(dropdown) - measure(LV_SYMBOL_DOWN) -
                           lv_obj_get_style_pad_column(dropdown, LV_PART_MAIN);
    if (measure(text) <= available) return text;
    // LVGL expands dropdown captions past their text area. Fit the caption
    // explicitly so long model/conversation names cannot cover the arrow.
    while (!text.empty() && measure(text + "…") > available) {
        auto last = text.size() - 1;
        while (last > 0 && (static_cast<unsigned char>(text[last]) & 0xC0) == 0x80) --last;
        text.resize(last);
    }
    return measure("…") <= available ? text + "…" : std::string{};
}

lv_obj_t *make_button(lv_obj_t *parent, const char *name, const char *text, std::uint32_t background,
                      std::uint32_t foreground, lv_event_cb_t callback, void *user_data)
{
    auto *button = lv_button_create(parent);
    lv_obj_set_name_static(button, name);
    lv_obj_set_height(button, 54);
    lv_obj_set_style_bg_color(button, lv_color_hex(background), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    components::set_focus_outline(button);
    if (callback) lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    auto *label = make_label(button, text, 22, foreground);
    lv_obj_center(label);
    return button;
}

lv_obj_t *make_card(lv_obj_t *parent, const char *name)
{
    auto *card = components::create_card(parent);
    lv_obj_set_name_static(card, name);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

// A settings section owns its caption, inset group and optional footnote.
// Rows share the white group surface; only the spaces between rows get a line.
lv_obj_t *make_settings_section(lv_obj_t *parent, const char *name, const char *caption, const char *footnote = nullptr)
{
    auto *section = lv_obj_create(parent);
    make_plain(section);
    lv_obj_set_size(section, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_clear_flag(section, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(section, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(section, 8, 0);
    auto *heading = make_label(section, caption, 14, components::color_muted_text);
    lv_obj_set_width(heading, LV_PCT(100));
    lv_obj_set_style_pad_hor(heading, 16, 0);

    auto *group = components::create_card(section);
    // Section names are assembled from channel metadata, so copy the name
    // into LVGL-owned storage instead of retaining a temporary string.
    lv_obj_set_name(group, name);
    lv_obj_set_size(group, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(group, 12, 0);
    lv_obj_set_style_pad_ver(group, 0, 0);
    lv_obj_set_style_pad_hor(group, 16, 0);
    lv_obj_set_style_pad_row(group, 0, 0);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
    if (footnote) {
        auto *note = make_label(section, footnote, 14, components::color_muted_text);
        lv_obj_set_width(note, LV_PCT(100));
        lv_obj_set_style_pad_hor(note, 16, 0);
        lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    }
    return group;
}

lv_obj_t *make_settings_row(lv_obj_t *parent)
{
    if (auto *previous = lv_obj_get_child(parent, -1)) {
        lv_obj_set_style_border_width(previous, 1, 0);
        lv_obj_set_style_border_side(previous, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(previous, lv_color_hex(components::color_border), 0);
    }
    auto *row = lv_obj_create(parent);
    make_plain(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, 52, 0);
    lv_obj_set_style_pad_ver(row, 4, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

lv_obj_t *make_settings_label(lv_obj_t *row, const char *text, bool stacked = false)
{
    auto *label = make_label(row, text, 17, components::color_text);
    lv_obj_set_width(label, LV_PCT(stacked ? 100 : 40));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

lv_obj_t *make_field(lv_obj_t *parent, const char *name, const char *label_text, bool password,
                     lv_event_cb_t focus_callback, void *user_data, bool stacked = false)
{
    auto *row = make_settings_row(parent);
    if (stacked) {
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_top(row, 12, 0);
    }
    make_settings_label(row, label_text, stacked);
    auto *input = lv_textarea_create(row);
    lv_obj_set_name_static(input, name);
    lv_obj_set_size(input, stacked ? LV_PCT(100) : 0, 44);
    if (!stacked) lv_obj_set_flex_grow(input, 1);
    lv_textarea_set_one_line(input, true);
    lv_textarea_set_max_length(input, password ? 511 : 255);
    lv_textarea_set_password_mode(input, password);
    lv_textarea_set_password_bullet(input, "*");
    (void)components::set_text_font(input, 17);
    lv_obj_set_style_text_color(input, lv_color_hex(components::color_muted_text), 0);
    lv_obj_set_style_text_align(input, stacked ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_bg_color(input, lv_color_hex(components::color_container_background), 0);
    lv_obj_set_style_bg_color(input, lv_color_hex(components::color_accent_soft), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(input, 0, 0);
    lv_obj_set_style_shadow_width(input, 0, 0);
    lv_obj_set_style_outline_width(input, 0, 0);
    components::set_focus_outline(input);
    lv_obj_set_style_radius(input, 6, 0);
    lv_obj_set_style_pad_hor(input, 0, 0);
    lv_obj_set_style_pad_ver(input, 12, 0);
    lv_obj_set_scrollbar_mode(input, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(input, focus_callback, LV_EVENT_FOCUSED, user_data);
    lv_obj_add_event_cb(input, focus_callback, LV_EVENT_CLICKED, user_data);
    return input;
}

lv_obj_t *make_settings_dropdown(lv_obj_t *parent, const char *name, const char *label_text)
{
    auto *row = make_settings_row(parent);
    make_settings_label(row, label_text);
    auto *dropdown = lv_dropdown_create(row);
    lv_obj_set_name_static(dropdown, name);
    lv_obj_set_size(dropdown, 0, 44);
    lv_obj_set_flex_grow(dropdown, 1);
    (void)components::set_text_font(dropdown, 17);
    lv_obj_set_style_text_color(dropdown, lv_color_hex(components::color_muted_text), 0);
    lv_obj_set_style_text_align(dropdown, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_bg_color(dropdown, lv_color_hex(components::color_container_background), 0);
    lv_obj_set_style_border_width(dropdown, 0, 0);
    components::set_focus_outline(dropdown);
    lv_obj_set_style_radius(dropdown, 6, 0);
    lv_obj_set_style_pad_hor(dropdown, 0, 0);
    lv_obj_set_style_pad_ver(dropdown, 12, 0);
    auto *list = lv_dropdown_get_list(dropdown);
    (void)components::set_text_font(list, 17);
    lv_obj_set_style_text_color(list, lv_color_hex(components::color_text), 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(components::color_container_background), 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(components::color_accent_soft), LV_PART_SELECTED);
    lv_obj_set_style_text_color(list, lv_color_hex(components::color_accent), LV_PART_SELECTED);
    return dropdown;
}

lv_obj_t *make_settings_value(lv_obj_t *parent, const char *name, const char *label_text, bool stacked = false)
{
    auto *row = make_settings_row(parent);
    lv_obj_set_style_pad_ver(row, 14, 0);
    if (stacked) {
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(row, 6, 0);
    }
    make_settings_label(row, label_text, stacked);
    auto *value = make_label(row, "", 17, components::color_muted_text);
    lv_obj_set_name_static(value, name);
    lv_obj_set_width(value, stacked ? LV_PCT(100) : 0);
    if (!stacked) lv_obj_set_flex_grow(value, 1);
    lv_obj_set_style_text_align(value, stacked ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
    return value;
}

void layout_settings_control(lv_obj_t *control, bool stacked)
{
    if (!control) return;
    auto *row   = lv_obj_get_parent(control);
    auto *label = lv_obj_get_child(row, 0);
    lv_obj_set_flex_flow(row, stacked ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_top(row, stacked ? 12 : 4, 0);
    lv_obj_set_width(label, LV_PCT(stacked ? 100 : 40));
    lv_obj_set_width(control, stacked ? LV_PCT(100) : 0);
    lv_obj_set_flex_grow(control, stacked ? 0 : 1);
    lv_obj_set_style_text_align(control, stacked ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT, 0);
}

lv_obj_t *make_settings_action(lv_obj_t *parent, const char *name, const char *text, std::uint32_t color,
                               lv_event_cb_t callback, void *user_data)
{
    auto *row    = make_settings_row(parent);
    auto *button = make_button(row, name, text, components::color_container_background, color, callback, user_data);
    lv_obj_set_size(button, LV_PCT(100), 44);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(button, lv_color_hex(components::color_control_background), LV_STATE_PRESSED);
    lv_obj_set_style_text_opa(button, LV_OPA_40, LV_STATE_DISABLED);
    auto *label = lv_obj_get_child(button, 0);
    (void)components::set_text_font(label, 17);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
    return button;
}

void set_disabled(lv_obj_t *object, bool disabled)
{
    if (!object) return;
    if (disabled) {
        lv_obj_add_state(object, LV_STATE_DISABLED);
        // FUI keeps the primary Launch action visible while the service is
        // idle/loading; disabled must remove interaction without fading the
        // card into the page background.
        lv_obj_set_style_opa(object, LV_OPA_COVER, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(object, LV_STATE_DISABLED);
    }
}

}  // namespace

PicoClawView::PicoClawView(PicoClawViewModel &view_model) noexcept : view_model_(view_model)
{
}

lv_obj_t *PicoClawView::create(lv_obj_t *parent, const char *application_name)
{
    surface_ = components::create_page(parent);
    lv_obj_set_name_static(surface_, "picoclaw_surface");
    lv_obj_add_event_cb(surface_, size_changed, LV_EVENT_SIZE_CHANGED, this);
    lv_obj_set_flex_flow(surface_, LV_FLEX_FLOW_ROW);

    main_column_ = lv_obj_create(surface_);
    make_plain(main_column_);
    lv_obj_set_name_static(main_column_, "picoclaw_main_column");
    lv_obj_set_height(main_column_, LV_PCT(100));
    lv_obj_set_width(main_column_, 0);
    lv_obj_set_flex_grow(main_column_, 1);
    lv_obj_set_flex_flow(main_column_, LV_FLEX_FLOW_COLUMN);

    header_ = lv_obj_create(main_column_);
    lv_obj_set_name_static(header_, "picoclaw_header");
    lv_obj_set_width(header_, LV_PCT(100));
    lv_obj_set_height(header_, 64);
    lv_obj_set_style_bg_color(header_, lv_color_hex(components::color_container_background), 0);
    lv_obj_set_style_bg_opa(header_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header_, 0, 0);
    lv_obj_set_style_border_width(header_, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(header_, lv_color_hex(components::color_border), 0);
    lv_obj_set_style_radius(header_, 0, 0);
    lv_obj_set_style_pad_hor(header_, 32, 0);
    lv_obj_set_style_pad_ver(header_, 8, 0);
    lv_obj_set_style_pad_column(header_, 16, 0);
    lv_obj_clear_flag(header_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(header_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    header_back_ = components::create_icon_button(header_, LV_SYMBOL_LEFT, "picoclaw_header_back",
                                                  components::color_page_background, header_back_clicked, this);
    lv_obj_set_size(header_back_, 40, 40);
    lv_obj_set_style_radius(header_back_, 20, 0);
    lv_obj_add_flag(header_back_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(lv_obj_get_child(header_back_, 0), lv_color_hex(components::color_accent), 0);
    (void)application_name;
    title_ = make_label(header_, "AI CHAT", 24, components::color_container_title);
    lv_obj_set_name_static(title_, "picoclaw_title");
    lv_obj_set_flex_grow(title_, 1);
    header_status_ = lv_obj_create(header_);
    make_plain(header_status_);
    lv_obj_set_name_static(header_status_, "picoclaw_header_status");
    lv_obj_set_size(header_status_, 62, 28);
    lv_obj_set_style_radius(header_status_, 6, 0);
    lv_obj_set_style_bg_color(header_status_, lv_color_hex(components::color_control_background), 0);
    auto *header_status_label = make_label(header_status_, "IDLE", 10, components::color_muted_text);
    lv_obj_set_name_static(header_status_label, "picoclaw_header_status_label");
    lv_obj_center(header_status_label);

    content_ = lv_obj_create(main_column_);
    make_plain(content_);
    lv_obj_set_name_static(content_, "picoclaw_content");
    lv_obj_set_width(content_, LV_PCT(100));
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    bottom_navigation_ = lv_obj_create(surface_);
    make_plain(bottom_navigation_);
    lv_obj_set_name_static(bottom_navigation_, "picoclaw_bottom_navigation");
    lv_obj_set_style_bg_color(bottom_navigation_, lv_color_hex(components::color_navigation), 0);
    lv_obj_set_style_bg_opa(bottom_navigation_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bottom_navigation_, 1, 0);
    lv_obj_set_style_border_color(bottom_navigation_, lv_color_hex(components::color_border), 0);
    lv_obj_clear_flag(bottom_navigation_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bottom_navigation_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bottom_navigation_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    bottom_chat_button_ = components::create_navigation_item(
        bottom_navigation_, components::NavigationIcon::Chat, "picoclaw_chat_tab", "Chat", chat_tab_clicked, this);
    bottom_models_button_ = components::create_navigation_item(
        bottom_navigation_, components::NavigationIcon::Models, "picoclaw_models_tab", "Models", models_tab_clicked, this);
    bottom_channels_button_ = components::create_navigation_item(
        bottom_navigation_, components::NavigationIcon::Channels, "picoclaw_channels_tab", "Channels",
        channels_tab_clicked, this);
    bottom_settings_button_ = components::create_navigation_item(
        bottom_navigation_, components::NavigationIcon::Settings, "picoclaw_settings_tab", "Settings",
        settings_tab_clicked, this);
    chat_tab_            = bottom_chat_button_;
    models_tab_          = bottom_models_button_;
    channels_tab_        = bottom_channels_button_;
    settings_tab_        = bottom_settings_button_;

    build_screen();
    lv_obj_update_layout(surface_);
    apply_layout();
    timer_ = lv_timer_create(timer_tick, 160, this);
    view_model_.request_status();
    sync_state(true);
    return surface_;
}

void PicoClawView::destroy() noexcept
{
    view_model_.cancel_weixin_binding();
    host_text_input_.focus(nullptr);
    lv_async_call_cancel(rebuild_screen_async, this);
    lv_async_call_cancel(layout_weixin_async, this);
    lv_async_call_cancel(rebuild_models_async, this);
    lv_async_call_cancel(rebuild_channels_async, this);
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    if (surface_) lv_obj_delete(surface_);
    surface_                 = nullptr;
    header_                  = nullptr;
    title_                   = nullptr;
    header_back_             = nullptr;
    header_status_           = nullptr;
    chat_tab_                = nullptr;
    models_tab_              = nullptr;
    channels_tab_            = nullptr;
    settings_tab_            = nullptr;
    content_                 = nullptr;
    bottom_navigation_       = nullptr;
    bottom_chat_button_      = nullptr;
    bottom_models_button_    = nullptr;
    bottom_channels_button_  = nullptr;
    bottom_settings_button_  = nullptr;
    clear_screen_pointers();
}

void PicoClawView::chat_tab_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view || view->view_model_.screen() == PicoClawScreen::Chat) return;
    view->hide_keyboard();
    view->view_model_.show_chat();
    view->build_screen();
}

void PicoClawView::header_back_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view) return;
    if (view->view_model_.screen() == PicoClawScreen::Channels) {
        view->hide_keyboard();
        view->view_model_.close_channel();
        lv_async_call_cancel(rebuild_channels_async, view);
        lv_async_call(rebuild_channels_async, view);
        return;
    }
    if (view->view_model_.screen() == PicoClawScreen::Models) {
        view->hide_keyboard();
        view->models_editing_ = false;
        view->models_feedback_visible_ = false;
        view->queue_models_rebuild();
        return;
    }
    view->hide_keyboard();
    if (view->view_model_.back()) view->build_screen();
}

void PicoClawView::models_tab_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view || view->view_model_.screen() == PicoClawScreen::Models) return;
    view->hide_keyboard();
    view->models_editing_ = false;
    view->models_feedback_visible_ = !view->view_model_.profiles_error().empty();
    view->view_model_.show_models();
    view->build_screen();
}

void PicoClawView::channels_tab_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view || view->view_model_.screen() == PicoClawScreen::Channels) return;
    view->hide_keyboard();
    view->view_model_.show_channels();
    view->build_screen();
}

void PicoClawView::rebuild_channels_async(void *user_data)
{
    auto *view = static_cast<PicoClawView *>(user_data);
    if (view->surface_ && view->view_model_.screen() == PicoClawScreen::Channels) view->build_screen();
}

void PicoClawView::channel_edit_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    const auto index = reinterpret_cast<std::uintptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event))) - 1U;
    if (!view || !view->view_model_.channels_loaded() || view->view_model_.busy()) return;
    view->view_model_.edit_channel(index);
    if (!view->view_model_.channel_draft()) return;
    lv_async_call_cancel(rebuild_channels_async, view);
    lv_async_call(rebuild_channels_async, view);
}

void PicoClawView::channel_field_changed(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    auto *input = lv_event_get_target_obj(event);
    const auto index = reinterpret_cast<std::uintptr_t>(lv_obj_get_user_data(input)) - 1U;
    if (!view || !view->view_model_.channel_draft()) return;
    view->view_model_.set_channel_field(index, lv_obj_check_type(input, &lv_switch_class)
        ? (lv_obj_has_state(input, LV_STATE_CHECKED) ? "true" : "false") : lv_textarea_get_text(input));
    view->sync_state(true);
}

void PicoClawView::channel_enabled_changed(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view) return;
    view->view_model_.set_channel_enabled(lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED));
    view->sync_state(true);
}

void PicoClawView::channel_save_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view) return;
    view->hide_keyboard();
    view->view_model_.save_channel();
    view->sync_state(true);
}

void PicoClawView::channels_refresh_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view) return;
    view->view_model_.load_channels();
    view->sync_state(true);
}

void PicoClawView::channel_secrets_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view || !view->view_model_.channel_draft()) return;
    view->channel_show_secrets_ = !view->channel_show_secrets_;
    const auto &fields = view->view_model_.channel_draft()->fields;
    for (std::size_t i = 0; i < fields.size(); ++i)
        if (fields[i].kind == PicoClawChannelFieldKind::Secret && view->channel_fields_[i])
            lv_textarea_set_password_mode(view->channel_fields_[i], !view->channel_show_secrets_);
    lv_label_set_text(lv_obj_get_child(lv_event_get_target_obj(event), 0),
                      view->channel_show_secrets_ ? "Hide credentials" : "Show credentials");
}

void PicoClawView::weixin_bind_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view) return;
    view->hide_keyboard();
    view->view_model_.start_weixin_binding();
    view->sync_state(true);
}

void PicoClawView::weixin_cancel_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view) return;
    view->view_model_.cancel_weixin_binding();
    view->sync_state(true);
}

void PicoClawView::queue_models_rebuild()
{
    lv_async_call_cancel(rebuild_models_async, this);
    lv_async_call(rebuild_models_async, this);
}

void PicoClawView::rebuild_models_async(void *user_data)
{
    auto *view = static_cast<PicoClawView *>(user_data);
    if (view->surface_ && view->view_model_.screen() == PicoClawScreen::Models) {
        const auto scroll_y = view->models_restore_scroll_y_;
        const bool reveal_saved = view->models_reveal_saved_;
        view->build_screen();
        if (reveal_saved && view->models_list_) {
            const auto name = "picoclaw_model_card_" + std::to_string(view->models_edit_index_);
            if (auto *card = lv_obj_find_by_name(view->models_list_, name.c_str())) {
                lv_obj_update_layout(view->surface_);
                lv_obj_scroll_to_view_recursive(card, LV_ANIM_OFF);
            }
        } else if (scroll_y >= 0 && view->models_scroll_) {
            lv_obj_update_layout(view->surface_);
            lv_obj_scroll_to_y(view->models_scroll_, scroll_y, LV_ANIM_OFF);
        }
    }
}

void PicoClawView::model_add_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    const auto provider = reinterpret_cast<std::uintptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event))) - 1U;
    if (!view || provider >= model_provider_count) return;
    view->models_provider_index_ = provider;
    view->models_adding_ = true;
    view->models_editing_ = true;
    view->models_feedback_visible_ = false;
    view->queue_models_rebuild();
}

void PicoClawView::model_import_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view) return;
    view->models_feedback_visible_ = true;
    lv_obj_clear_flag(view->models_feedback_label_, LV_OBJ_FLAG_HIDDEN);
    view->view_model_.import_models();
    view->sync_state(true);
}

void PicoClawView::model_edit_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    const auto index = reinterpret_cast<std::uintptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event))) - 1U;
    if (!view || index >= view->view_model_.profiles().size()) return;
    view->models_edit_index_ = index;
    view->models_provider_index_ = model_provider_index(view->view_model_.profiles()[index]);
    view->models_adding_ = false;
    view->models_editing_ = true;
    view->models_feedback_visible_ = false;
    view->queue_models_rebuild();
}

void PicoClawView::model_editor_name_changed(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (view) view->update_navigation();
}

void PicoClawView::model_editor_selection_changed(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view || !view->model_editor_dropdown_ || !view->model_editor_id_) return;
    view->hide_keyboard();
    const auto index = lv_dropdown_get_selected(view->model_editor_dropdown_);
    if (index >= view->model_editor_options_.size()) return;
    // Updating the input refreshes the choices, so retain the selected ID first.
    const auto selected = view->model_editor_options_[index];
    lv_textarea_set_text(view->model_editor_id_, selected.c_str());
    view->update_model_editor_dropdown();
    view->apply_layout();
}

void PicoClawView::model_editor_input_changed(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (view) view->update_model_editor_dropdown();
}

void PicoClawView::model_editor_dropdown_opened(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view || !view->model_editor_dropdown_) return;
    view->hide_keyboard();
    lv_obj_update_layout(view->surface_);
    auto *list = lv_dropdown_get_list(view->model_editor_dropdown_);
    auto *controls = lv_obj_get_parent(view->model_editor_dropdown_);
    const auto width = lv_obj_get_content_width(controls);
    lv_obj_set_style_min_width(list, width, 0);
    lv_obj_set_style_max_width(list, width, 0);
    lv_obj_set_style_translate_x(list, -lv_obj_get_x(view->model_editor_dropdown_), 0);
    lv_obj_set_style_max_height(list, std::min<lv_coord_t>(300, lv_obj_get_height(view->surface_) / 2), 0);
}

void PicoClawView::model_save_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view || !view->model_editor_key_) return;
    PicoClawProfile profile{
        lv_textarea_get_text(view->model_editor_provider_), {},
        lv_textarea_get_text(view->model_editor_id_), lv_textarea_get_text(view->model_editor_base_),
        lv_textarea_get_text(view->model_editor_key_), model_providers[view->models_provider_index_].key};
    view->hide_keyboard();
    const bool saved = view->models_adding_ ? view->view_model_.add_profile(std::move(profile))
                                          : view->view_model_.update_profile(view->models_edit_index_, std::move(profile));
    view->models_feedback_visible_ = true;
    if (saved) {
        if (view->models_adding_) view->models_edit_index_ = view->view_model_.profiles().size() - 1U;
        view->models_reveal_saved_ = true;
        view->models_editing_ = false;
        view->queue_models_rebuild();
    } else {
        lv_obj_clear_flag(view->models_feedback_label_, LV_OBJ_FLAG_HIDDEN);
        view->sync_state(true);
    }
}

void PicoClawView::model_front_key(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view || view->view_model_.busy()) return;
    auto *input = lv_indev_active();
    if (!input) return;
    const auto key = lv_indev_get_key(input);
    if (key != LV_KEY_DEL && key != LV_KEY_BACKSPACE) return;

    auto *front = lv_event_get_target_obj(event);
    auto *row = front ? lv_obj_get_parent(front) : nullptr;
    const auto index = front ? reinterpret_cast<std::uintptr_t>(lv_obj_get_user_data(front)) - 1U : 0U;
    if (!row || index >= view->view_model_.profiles().size()) return;

    // A keyboard has no swipe gesture. Open the same action row used by touch,
    // show the confirmation state immediately, and move focus to the action so
    // Enter completes the deletion without requiring a pointer.
    view->settle_model_swipe(row, true);
    auto *button = lv_obj_get_child(row, 1);
    if (!button) return;
    view->models_swipe_confirm_ = true;
    lv_label_set_text(lv_obj_get_child(button, 0), "Confirm");
    if (auto *group = lv_obj_get_group(front)) {
        if (lv_obj_get_group(button) != group) lv_group_add_obj(group, button);
        lv_group_focus_obj(button);
    }
    lv_obj_scroll_to_view_recursive(button, LV_ANIM_OFF);
}

void PicoClawView::model_delete_key(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view) return;
    auto *input = lv_indev_active();
    if (!input) return;
    const auto key = lv_indev_get_key(input);
    auto *button = lv_event_get_target_obj(event);
    auto *row = button ? lv_obj_get_parent(button) : nullptr;
    if (!row || view->models_swipe_row_ != row) return;
    if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
        view->settle_model_swipe(row, false);
        auto *front = lv_obj_get_child(row, 0);
        if (front) {
            if (auto *group = lv_obj_get_group(button)) {
                if (lv_obj_get_group(front) != group) lv_group_add_obj(group, front);
                lv_group_focus_obj(front);
            }
        }
        lv_event_stop_processing(event);
        return;
    }
    if (key != LV_KEY_DEL && key != LV_KEY_BACKSPACE) return;
    // Keep Delete/Backspace equivalent to pressing the focused Delete button.
    // This also preserves the touch path's two-step confirmation behavior.
    view->model_swipe_delete_clicked(event);
}

void PicoClawView::settle_model_swipe(lv_obj_t *row, bool open)
{
    if (!row) return;
    if (open && models_swipe_row_ && models_swipe_row_ != row) settle_model_swipe(models_swipe_row_, false);
    models_adjusting_swipe_ = true;
    auto *button = lv_obj_get_child(row, 1);
    lv_obj_scroll_to_x(row, open ? model_delete_width : 0, LV_ANIM_OFF);
    set_disabled(button, !open || view_model_.busy());
    lv_label_set_text(lv_obj_get_child(button, 0), "Delete");
    models_swipe_confirm_ = false;
    if (open) models_swipe_row_ = row;
    else if (models_swipe_row_ == row) models_swipe_row_ = nullptr;
    models_adjusting_swipe_ = false;
}

void PicoClawView::model_swipe_event(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view || view->models_adjusting_swipe_) return;
    auto *row = lv_event_get_target_obj(event);
    if (lv_event_get_code(event) == LV_EVENT_SCROLL_BEGIN) {
        if (view->models_swipe_row_ && view->models_swipe_row_ != row)
            view->settle_model_swipe(view->models_swipe_row_, false);
        view->models_swipe_row_ = row;
        view->models_swipe_confirm_ = false;
        auto *button = lv_obj_get_child(row, 1);
        lv_label_set_text(lv_obj_get_child(button, 0), "Delete");
        set_disabled(button, true);
    } else {
        view->settle_model_swipe(row, !view->view_model_.busy() && lv_obj_get_scroll_x(row) >= model_delete_width / 2);
    }
}

void PicoClawView::models_scroll_started(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (view && view->models_swipe_row_ && lv_event_get_target_obj(event) == view->models_scroll_)
        view->settle_model_swipe(view->models_swipe_row_, false);
}

void PicoClawView::model_swipe_delete_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    auto *button = lv_event_get_target_obj(event);
    auto *row = lv_obj_get_parent(button);
    const auto index = reinterpret_cast<std::uintptr_t>(lv_obj_get_user_data(button)) - 1U;
    if (!view || view->view_model_.busy() || view->models_swipe_row_ != row ||
        lv_obj_get_scroll_x(row) != model_delete_width || index >= view->view_model_.profiles().size()) return;
    if (!view->models_swipe_confirm_) {
        view->models_swipe_confirm_ = true;
        lv_label_set_text(lv_obj_get_child(button, 0), "Confirm");
        return;
    }
    view->models_feedback_visible_ = true;
    if (view->view_model_.remove_profile(index)) {
        set_disabled(button, true);
        view->models_swipe_row_ = nullptr;
        view->models_swipe_confirm_ = false;
        view->models_restore_scroll_y_ = lv_obj_get_scroll_y(view->models_scroll_);
        view->queue_models_rebuild();
    } else {
        view->models_swipe_confirm_ = false;
        lv_label_set_text(lv_obj_get_child(button, 0), "Delete");
        lv_obj_clear_flag(view->models_feedback_label_, LV_OBJ_FLAG_HIDDEN);
        view->sync_state(true);
    }
}

void PicoClawView::model_key_toggle_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view || !view->model_editor_key_) return;
    const bool masked = !lv_textarea_get_password_mode(view->model_editor_key_);
    lv_textarea_set_password_mode(view->model_editor_key_, masked);
    lv_label_set_text(lv_obj_get_child(lv_event_get_target_obj(event), 0), masked ? "Show API key" : "Hide API key");
}

void PicoClawView::settings_logs_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view || view->view_model_.screen() == PicoClawScreen::Logs) return;
    view->hide_keyboard();
    view->view_model_.show_logs();
    // The menu belongs to the Settings content being replaced. Rebuild only
    // after its click event has finished using that object.
    lv_async_call_cancel(rebuild_screen_async, view);
    lv_async_call(rebuild_screen_async, view);
    view->view_model_.request_logs();
}

void PicoClawView::rebuild_screen_async(void *user_data)
{
    auto *view = static_cast<PicoClawView *>(user_data);
    if (view->surface_) view->build_screen();
}

void PicoClawView::gateway_refresh_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view) return;
    if (view->settings_feedback_label_) lv_obj_clear_flag(view->settings_feedback_label_, LV_OBJ_FLAG_HIDDEN);
    view->view_model_.request_status();
    view->sync_state(true);
}

void PicoClawView::settings_tab_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view || view->view_model_.screen() == PicoClawScreen::Settings) return;
    view->hide_keyboard();
    view->view_model_.show_settings();
    view->build_screen();
}

void PicoClawView::send_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (view) view->send_message();
}

void PicoClawView::new_chat_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view) return;
    view->hide_keyboard();
    if (view->view_model_.new_chat()) {
        view->visible_message_count_ = PicoClawModel::maximum_messages;
        if (view->message_input_) lv_textarea_set_text(view->message_input_, "");
        view->rebuild_messages();
    }
    view->sync_state(true);
}

void PicoClawView::conversation_changed(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view) return;
    const auto index = lv_dropdown_get_selected(lv_event_get_target_obj(event));
    if (view->view_model_.select_conversation(index)) {
        view->hide_keyboard();
        view->visible_message_count_ = PicoClawModel::maximum_messages;
        if (view->message_input_) lv_textarea_set_text(view->message_input_, "");
    }
    view->sync_state(true);
}

void PicoClawView::earlier_messages_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view) return;
    view->visible_message_count_ += PicoClawModel::maximum_messages;
    view->rebuild_messages();
    lv_obj_scroll_to_y(view->conversation_, 0, LV_ANIM_OFF);
}

void PicoClawView::quick_prompt_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view || !view->message_input_) return;
    auto *button = lv_event_get_target_obj(event);
    auto *label  = button ? lv_obj_get_child(button, 0) : nullptr;
    if (!label) return;
    lv_textarea_set_text(view->message_input_, lv_label_get_text(label));
    view->show_keyboard(view->message_input_, true);
}

void PicoClawView::input_focused(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view) return;
    lv_obj_t *input = lv_event_get_target_obj(event);
    view->show_keyboard(input, input == view->message_input_);
}

void PicoClawView::keyboard_finished(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view) return;
    const bool send = lv_event_get_code(event) == LV_EVENT_READY && view->keyboard_is_chat_;
    view->hide_keyboard();
    if (send) view->send_message();
}

void PicoClawView::chat_model_changed(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view || !view->chat_model_dropdown_) return;
    const auto index = lv_dropdown_get_selected(view->chat_model_dropdown_);
    view->hide_keyboard();
    (void)view->view_model_.select_chat_profile(index);
    view->sync_state();
}

void PicoClawView::chat_model_opened(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view || !view->chat_model_dropdown_) return;
    // LVGL aligns the popup to the selector's left edge. Use the selector's
    // own width so a long provider/model name cannot extend off the display.
    auto *list = lv_dropdown_get_list(view->chat_model_dropdown_);
    lv_obj_set_style_max_width(list, lv_obj_get_width(view->chat_model_dropdown_), 0);
}

void PicoClawView::test_connection_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view) return;
    auto *base = view->model_editor_base_;
    auto *key = view->model_editor_key_;
    if (!base || !key) return;
    view->hide_keyboard();
    if (view->models_feedback_label_) {
        view->models_feedback_visible_ = true;
        lv_obj_clear_flag(view->models_feedback_label_, LV_OBJ_FLAG_HIDDEN);
    }
    view->view_model_.test_connection(lv_textarea_get_text(base), lv_textarea_get_text(key));
    view->sync_state(true);
}

void PicoClawView::model_status_refresh_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view) return;
    const auto index = reinterpret_cast<std::uintptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event))) - 1U;
    if (index >= view->view_model_.profiles().size()) return;
    const auto &profile = view->view_model_.profiles()[index];
    view->view_model_.test_connection(profile.api_base, profile.api_key);
    view->sync_state(true);
}

void PicoClawView::fetch_models_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view) return;
    auto *base = view->model_editor_base_;
    auto *key = view->model_editor_key_;
    if (!base || !key) return;
    view->hide_keyboard();
    if (view->models_feedback_label_) {
        view->models_feedback_visible_ = true;
        lv_obj_clear_flag(view->models_feedback_label_, LV_OBJ_FLAG_HIDDEN);
    }
    view->view_model_.fetch_models(lv_textarea_get_text(base), lv_textarea_get_text(key));
    view->sync_state(true);
}

void PicoClawView::save_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view) return;
    PicoClawSettings settings = view->view_model_.settings();
    if (view->settings_language_dropdown_)
        settings.language = lv_dropdown_get_selected(view->settings_language_dropdown_) == 1U ? "zh" : "en";
    view->hide_keyboard();
    if (view->settings_feedback_label_) lv_obj_clear_flag(view->settings_feedback_label_, LV_OBJ_FLAG_HIDDEN);
    (void)view->view_model_.save_settings(std::move(settings));
    view->sync_state(true);
}

void PicoClawView::settings_language_changed(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (view) view->sync_state(true);
}

void PicoClawView::install_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view) return;
    if (view->settings_feedback_label_) lv_obj_clear_flag(view->settings_feedback_label_, LV_OBJ_FLAG_HIDDEN);
    if (view->view_model_.runtime().update_supported)
        view->view_model_.install_selected_release();
    else
        view->view_model_.install_runtime();
    view->sync_state(true);
}

void PicoClawView::check_updates_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view) return;
    if (view->settings_feedback_label_) lv_obj_clear_flag(view->settings_feedback_label_, LV_OBJ_FLAG_HIDDEN);
    view->view_model_.check_updates();
    view->sync_state(true);
}

void PicoClawView::release_changed(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view || !view->release_dropdown_) return;
    view->view_model_.select_release(lv_dropdown_get_selected(view->release_dropdown_));
    if (view->settings_feedback_label_) lv_obj_clear_flag(view->settings_feedback_label_, LV_OBJ_FLAG_HIDDEN);
    view->sync_state(true);
}

void PicoClawView::gateway_start_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view) return;
    if (view->settings_feedback_label_) lv_obj_clear_flag(view->settings_feedback_label_, LV_OBJ_FLAG_HIDDEN);
    view->view_model_.start_gateway();
    view->sync_state(true);
}

void PicoClawView::gateway_stop_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view) return;
    if (view->settings_feedback_label_) lv_obj_clear_flag(view->settings_feedback_label_, LV_OBJ_FLAG_HIDDEN);
    view->view_model_.stop_gateway();
    view->sync_state(true);
}

void PicoClawView::logs_clicked(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (!view) return;
    if (view->settings_feedback_label_) lv_obj_clear_flag(view->settings_feedback_label_, LV_OBJ_FLAG_HIDDEN);
    view->view_model_.request_logs();
    view->sync_state(true);
}

void PicoClawView::size_changed(lv_event_t *event)
{
    auto *view = static_cast<PicoClawView *>(lv_event_get_user_data(event));
    if (view) {
        view->apply_layout();
        if (view->weixin_qr_) {
            lv_async_call_cancel(layout_weixin_async, view);
            lv_async_call(layout_weixin_async, view);
        }
    }
}

void PicoClawView::layout_weixin_async(void *user_data)
{
    auto *view = static_cast<PicoClawView *>(user_data);
    if (!view->surface_ || !view->weixin_qr_) return;
    view->apply_layout();
    if ((!view->keyboard_ || lv_obj_has_flag(view->keyboard_, LV_OBJ_FLAG_HIDDEN)) &&
        !lv_obj_has_flag(view->weixin_qr_, LV_OBJ_FLAG_HIDDEN)) view->reveal_weixin_qr();
}

void PicoClawView::timer_tick(lv_timer_t *timer)
{
    auto *view = static_cast<PicoClawView *>(lv_timer_get_user_data(timer));
    if (!view) return;
    (void)view->view_model_.poll();
    view->sync_state();
}

void PicoClawView::build_screen()
{
    if (!content_) return;
    // The page subtree is about to be replaced. Detach host shortcuts before
    // deleting the textarea that currently owns them.
    host_text_input_.focus(nullptr);
    lv_async_call_cancel(rebuild_screen_async, this);
    lv_async_call_cancel(layout_weixin_async, this);
    lv_async_call_cancel(rebuild_models_async, this);
    lv_async_call_cancel(rebuild_channels_async, this);
    if (models_swipe_row_) settle_model_swipe(models_swipe_row_, false);
    models_restore_scroll_y_ = -1;
    models_reveal_saved_ = false;
    // Settings owns a header action as well as its content subtree.
    if (save_button_) lv_obj_delete(save_button_);
    if (models_import_button_) lv_obj_delete(models_import_button_);
    if (channels_refresh_) lv_obj_delete(channels_refresh_);
    // The chat toolbar lives in the persistent header rather than the page
    // content, so remove it explicitly when navigating away from Chat.
    if (chat_model_row_) lv_obj_delete(chat_model_row_);
    save_button_ = nullptr;
    models_import_button_ = nullptr;
    channels_refresh_ = nullptr;
    lv_obj_clean(content_);
    clear_screen_pointers();
    if (view_model_.screen() == PicoClawScreen::Chat) {
        build_chat();
    } else if (view_model_.screen() == PicoClawScreen::Models) {
        build_models();
    } else if (view_model_.screen() == PicoClawScreen::Channels) {
        build_channels();
    } else if (view_model_.screen() == PicoClawScreen::Logs) {
        build_logs();
    } else {
        build_settings();
    }
    update_navigation();
    rendered_revision_ = 0;
    apply_layout();
    sync_state(true);
}

void PicoClawView::build_channels()
{
    channels_shell_ = lv_obj_create(content_);
    make_plain(channels_shell_);
    lv_obj_set_name_static(channels_shell_, "picoclaw_channels_page");
    lv_obj_set_size(channels_shell_, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(channels_shell_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(channels_shell_, LV_FLEX_FLOW_COLUMN);
    if (view_model_.channel_draft()) {
        save_button_ = make_button(header_, "picoclaw_channel_save", "Save", components::color_container_background,
                                   components::color_action, channel_save_clicked, this);
        lv_obj_set_size(save_button_, 60, 44);
        lv_obj_set_style_bg_opa(save_button_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_opa(save_button_, LV_OPA_40, LV_STATE_DISABLED);
        (void)components::set_text_font(lv_obj_get_child(save_button_, 0), 17);
    } else {
        channels_refresh_ = components::create_icon_button(header_, LV_SYMBOL_REFRESH, "picoclaw_channels_refresh",
                                                            components::color_container_background,
                                                            channels_refresh_clicked, this);
        lv_obj_set_size(channels_refresh_, 44, 44);
        lv_obj_set_style_text_color(lv_obj_get_child(channels_refresh_, 0), lv_color_hex(components::color_action), 0);
    }
    channels_feedback_ = make_label(channels_shell_, "", 14, components::color_muted_text);
    lv_obj_set_name_static(channels_feedback_, "picoclaw_channel_feedback");
    lv_obj_set_width(channels_feedback_, LV_PCT(100));
    lv_obj_set_style_pad_hor(channels_feedback_, 16, 0);
    lv_obj_set_style_pad_ver(channels_feedback_, 8, 0);
    lv_label_set_long_mode(channels_feedback_, LV_LABEL_LONG_WRAP);

    channels_scroll_ = lv_obj_create(channels_shell_);
    make_plain(channels_scroll_);
    lv_obj_set_name_static(channels_scroll_, "picoclaw_channels_scroll");
    lv_obj_set_width(channels_scroll_, LV_PCT(100));
    lv_obj_set_flex_grow(channels_scroll_, 1);
    lv_obj_add_flag(channels_scroll_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(channels_scroll_, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_set_scroll_dir(channels_scroll_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(channels_scroll_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(channels_scroll_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(channels_scroll_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    channels_list_ = lv_obj_create(channels_scroll_);
    make_plain(channels_list_);
    lv_obj_set_name_static(channels_list_, "picoclaw_channels_list");
    lv_obj_set_size(channels_list_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(channels_list_, view_model_.channel_draft() ? 680 : 760, 0);
    lv_obj_clear_flag(channels_list_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(channels_list_, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(channels_list_, 12, 0);
    lv_obj_set_style_pad_column(channels_list_, 12, 0);
    if (view_model_.channel_draft()) {
        build_channel_editor();
        return;
    }
    auto *intro = make_label(channels_list_, "Connect your chats", 22, components::color_container_title);
    lv_obj_set_width(intro, LV_PCT(100));
    auto *note = make_label(channels_list_, "Choose a platform to configure its bot and who can message it.",
                            17, components::color_muted_text);
    lv_obj_set_width(note, LV_PCT(100));
    channels_summary_ = make_label(channels_list_, "", 14, components::color_muted_text);
    lv_obj_set_width(channels_summary_, LV_PCT(100));
    const auto &channels = view_model_.channels();
    for (std::size_t i = 0; i < channels.size(); ++i) {
        const auto &channel = channels[i];
        channels_rendered_ids_ += channel.id + '\n';
        auto *row = make_card(channels_list_, "picoclaw_channel");
        lv_obj_set_name(row, ("picoclaw_channel_" + channel.id).c_str());
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_style_min_height(row, 102, 0);
        lv_obj_set_style_pad_all(row, 16, 0);
        lv_obj_set_style_pad_column(row, 12, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_color(row, lv_color_hex(components::color_accent_soft), LV_STATE_PRESSED);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, reinterpret_cast<void *>(i + 1U));
        lv_obj_add_event_cb(row, channel_edit_clicked, LV_EVENT_CLICKED, this);
        auto *mark = lv_obj_create(row);
        make_plain(mark);
        lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(mark, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(mark, 44, 44);
        lv_obj_set_style_radius(mark, 12, 0);
        lv_obj_set_style_bg_color(mark, lv_color_hex(components::color_accent_soft), 0);
        lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
        const char *symbol = channel.id == "telegram" ? "\xef\x8b\x86" : channel.id == "discord" ? "\xef\x8e\x92"
            : channel.id == "weixin" ? "\xef\x87\x97" : channel.id == "slack" ? "\xef\x86\x98"
            : channel.id == "whatsapp" ? "\xef\x88\xb2" : channel.id == "qq" ? "\xef\x87\x96"
            : channel.id == "line" ? "\xef\x8f\x80" : "\xef\x87\xa0";
        auto *icon = make_label(mark, symbol, 22, components::color_action);
        lv_obj_center(icon);
        auto *details = lv_obj_create(row);
        make_plain(details);
        lv_obj_clear_flag(details, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(details, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(details, 0, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(details, 1);
        lv_obj_set_flex_flow(details, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(details, 5, 0);
        auto *name = make_label(details, channel.title.c_str(), 17, components::color_text);
        lv_obj_set_width(name, LV_PCT(100));
        auto *state = make_label(details, "", 14, components::color_muted_text);
        lv_obj_set_name(state, ("picoclaw_channel_state_" + channel.id).c_str());
        lv_obj_set_width(state, LV_PCT(100));
        auto *arrow = make_label(row, LV_SYMBOL_RIGHT, 14, components::color_muted_text);
        lv_obj_clear_flag(arrow, LV_OBJ_FLAG_CLICKABLE);
    }
    auto *footer = make_label(channels_list_, "After saving, restart the gateway from Settings to apply changes.",
                              14, components::color_muted_text);
    lv_obj_set_width(footer, LV_PCT(100));
}

void PicoClawView::build_channel_editor()
{
    const auto *draft = view_model_.channel_draft();
    if (!draft) return;
    lv_obj_set_flex_flow(channels_list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(channels_list_, 24, 0);
    auto *connection = make_settings_section(channels_list_, "picoclaw_channel_general", "CHANNEL",
                                             draft->description.c_str());
    auto *row = make_settings_row(connection);
    auto *label = make_settings_label(row, "Enabled");
    lv_obj_set_width(label, 0);
    lv_obj_set_flex_grow(label, 1);
    channel_enabled_ = lv_switch_create(row);
    components::set_focus_outline(channel_enabled_);
    lv_obj_set_name_static(channel_enabled_, "picoclaw_channel_enabled");
    lv_obj_set_size(channel_enabled_, 51, 31);
    lv_obj_set_style_bg_color(channel_enabled_, lv_color_hex(components::color_control_background), LV_PART_MAIN);
    lv_obj_set_style_bg_color(channel_enabled_, lv_color_hex(components::color_success), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (draft->enabled) lv_obj_add_state(channel_enabled_, LV_STATE_CHECKED);
    lv_obj_add_event_cb(channel_enabled_, channel_enabled_changed, LV_EVENT_VALUE_CHANGED, this);
    if (draft->id == "weixin") build_weixin_binding();
    struct Section {
        const char *id;
        const char *title;
        const char *help;
        const char *secret_action;
    };
    // Keep this order close to the Web UI: credentials and callback security
    // come before access and behavior, while transport details stay below.
    static constexpr Section sections[] = {
        {"credentials", "CREDENTIALS", "Bot and application credentials.", "picoclaw_channel_secrets"},
        {"security", "SECURITY", "Signing and encryption values for callbacks.", "picoclaw_channel_security_secrets"},
        {"access", "ACCESS", "Limit which users or groups may reach this channel.", nullptr},
        {"behavior", "BEHAVIOR", "Choose how the channel responds to incoming messages.", nullptr},
        {"connection", "CONNECTION", "API endpoints, proxies and transport addresses.", nullptr},
        {"webhook", "WEBHOOK", "Local callback listener settings.", nullptr},
        {"advanced", "ADVANCED", "Optional runtime-specific settings.", nullptr},
    };
    channel_fields_.assign(draft->fields.size(), nullptr);
    for (const auto &section : sections) {
        std::vector<std::size_t> indexes;
        for (std::size_t i = 0; i < draft->fields.size(); ++i) {
            // iLink supplies these values after the user scans and confirms.
            // Keep them in the model for saving, without exposing credential
            // inputs or internal service endpoints in the binding flow.
            if (draft->id == "weixin" && draft->fields[i].key != "allow_from" && draft->fields[i].key != "proxy")
                continue;
            if (draft->fields[i].section == section.id) indexes.push_back(i);
        }
        if (indexes.empty()) continue;
        const auto group_name = std::string("picoclaw_channel_") + section.id;
        auto *fields = make_settings_section(channels_list_, group_name.c_str(), section.title, section.help);
        bool has_secret = false;
        for (const auto index : indexes) {
            const auto &field = draft->fields[index];
            lv_obj_t *input = nullptr;
            const auto field_label = field.label + (field.required ? " *" : "");
            if (field.kind == PicoClawChannelFieldKind::Boolean) {
                auto *field_row = make_settings_row(fields);
                auto *field_name = make_settings_label(field_row, field_label.c_str());
                lv_obj_set_width(field_name, 0);
                lv_obj_set_flex_grow(field_name, 1);
                input = lv_switch_create(field_row);
                components::set_focus_outline(input);
                lv_obj_set_size(input, 51, 31);
                lv_obj_set_style_bg_color(input, lv_color_hex(components::color_control_background), LV_PART_MAIN);
                lv_obj_set_style_bg_color(input, lv_color_hex(components::color_success), LV_PART_INDICATOR | LV_STATE_CHECKED);
                if (field.value == "true") lv_obj_add_state(input, LV_STATE_CHECKED);
            } else {
                const bool secret = field.kind == PicoClawChannelFieldKind::Secret;
                has_secret |= secret;
                input = make_field(fields, "picoclaw_channel_field", field_label.c_str(), secret, input_focused, this, true);
                lv_textarea_set_max_length(input, std::max<std::size_t>(8192, field.value.size()));
                lv_textarea_set_password_show_time(input, 0);
                lv_textarea_set_text(input, field.value.c_str());
                lv_textarea_set_placeholder_text(input, field.required ? "Required when enabled" : "Optional");
                if (field.kind == PicoClawChannelFieldKind::Integer) lv_textarea_set_accepted_chars(input, "0123456789");
            }
            lv_obj_set_name(input, ("picoclaw_channel_field_" + field.key).c_str());
            lv_obj_set_user_data(input, reinterpret_cast<void *>(index + 1U));
            channel_fields_[index] = input;
            lv_obj_add_event_cb(input, channel_field_changed, LV_EVENT_VALUE_CHANGED, this);
            if (!field.help.empty()) {
                auto *field_row = lv_obj_get_parent(input);
                if (field.kind == PicoClawChannelFieldKind::Boolean) continue;
                auto *help = make_label(field_row, field.help.c_str(), 14, components::color_muted_text);
                lv_obj_set_width(help, LV_PCT(100));
                lv_obj_set_style_pad_bottom(help, 10, 0);
            }
        }
        if (has_secret) {
            const char *action = section.secret_action ? section.secret_action : "picoclaw_channel_secrets";
            make_settings_action(fields, action, "Show credentials", components::color_action,
                                  channel_secrets_clicked, this);
        }
    }
    keyboard_ = lv_keyboard_create(channels_shell_);
    lv_obj_set_name_static(keyboard_, "picoclaw_channels_keyboard");
    lv_obj_set_width(keyboard_, LV_PCT(100));
    lv_obj_set_height(keyboard_, 230);
    lv_obj_set_style_bg_color(keyboard_, lv_color_hex(components::color_control_background), 0);
    lv_obj_set_style_border_width(keyboard_, 0, 0);
    lv_obj_set_style_radius(keyboard_, 0, 0);
    (void)components::set_text_font(keyboard_, 22, LV_PART_ITEMS);
    lv_obj_add_event_cb(keyboard_, keyboard_finished, LV_EVENT_READY, this);
    lv_obj_add_event_cb(keyboard_, keyboard_finished, LV_EVENT_CANCEL, this);
    lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
}

void PicoClawView::build_weixin_binding()
{
    weixin_binding_ = make_settings_section(channels_list_, "picoclaw_weixin_binding", "WEIXIN ACCOUNT",
                                             "Scan with WeChat and confirm on your phone. Save to keep the linked account.");
    auto *body = lv_obj_create(weixin_binding_);
    make_plain(body);
    lv_obj_set_size(body, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_ver(body, 16, 0);
    lv_obj_set_style_pad_row(body, 12, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    weixin_account_ = make_label(body, "No Weixin account linked.", 17, components::color_text);
    lv_obj_set_name_static(weixin_account_, "picoclaw_weixin_account");
    lv_obj_set_width(weixin_account_, LV_PCT(100));
    lv_obj_set_style_text_align(weixin_account_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(weixin_account_, LV_LABEL_LONG_WRAP);
    weixin_status_ = make_label(body, "Bind your account using a QR code.", 14, components::color_muted_text);
    lv_obj_set_name_static(weixin_status_, "picoclaw_weixin_bind_status");
    lv_obj_set_width(weixin_status_, LV_PCT(100));
    lv_obj_set_style_text_align(weixin_status_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(weixin_status_, LV_LABEL_LONG_WRAP);
    weixin_qr_ = lv_qrcode_create(body);
    lv_obj_set_name_static(weixin_qr_, "picoclaw_weixin_qr");
    lv_qrcode_set_size(weixin_qr_, 224);
    lv_qrcode_set_quiet_zone(weixin_qr_, true);
    lv_qrcode_set_dark_color(weixin_qr_, lv_color_hex(components::color_text));
    lv_qrcode_set_light_color(weixin_qr_, lv_color_hex(components::color_container_background));
    lv_obj_add_flag(weixin_qr_, LV_OBJ_FLAG_HIDDEN);
    // Keep scan/confirmation feedback directly below the code so it remains
    // readable when a short display scrolls the account heading out of view.
    lv_obj_move_to_index(weixin_status_, -1);
    auto *actions = lv_obj_create(body);
    make_plain(actions);
    lv_obj_set_size(actions, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(actions, 8, 0);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    weixin_bind_ = make_button(actions, "picoclaw_weixin_bind", "Bind Weixin", components::color_action,
                               components::color_on_action, weixin_bind_clicked, this);
    lv_obj_set_size(weixin_bind_, 0, 44);
    lv_obj_set_style_pad_all(weixin_bind_, 0, 0);
    lv_obj_set_flex_grow(weixin_bind_, 1);
    (void)components::set_text_font(lv_obj_get_child(weixin_bind_, 0), 17);
    weixin_cancel_ = make_button(actions, "picoclaw_weixin_cancel", "Cancel", components::color_control_background,
                                 components::color_text, weixin_cancel_clicked, this);
    lv_obj_set_size(weixin_cancel_, 84, 44);
    lv_obj_set_style_pad_all(weixin_cancel_, 0, 0);
    (void)components::set_text_font(lv_obj_get_child(weixin_cancel_, 0), 17);
    lv_obj_add_flag(weixin_cancel_, LV_OBJ_FLAG_HIDDEN);
}

void PicoClawView::sync_weixin_binding()
{
    if (!weixin_binding_) return;
    const auto *draft = view_model_.channel_draft();
    if (!draft || draft->id != "weixin") return;
    const auto &binding = view_model_.weixin_binding();
    using State = PicoClawWeixinBindingState;
    std::string account;
    bool linked = false;
    for (const auto &field : draft->fields) {
        if (field.key == "account_id") account = field.value;
        if (field.key == "token") linked = !field.value.empty();
    }
    lv_label_set_text(weixin_account_, linked
        ? (account.empty() ? "Weixin account linked." : ("Account: " + account).c_str()) : "No Weixin account linked.");
    const std::string message = binding.message.empty() ? "Bind your account using a QR code." : binding.message;
    lv_label_set_text(weixin_status_, message.c_str());
    lv_obj_set_style_text_color(weixin_status_, lv_color_hex(
        binding.state == State::Failed || binding.state == State::Expired ? components::color_danger
        : binding.state == State::Confirmed ? components::color_success : components::color_muted_text), 0);
    const bool visible = (binding.state == State::Waiting || binding.state == State::Scanned) && !binding.qr_url.empty();
    if (visible) {
        const bool new_code = weixin_rendered_qr_ != binding.qr_url;
        if (new_code) {
            weixin_rendered_qr_ = binding.qr_url;
            if (lv_qrcode_update(weixin_qr_, binding.qr_url.data(), binding.qr_url.size()) != LV_RESULT_OK) {
                weixin_rendered_qr_.clear();
                lv_obj_add_flag(weixin_qr_, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(weixin_status_, "Could not display the QR code. Request a new code.");
            } else {
                lv_obj_clear_flag(weixin_qr_, LV_OBJ_FLAG_HIDDEN);
                reveal_weixin_qr();
            }
        }
    } else {
        lv_obj_add_flag(weixin_qr_, LV_OBJ_FLAG_HIDDEN);
        if (!weixin_rendered_qr_.empty()) {
            lv_canvas_fill_bg(weixin_qr_, lv_color_hex(components::color_container_background), LV_OPA_COVER);
            weixin_rendered_qr_.clear();
        }
    }
    const bool active = binding.active();
    if (active) lv_obj_clear_flag(weixin_cancel_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(weixin_cancel_, LV_OBJ_FLAG_HIDDEN);
    set_disabled(weixin_bind_, view_model_.busy() || binding.state == State::Starting);
    const char *button_text = binding.state == State::Starting ? "Loading..." : active ? "New QR code"
        : binding.state == State::Expired || binding.state == State::Failed ? "Try again"
        : linked ? "Rebind Weixin" : "Bind Weixin";
    lv_label_set_text(lv_obj_get_child(weixin_bind_, 0), button_text);
}

void PicoClawView::reveal_weixin_qr()
{
    lv_obj_update_layout(surface_);
    lv_area_t qr{}, viewport{};
    lv_obj_get_coords(weixin_qr_, &qr);
    lv_obj_get_content_coords(channels_scroll_, &viewport);
    const auto delta = qr.y1 < viewport.y1 ? qr.y1 - viewport.y1 : std::max<lv_coord_t>(0, qr.y2 - viewport.y2);
    if (delta) lv_obj_scroll_to_y(channels_scroll_, lv_obj_get_scroll_y(channels_scroll_) + delta, LV_ANIM_OFF);
}

void PicoClawView::sync_channels()
{
    if (!channels_shell_) return;
    const bool busy = view_model_.busy();
    lv_label_set_text(channels_feedback_, view_model_.channels_status().c_str());
    lv_obj_set_style_text_color(channels_feedback_, lv_color_hex(view_model_.channels_error().empty()
        ? components::color_muted_text : components::color_danger), 0);
    set_disabled(channels_refresh_, busy);
    set_disabled(channel_enabled_, busy);
    for (auto *input : channel_fields_) set_disabled(input, busy);
    sync_weixin_binding();
    if (view_model_.channel_draft()) return;
    std::string ids;
    std::size_t enabled = 0;
    for (const auto &channel : view_model_.channels()) {
        ids += channel.id + '\n';
        if (channel.enabled) ++enabled;
        auto *state = lv_obj_find_by_name(channels_list_, ("picoclaw_channel_state_" + channel.id).c_str());
        if (state) {
            lv_label_set_text(state, !view_model_.channels_loaded() ? "Not loaded" : channel.enabled ? "Enabled" : "Disabled");
            lv_obj_set_style_text_color(state, lv_color_hex(channel.enabled && view_model_.channels_loaded()
                ? components::color_success : components::color_muted_text), 0);
        }
        set_disabled(lv_obj_find_by_name(channels_list_, ("picoclaw_channel_" + channel.id).c_str()),
                      busy || !view_model_.channels_loaded());
    }
    if (channels_summary_) {
        const auto summary = view_model_.channels_loaded()
            ? std::to_string(enabled) + " enabled / " + std::to_string(view_model_.channels().size()) + " channels"
            : "Load configuration to get started";
        lv_label_set_text(channels_summary_, summary.c_str());
    }
    if (ids != channels_rendered_ids_) {
        lv_async_call_cancel(rebuild_channels_async, this);
        lv_async_call(rebuild_channels_async, this);
    }
}

void PicoClawView::build_models()
{
    models_shell_ = lv_obj_create(content_);
    make_plain(models_shell_);
    lv_obj_set_name_static(models_shell_, "picoclaw_models_page");
    lv_obj_set_size(models_shell_, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(models_shell_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(models_shell_, LV_FLEX_FLOW_COLUMN);

    if (models_editing_) {
        save_button_ = make_button(header_, "picoclaw_model_save", "Save", components::color_container_background,
                                   components::color_action, model_save_clicked, this);
        lv_obj_set_size(save_button_, 60, 44);
        lv_obj_set_style_bg_opa(save_button_, LV_OPA_TRANSP, 0);
        (void)components::set_text_font(lv_obj_get_child(save_button_, 0), 17);
    } else {
        models_import_button_ = make_button(header_, "picoclaw_model_import", "Import",
                                            components::color_container_background, components::color_action,
                                            model_import_clicked, this);
        lv_obj_set_size(models_import_button_, 72, 44);
        lv_obj_set_style_bg_opa(models_import_button_, LV_OPA_TRANSP, 0);
        (void)components::set_text_font(lv_obj_get_child(models_import_button_, 0), 17);
    }

    models_feedback_label_ = make_label(models_shell_, "", 14, components::color_muted_text);
    lv_obj_set_name_static(models_feedback_label_, "picoclaw_models_feedback");
    lv_obj_set_width(models_feedback_label_, LV_PCT(100));
    lv_obj_set_style_pad_hor(models_feedback_label_, 16, 0);
    lv_obj_set_style_pad_ver(models_feedback_label_, 8, 0);
    lv_label_bind_text(models_feedback_label_, view_model_.status_subject(), "%s");
    if (!models_feedback_visible_) lv_obj_add_flag(models_feedback_label_, LV_OBJ_FLAG_HIDDEN);

    models_scroll_ = lv_obj_create(models_shell_);
    make_plain(models_scroll_);
    lv_obj_set_name_static(models_scroll_, "picoclaw_models_scroll");
    lv_obj_add_event_cb(models_scroll_, models_scroll_started, LV_EVENT_SCROLL_BEGIN, this);
    lv_obj_set_width(models_scroll_, LV_PCT(100));
    lv_obj_set_flex_grow(models_scroll_, 1);
    lv_obj_add_flag(models_scroll_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(models_scroll_, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_set_scroll_dir(models_scroll_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(models_scroll_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(models_scroll_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(models_scroll_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    models_list_ = lv_obj_create(models_scroll_);
    make_plain(models_list_);
    lv_obj_set_name_static(models_list_, "picoclaw_models_list");
    lv_obj_set_size(models_list_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(models_list_, 760, 0);
    lv_obj_clear_flag(models_list_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_row(models_list_, 12, 0);
    lv_obj_set_style_pad_column(models_list_, 12, 0);
    lv_obj_set_flex_flow(models_list_, LV_FLEX_FLOW_ROW_WRAP);
    models_rendered_count_ = view_model_.profiles().size();
    if (models_editing_) {
        build_model_editor();
        return;
    }

    lv_obj_set_flex_flow(models_list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(models_list_, 24, 0);
    const auto &profiles = view_model_.profiles();
    for (std::size_t provider_index = 0; provider_index < model_provider_count; ++provider_index) {
        auto *group = lv_obj_create(models_list_);
        make_plain(group);
        lv_obj_set_name(group, ("picoclaw_provider_group_" + std::to_string(provider_index)).c_str());
        lv_obj_set_size(group, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(group, 10, 0);
        auto *heading = make_label(group, model_providers[provider_index].name, 22, components::color_container_title);
        lv_obj_set_name(heading, ("picoclaw_provider_title_" + std::to_string(provider_index)).c_str());
        lv_obj_set_width(heading, LV_PCT(100));
        lv_obj_set_style_pad_hor(heading, 4, 0);

        auto *configurations = make_card(group, "picoclaw_provider_configurations");
        lv_obj_set_name(configurations, ("picoclaw_provider_configs_" + std::to_string(provider_index)).c_str());
        lv_obj_set_width(configurations, LV_PCT(100));
        lv_obj_set_style_pad_all(configurations, 0, 0);
        lv_obj_set_style_pad_row(configurations, 8, 0);
        lv_obj_set_style_pad_column(configurations, 12, 0);
        lv_obj_set_style_bg_opa(configurations, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(configurations, 16, 0);
        lv_obj_set_flex_flow(configurations, LV_FLEX_FLOW_ROW_WRAP);
        for (std::size_t index = 0; index < profiles.size(); ++index) {
            const auto &profile = profiles[index];
            if (model_provider_index(profile) != provider_index) continue;
            auto *card = lv_obj_create(configurations);
            make_plain(card);
            lv_obj_set_name(card, ("picoclaw_model_card_" + std::to_string(index)).c_str());
            lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLL_ELASTIC);
            lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLL_MOMENTUM);
            lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
            lv_obj_set_scroll_dir(card, LV_DIR_HOR);
            lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
            lv_obj_set_style_pad_all(card, 0, 0);
            lv_obj_set_style_pad_column(card, 0, 0);
            lv_obj_set_style_radius(card, 16, 0);
            lv_obj_set_style_clip_corner(card, true, 0);
            lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_add_event_cb(card, model_swipe_event, LV_EVENT_SCROLL_BEGIN, this);
            lv_obj_add_event_cb(card, model_swipe_event, LV_EVENT_SCROLL_END, this);

            // A button participates in LVGL's keypad focus group, so a
            // hardware Enter can activate the same edit action as a tap.
            auto *front = lv_button_create(card);
            make_plain(front);
            // Reserve the focus border so focusing cannot resize the swipe
            // contents or move the delete action during a drag.
            lv_obj_set_style_border_width(front, 2, 0);
            lv_obj_set_style_border_opa(front, LV_OPA_TRANSP, 0);
            components::set_focus_outline(front);
            lv_obj_set_name(front, ("picoclaw_model_front_" + std::to_string(index)).c_str());
            lv_obj_set_size(front, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_clear_flag(front, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(front, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(front, reinterpret_cast<void *>(index + 1U));
            lv_obj_add_event_cb(front, model_edit_clicked, LV_EVENT_CLICKED, this);
            lv_obj_add_event_cb(front, model_front_key, LV_EVENT_KEY, this);
            lv_obj_set_style_min_height(front, 88, 0);
            lv_obj_set_style_bg_color(front, lv_color_hex(components::color_container_background), 0);
            lv_obj_set_style_bg_color(front, lv_color_hex(components::color_accent_soft), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(front, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(front, 16, 0);
            lv_obj_set_style_pad_all(front, 10, 0);
            lv_obj_set_style_pad_column(front, 8, 0);
            lv_obj_set_flex_flow(front, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(front, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

            auto *details = lv_obj_create(front);
            make_plain(details);
            lv_obj_set_size(details, 0, LV_SIZE_CONTENT);
            lv_obj_set_flex_grow(details, 1);
            lv_obj_clear_flag(details, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(details, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_flex_flow(details, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(details, 6, 0);
            auto *name = make_label(details, profile.title.c_str(), 17, components::color_container_title);
            lv_obj_set_width(name, LV_PCT(100));
            lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
            auto *model = make_label(details, profile.model_id.c_str(), 14, components::color_muted_text);
            lv_obj_set_width(model, LV_PCT(100));
            lv_obj_set_height(model, 18);
            lv_label_set_long_mode(model, LV_LABEL_LONG_DOT);
            const bool local = provider_index == 4;
            auto *key = make_label(details,
                profile.api_key.empty() ? (local ? "Local model" : "Key not saved") : "Key  ********", 14,
                profile.api_key.empty() && !local ? components::color_muted_text : components::color_success);
            lv_obj_set_width(key, LV_PCT(100));

            auto *status_row = lv_obj_create(details);
            make_plain(status_row);
            lv_obj_set_size(status_row, LV_PCT(100), 20);
            lv_obj_clear_flag(status_row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(status_row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(status_row, 6, 0);
            auto *dot = lv_obj_create(status_row);
            make_plain(dot);
            lv_obj_set_name(dot, ("picoclaw_model_status_dot_" + std::to_string(index)).c_str());
            lv_obj_set_size(dot, 8, 8);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            auto *connection = make_label(status_row, "Not tested", 14, components::color_muted_text);
            lv_obj_set_name(connection, ("picoclaw_model_status_" + std::to_string(index)).c_str());
            lv_obj_set_width(connection, 0);
            lv_obj_set_flex_grow(connection, 1);
            auto *available = make_label(details, "Available: --", 14, components::color_muted_text);
            lv_obj_set_name(available, ("picoclaw_model_available_" + std::to_string(index)).c_str());
            lv_obj_set_width(available, LV_PCT(100));
            lv_label_set_long_mode(available, LV_LABEL_LONG_WRAP);

            auto *refresh = components::create_icon_button(front, LV_SYMBOL_REFRESH, "picoclaw_model_status_refresh",
                                                           components::color_control_background,
                                                           model_status_refresh_clicked, this);
            lv_obj_set_name(refresh, ("picoclaw_model_status_refresh_" + std::to_string(index)).c_str());
            lv_obj_set_size(refresh, 44, 44);
            lv_obj_set_style_text_color(lv_obj_get_child(refresh, 0), lv_color_hex(components::color_action), 0);
            lv_obj_set_user_data(refresh, reinterpret_cast<void *>(index + 1U));
            auto *chevron = make_label(front, LV_SYMBOL_RIGHT, 17, components::color_muted_text);
            lv_obj_clear_flag(chevron, LV_OBJ_FLAG_CLICKABLE);

            auto *remove = make_button(card, "picoclaw_model_swipe_delete", "Delete", components::color_danger,
                                       components::color_on_action, model_swipe_delete_clicked, this);
            lv_obj_set_name(remove, ("picoclaw_model_swipe_delete_" + std::to_string(index)).c_str());
            lv_obj_set_size(remove, model_delete_width, LV_PCT(100));
            lv_obj_set_style_radius(remove, 16, 0);
            lv_obj_set_user_data(remove, reinterpret_cast<void *>(index + 1U));
            (void)components::set_text_font(lv_obj_get_child(remove, 0), 17);
            lv_obj_add_event_cb(remove, model_delete_key, LV_EVENT_KEY, this);
            set_disabled(remove, true);
        }
        auto *add = make_button(configurations, "picoclaw_provider_add", LV_SYMBOL_PLUS,
                                components::color_accent_soft, components::color_action, model_add_clicked, this);
        lv_obj_set_name(add, ("picoclaw_provider_add_" + std::to_string(provider_index)).c_str());
        // The add action is a tile in the same responsive grid as model cards.
        // Use a card-sized default for empty provider groups; apply_layout copies
        // the rendered card height when one is available.
        lv_obj_set_size(add, LV_PCT(100), 144);
        lv_obj_set_style_radius(add, 16, 0);
        lv_obj_set_user_data(add, reinterpret_cast<void *>(provider_index + 1U));
    }
    update_provider_statuses();
}

void PicoClawView::build_model_editor()
{
    lv_obj_set_flex_flow(models_list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_max_width(models_list_, 680, 0);
    model_editor_options_.clear();
    const auto &provider = model_providers[models_provider_index_];
    auto *connection = make_settings_section(models_list_, "picoclaw_model_connection", "MODEL",
                                             "Test the API connection, then fetch available models.");
    model_editor_provider_ = make_field(connection, "picoclaw_model_editor_provider", "Provider name", false,
                                        input_focused, this);
    lv_textarea_set_placeholder_text(model_editor_provider_, provider.name);
    lv_obj_add_event_cb(model_editor_provider_, model_editor_name_changed, LV_EVENT_VALUE_CHANGED, this);
    model_editor_base_ = make_field(connection, "picoclaw_model_editor_base", "API base", false, input_focused, this, true);
    model_editor_key_ = make_field(connection, "picoclaw_model_editor_key", "API key", true, input_focused, this, true);
    lv_textarea_set_password_show_time(model_editor_key_, 0);
    lv_textarea_set_placeholder_text(model_editor_base_, provider.api_base);
    lv_textarea_set_placeholder_text(model_editor_key_, "Optional for local models");
    auto *key_controls = lv_obj_create(lv_obj_get_parent(model_editor_key_));
    make_plain(key_controls);
    lv_obj_set_size(key_controls, LV_PCT(100), 44);
    lv_obj_clear_flag(key_controls, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(key_controls, 8, 0);
    lv_obj_set_flex_flow(key_controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_parent(model_editor_key_, key_controls);
    lv_obj_set_width(model_editor_key_, 0);
    lv_obj_set_flex_grow(model_editor_key_, 1);
    model_editor_test_ = make_button(key_controls, "picoclaw_model_editor_test", "Test", components::color_accent_soft,
                                     components::color_action, test_connection_clicked, this);
    lv_obj_set_size(model_editor_test_, 72, 44);
    lv_obj_set_style_pad_all(model_editor_test_, 0, 0);
    (void)components::set_text_font(lv_obj_get_child(model_editor_test_, 0), 17);
    make_settings_action(connection, "picoclaw_model_key_toggle", "Show API key", components::color_action,
                         model_key_toggle_clicked, this);

    model_editor_id_ = make_field(connection, "picoclaw_model_editor_id", "Model", false, input_focused, this, true);
    lv_textarea_set_placeholder_text(model_editor_id_, provider.default_model);
    auto *model_row = lv_obj_get_parent(model_editor_id_);
    auto *model_controls = lv_obj_create(model_row);
    make_plain(model_controls);
    lv_obj_set_size(model_controls, LV_PCT(100), 44);
    lv_obj_clear_flag(model_controls, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(model_controls, 8, 0);
    lv_obj_set_flex_flow(model_controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_parent(model_editor_id_, model_controls);
    lv_obj_set_width(model_editor_id_, 0);
    lv_obj_set_flex_grow(model_editor_id_, 1);
    model_editor_dropdown_ = lv_dropdown_create(model_controls);
    components::set_focus_outline(model_editor_dropdown_);
    lv_obj_set_name_static(model_editor_dropdown_, "picoclaw_model_editor_dropdown");
    lv_obj_set_size(model_editor_dropdown_, 44, 44);
    lv_dropdown_set_text(model_editor_dropdown_, "");
    (void)components::set_text_font(model_editor_dropdown_, 17);
    lv_obj_set_style_bg_color(model_editor_dropdown_, lv_color_hex(components::color_container_background), 0);
    lv_obj_set_style_text_color(model_editor_dropdown_, lv_color_hex(components::color_muted_text), 0);
    lv_obj_set_style_border_width(model_editor_dropdown_, 0, 0);
    lv_obj_set_style_radius(model_editor_dropdown_, 6, 0);
    lv_obj_set_style_pad_hor(model_editor_dropdown_, 12, 0);
    lv_obj_set_style_pad_ver(model_editor_dropdown_, 12, 0);
    lv_obj_add_event_cb(model_editor_dropdown_, model_editor_selection_changed, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(model_editor_dropdown_, model_editor_dropdown_opened, LV_EVENT_READY, this);
    auto *model_list = lv_dropdown_get_list(model_editor_dropdown_);
    (void)components::set_text_font(model_list, 17);
    lv_obj_set_style_bg_color(model_list, lv_color_hex(components::color_container_background), 0);
    lv_obj_set_style_text_color(model_list, lv_color_hex(components::color_text), 0);
    lv_obj_set_style_bg_color(model_list, lv_color_hex(components::color_accent_soft), LV_PART_SELECTED);
    lv_obj_set_style_text_color(model_list, lv_color_hex(components::color_action), LV_PART_SELECTED);
    lv_obj_set_style_text_line_space(model_list, 22, 0);
    model_editor_fetch_ = make_button(model_controls, "picoclaw_model_editor_fetch", "Fetch", components::color_accent_soft,
                                      components::color_action, fetch_models_clicked, this);
    lv_obj_set_size(model_editor_fetch_, 72, 44);
    lv_obj_set_style_pad_all(model_editor_fetch_, 0, 0);
    (void)components::set_text_font(lv_obj_get_child(model_editor_fetch_, 0), 17);
    lv_obj_add_event_cb(model_editor_id_, model_editor_input_changed, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(model_editor_base_, model_editor_input_changed, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(model_editor_key_, model_editor_input_changed, LV_EVENT_VALUE_CHANGED, this);
    if (models_adding_) {
        lv_textarea_set_text(model_editor_provider_, provider.name);
        lv_textarea_set_text(model_editor_id_, provider.default_model);
        lv_textarea_set_text(model_editor_base_, provider.api_base);
    }
    if (!models_adding_ && models_edit_index_ < view_model_.profiles().size()) {
        const auto &profile = view_model_.profiles()[models_edit_index_];
        // Retain a previously stored long value intact. Validation can then
        // report it without an input widget silently shortening a saved key.
        lv_textarea_set_max_length(model_editor_provider_, std::max<std::size_t>(255, profile.title.size()));
        lv_textarea_set_max_length(model_editor_id_, std::max<std::size_t>(255, profile.model_id.size()));
        lv_textarea_set_max_length(model_editor_base_, std::max<std::size_t>(255, profile.api_base.size()));
        lv_textarea_set_max_length(model_editor_key_, std::max<std::size_t>(511, profile.api_key.size()));
        lv_textarea_set_text(model_editor_provider_, profile.title.c_str());
        lv_textarea_set_text(model_editor_id_, profile.model_id.c_str());
        lv_textarea_set_text(model_editor_base_, profile.api_base.c_str());
        lv_textarea_set_text(model_editor_key_, profile.api_key.c_str());
    }

    update_model_editor_dropdown();

    keyboard_ = lv_keyboard_create(models_shell_);
    lv_obj_set_name_static(keyboard_, "picoclaw_models_keyboard");
    lv_obj_set_width(keyboard_, LV_PCT(100));
    lv_obj_set_height(keyboard_, 230);
    lv_obj_set_style_bg_color(keyboard_, lv_color_hex(components::color_control_background), 0);
    lv_obj_set_style_border_width(keyboard_, 0, 0);
    lv_obj_set_style_radius(keyboard_, 0, 0);
    (void)components::set_text_font(keyboard_, 22, LV_PART_ITEMS);
    lv_obj_add_event_cb(keyboard_, keyboard_finished, LV_EVENT_READY, this);
    lv_obj_add_event_cb(keyboard_, keyboard_finished, LV_EVENT_CANCEL, this);
    lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
}

void PicoClawView::build_chat()
{
    chat_shell_ = lv_obj_create(content_);
    make_plain(chat_shell_);
    lv_obj_set_name_static(chat_shell_, "picoclaw_chat_page");
    lv_obj_set_height(chat_shell_, LV_PCT(100));
    lv_obj_set_style_pad_all(chat_shell_, 18, 0);
    lv_obj_set_style_pad_row(chat_shell_, 12, 0);
    lv_obj_set_flex_flow(chat_shell_, LV_FLEX_FLOW_COLUMN);
    chat_feedback_ = make_label(chat_shell_, "", 14, components::color_danger);
    lv_obj_set_name_static(chat_feedback_, "picoclaw_chat_feedback");
    lv_obj_set_width(chat_feedback_, LV_PCT(100));
    lv_obj_add_flag(chat_feedback_, LV_OBJ_FLAG_HIDDEN);

    // Chat controls share the title bar so the transcript gets the full body.
    chat_model_row_ = lv_obj_create(header_);
    make_plain(chat_model_row_);
    lv_obj_set_name_static(chat_model_row_, "picoclaw_chat_model_row");
    lv_obj_set_size(chat_model_row_, LV_SIZE_CONTENT, 44);
    lv_obj_clear_flag(chat_model_row_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(chat_model_row_, 4, 0);
    lv_obj_set_flex_flow(chat_model_row_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chat_model_row_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    chat_model_dropdown_ = lv_dropdown_create(chat_model_row_);
    components::set_focus_outline(chat_model_dropdown_);
    lv_obj_set_name_static(chat_model_dropdown_, "picoclaw_chat_model_dropdown");
    lv_obj_set_size(chat_model_dropdown_, 0, 44);
    (void)components::set_text_font(chat_model_dropdown_, 14);
    lv_obj_set_style_text_color(chat_model_dropdown_, lv_color_hex(components::color_action), 0);
    lv_obj_set_style_bg_color(chat_model_dropdown_, lv_color_hex(components::color_container_background), 0);
    lv_obj_set_style_border_color(chat_model_dropdown_, lv_color_hex(components::color_border), 0);
    lv_obj_set_style_radius(chat_model_dropdown_, 12, 0);
    lv_obj_set_style_pad_ver(chat_model_dropdown_, 11, 0);
    lv_obj_add_event_cb(chat_model_dropdown_, chat_model_changed, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(chat_model_dropdown_, chat_model_opened, LV_EVENT_READY, this);
    auto *model_list = lv_dropdown_get_list(chat_model_dropdown_);
    (void)components::set_text_font(model_list, 16);
    lv_obj_set_style_text_color(model_list, lv_color_hex(components::color_text), 0);
    lv_obj_set_style_bg_color(model_list, lv_color_hex(components::color_container_background), 0);
    lv_obj_set_style_bg_color(model_list, lv_color_hex(components::color_accent_soft), LV_PART_SELECTED);
    lv_obj_set_style_text_color(model_list, lv_color_hex(components::color_action), LV_PART_SELECTED);
    lv_obj_set_style_text_line_space(model_list, 24, LV_PART_MAIN);

    conversation_dropdown_ = lv_dropdown_create(chat_model_row_);
    components::set_focus_outline(conversation_dropdown_);
    lv_obj_set_name_static(conversation_dropdown_, "picoclaw_history");
    lv_obj_set_width(conversation_dropdown_, 0);
    lv_obj_set_height(conversation_dropdown_, 44);
    (void)components::set_text_font(conversation_dropdown_, 14);
    lv_obj_set_style_text_color(conversation_dropdown_, lv_color_hex(components::color_text), 0);
    lv_obj_set_style_bg_color(conversation_dropdown_, lv_color_hex(components::color_container_background), 0);
    lv_obj_set_style_border_color(conversation_dropdown_, lv_color_hex(components::color_border), 0);
    lv_obj_add_event_cb(conversation_dropdown_, conversation_changed, LV_EVENT_VALUE_CHANGED, this);
    auto *history_list = lv_dropdown_get_list(conversation_dropdown_);
    (void)components::set_text_font(history_list, 16);
    lv_obj_set_style_text_color(history_list, lv_color_hex(components::color_text), 0);
    lv_obj_set_style_bg_color(history_list, lv_color_hex(components::color_container_background), 0);

    connection_badge_ = lv_obj_create(chat_model_row_);
    make_plain(connection_badge_);
    lv_obj_set_name_static(connection_badge_, "picoclaw_connection_badge");
    lv_obj_set_size(connection_badge_, 82, 28);
    lv_obj_set_style_bg_color(connection_badge_, lv_color_hex(components::color_accent_soft), 0);
    lv_obj_set_style_bg_opa(connection_badge_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(connection_badge_, 14, 0);
    lv_obj_set_style_pad_column(connection_badge_, 4, 0);
    lv_obj_set_flex_flow(connection_badge_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(connection_badge_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    auto *connection_dot = lv_obj_create(connection_badge_);
    make_plain(connection_dot);
    lv_obj_set_name_static(connection_dot, "picoclaw_connection_dot");
    lv_obj_set_size(connection_dot, 6, 6);
    lv_obj_set_style_bg_color(connection_dot, lv_color_hex(components::color_warning), 0);
    lv_obj_set_style_bg_opa(connection_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(connection_dot, 3, 0);
    auto *connection_text = make_label(connection_badge_, "Offline", 12, components::color_warning);
    lv_obj_set_name_static(connection_text, "picoclaw_connection_text");
    lv_obj_center(connection_text);
    // Keep the runtime indicator next to the title before the selectors. This
    // gives the header a stable visual order: status, model, history, new chat.
    lv_obj_move_to_index(connection_badge_, 0);

    new_chat_button_ = components::create_icon_button(chat_model_row_, LV_SYMBOL_PLUS, "picoclaw_new_chat",
                                                      components::color_accent, new_chat_clicked, this);
    lv_obj_set_size(new_chat_button_, 42, 42);
    lv_obj_set_style_radius(new_chat_button_, 21, 0);

    conversation_ = components::create_card(chat_shell_);
    lv_obj_set_name_static(conversation_, "picoclaw_conversation");
    lv_obj_set_width(conversation_, LV_PCT(100));
    lv_obj_set_flex_grow(conversation_, 1);
    lv_obj_set_style_pad_all(conversation_, 0, 0);
    lv_obj_set_style_bg_color(conversation_, lv_color_hex(components::color_page_background), 0);
    lv_obj_set_style_border_width(conversation_, 0, 0);
    lv_obj_set_style_pad_row(conversation_, 4, 0);
    lv_obj_set_flex_flow(conversation_, LV_FLEX_FLOW_COLUMN);
    // Generic cards disable scrolling; the transcript must accept pointer
    // drags as well as the programmatic scroll used when a reply arrives.
    lv_obj_add_flag(conversation_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(conversation_, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_set_scroll_dir(conversation_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(conversation_, LV_SCROLLBAR_MODE_AUTO);

    composer_ = lv_obj_create(chat_shell_);
    make_plain(composer_);
    lv_obj_set_name_static(composer_, "picoclaw_composer");
    lv_obj_set_size(composer_, LV_PCT(100), 60);
    lv_obj_set_style_pad_column(composer_, 10, 0);
    lv_obj_set_flex_flow(composer_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(composer_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    message_input_ = lv_textarea_create(composer_);
    components::set_focus_outline(message_input_);
    lv_obj_set_name_static(message_input_, "picoclaw_message_input");
    lv_obj_set_height(message_input_, 58);
    lv_obj_set_width(message_input_, 0);
    lv_obj_set_flex_grow(message_input_, 1);
    lv_textarea_set_one_line(message_input_, true);
    lv_textarea_set_max_length(message_input_, PicoClawModel::maximum_prompt_length);
    lv_textarea_set_placeholder_text(message_input_, "Message");
    (void)components::set_text_font(message_input_, 22);
    lv_obj_set_style_text_color(message_input_, lv_color_hex(components::color_text), 0);
    lv_obj_set_style_bg_color(message_input_, lv_color_hex(components::color_container_background), 0);
    lv_obj_set_style_border_width(message_input_, 1, 0);
    lv_obj_set_style_border_color(message_input_, lv_color_hex(components::color_border), 0);
    lv_obj_set_style_border_color(message_input_, lv_color_hex(components::color_accent), LV_STATE_FOCUSED);
    // A pill-shaped composer mirrors the Android chat field and leaves a
    // larger, forgiving touch target around the send action.
    lv_obj_set_style_radius(message_input_, 24, 0);
    lv_obj_set_style_pad_hor(message_input_, 20, 0);
    lv_obj_add_event_cb(message_input_, input_focused, LV_EVENT_FOCUSED, this);
    lv_obj_add_event_cb(message_input_, input_focused, LV_EVENT_CLICKED, this);

    send_button_ = components::create_icon_button(composer_, LV_SYMBOL_RIGHT, "picoclaw_send_button",
                                                  components::color_accent, send_clicked, this);
    lv_obj_set_size(send_button_, 58, 58);

    keyboard_ = lv_keyboard_create(chat_shell_);
    lv_obj_set_name_static(keyboard_, "picoclaw_keyboard");
    lv_obj_set_width(keyboard_, LV_PCT(100));
    lv_obj_set_height(keyboard_, 230);
    lv_obj_set_style_bg_color(keyboard_, lv_color_hex(components::color_control_background), 0);
    lv_obj_set_style_border_width(keyboard_, 0, 0);
    lv_obj_set_style_radius(keyboard_, 0, 0);
    (void)components::set_text_font(keyboard_, 22, LV_PART_ITEMS);
    lv_obj_add_event_cb(keyboard_, keyboard_finished, LV_EVENT_READY, this);
    lv_obj_add_event_cb(keyboard_, keyboard_finished, LV_EVENT_CANCEL, this);
    lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);

    rebuild_messages();
}

void PicoClawView::build_logs()
{
    settings_shell_ = lv_obj_create(content_);
    make_plain(settings_shell_);
    lv_obj_set_name_static(settings_shell_, "picoclaw_logs_page");
    lv_obj_set_size(settings_shell_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(settings_shell_, 24, 0);
    lv_obj_set_style_pad_row(settings_shell_, 16, 0);
    lv_obj_set_flex_flow(settings_shell_, LV_FLEX_FLOW_COLUMN);
    logs_button_ = make_button(settings_shell_, "picoclaw_logs_refresh", LV_SYMBOL_REFRESH "  REFRESH",
                               components::color_accent_soft, components::color_accent, logs_clicked, this);
    lv_obj_set_width(logs_button_, 160);
    settings_scroll_ = lv_obj_create(settings_shell_);
    make_plain(settings_scroll_);
    lv_obj_set_name_static(settings_scroll_, "picoclaw_logs_scroll");
    lv_obj_set_width(settings_scroll_, LV_PCT(100));
    lv_obj_set_flex_grow(settings_scroll_, 1);
    lv_obj_set_style_bg_color(settings_scroll_, lv_color_hex(components::color_log_background), 0);
    lv_obj_set_style_bg_opa(settings_scroll_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(settings_scroll_, 20, 0);
    lv_obj_set_style_border_width(settings_scroll_, 1, 0);
    lv_obj_set_style_border_color(settings_scroll_, lv_color_hex(components::color_border), 0);
    lv_obj_set_style_pad_all(settings_scroll_, 16, 0);
    lv_obj_add_flag(settings_scroll_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(settings_scroll_, LV_DIR_VER);
    output_label_ = make_label(settings_scroll_, "No activity yet", 14, components::color_log_text);
    lv_obj_set_name_static(output_label_, "picoclaw_output");
    lv_obj_set_width(output_label_, LV_PCT(100));
    lv_label_set_long_mode(output_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(output_label_, 6, 0);
}

void PicoClawView::build_settings()
{
    settings_shell_ = lv_obj_create(content_);
    make_plain(settings_shell_);
    lv_obj_set_name_static(settings_shell_, "picoclaw_settings_page");
    lv_obj_set_size(settings_shell_, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(settings_shell_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(settings_shell_, LV_FLEX_FLOW_COLUMN);

    // Keep the save action reachable while scrolling the settings groups.
    save_button_ = make_button(header_, "picoclaw_save_settings", "Save", components::color_page_background,
                               components::color_action, save_clicked, this);
    lv_obj_set_size(save_button_, 64, 44);
    lv_obj_set_style_bg_opa(save_button_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(save_button_, LV_OPA_TRANSP, LV_STATE_DISABLED);
    lv_obj_set_style_text_opa(save_button_, LV_OPA_40, LV_STATE_DISABLED);
    (void)components::set_text_font(lv_obj_get_child(save_button_, 0), 17);

    settings_scroll_ = lv_obj_create(settings_shell_);
    make_plain(settings_scroll_);
    lv_obj_set_name_static(settings_scroll_, "picoclaw_settings_scroll");
    lv_obj_set_width(settings_scroll_, LV_PCT(100));
    lv_obj_set_flex_grow(settings_scroll_, 1);
    lv_obj_add_flag(settings_scroll_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(settings_scroll_, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_set_scroll_dir(settings_scroll_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(settings_scroll_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(settings_scroll_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(settings_scroll_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    settings_grid_ = lv_obj_create(settings_scroll_);
    make_plain(settings_grid_);
    lv_obj_set_name_static(settings_grid_, "picoclaw_settings_grid");
    lv_obj_set_size(settings_grid_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(settings_grid_, 680, 0);
    lv_obj_clear_flag(settings_grid_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_row(settings_grid_, 24, 0);
    lv_obj_set_flex_flow(settings_grid_, LV_FLEX_FLOW_COLUMN);

    auto *gateway = make_settings_section(settings_grid_, "picoclaw_gateway_card", "GATEWAY");
    runtime_service_label_ = make_settings_value(gateway, "picoclaw_runtime_service", "Status");
    gateway_start_button_ = make_settings_action(gateway, "picoclaw_gateway_start", "Start gateway",
                                                 components::color_action, gateway_start_clicked, this);
    gateway_stop_button_ = make_settings_action(gateway, "picoclaw_gateway_stop", "Stop gateway",
                                                components::color_danger, gateway_stop_clicked, this);
    gateway_refresh_button_ = make_settings_action(gateway, "picoclaw_gateway_refresh", "Refresh status",
                                                   components::color_action, gateway_refresh_clicked, this);
    auto *logs = make_settings_action(gateway, "picoclaw_settings_logs", "Logs",
                                      components::color_text, settings_logs_clicked, this);
    auto *chevron = make_label(logs, LV_SYMBOL_RIGHT, 14, components::color_muted_text);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, 0, 0);

    auto *status           = make_settings_section(settings_grid_, "picoclaw_runtime_status_card", "STATUS");
    runtime_state_label_   = make_settings_value(status, "picoclaw_runtime_state", "Runtime");
    runtime_version_label_ = make_settings_value(status, "picoclaw_runtime_version", "Version");
    runtime_network_label_ = make_settings_value(status, "picoclaw_runtime_network", "Network");
    runtime_config_label_  = make_settings_value(status, "picoclaw_runtime_config", "Configuration");
    runtime_key_label_     = make_settings_value(status, "picoclaw_runtime_key", "API key");
    runtime_gateway_label_ = make_settings_value(status, "picoclaw_runtime_gateway", "Gateway address", true);

    auto *general               = make_settings_section(settings_grid_, "picoclaw_general_card", "GENERAL");
    settings_language_dropdown_ = make_settings_dropdown(general, "picoclaw_settings_language", "Language");
    lv_dropdown_set_options(settings_language_dropdown_, "English\n中文");
    lv_obj_add_event_cb(settings_language_dropdown_, settings_language_changed, LV_EVENT_VALUE_CHANGED, this);

    updates_card_ = make_settings_section(settings_grid_, "picoclaw_updates_card", "UPDATES");
    update_current_label_ = make_settings_value(updates_card_, "picoclaw_update_current", "Current version", true);
    update_message_label_ = make_settings_value(updates_card_, "picoclaw_update_message", "Update status", true);
    check_updates_button_ = make_settings_action(updates_card_, "picoclaw_check_updates", "Check for updates",
                                                  components::color_action, check_updates_clicked, this);
    release_dropdown_ = make_settings_dropdown(updates_card_, "picoclaw_release_dropdown", "Release");
    lv_dropdown_set_options(release_dropdown_, "Check for updates first");
    lv_obj_add_event_cb(release_dropdown_, release_changed, LV_EVENT_VALUE_CHANGED, this);
    release_detail_label_ = make_settings_value(updates_card_, "picoclaw_release_detail", "Selected release", true);
    install_button_ = make_settings_action(updates_card_, "picoclaw_install", "Install selected",
                                             components::color_action, install_clicked, this);

    // Keep operation feedback visible above the settings groups. The shared
    // status subject reports validation errors and save results.
    settings_feedback_label_ = make_label(settings_shell_, "", 14, components::color_muted_text);
    lv_obj_move_to_index(settings_feedback_label_, 0);
    lv_obj_add_flag(settings_feedback_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_pad_ver(settings_feedback_label_, 8, 0);
    lv_obj_set_name_static(settings_feedback_label_, "picoclaw_settings_feedback");
    lv_obj_set_width(settings_feedback_label_, LV_PCT(100));
    lv_obj_set_style_pad_hor(settings_feedback_label_, 16, 0);
    lv_label_bind_text(settings_feedback_label_, view_model_.status_subject(), "%s");
    settings_about_label_ = make_label(settings_grid_, "PicoClaw", 14, components::color_muted_text);
    lv_obj_set_width(settings_about_label_, LV_PCT(100));
    lv_obj_set_style_text_align(settings_about_label_, LV_TEXT_ALIGN_CENTER, 0);

    fill_settings_fields();
}

void PicoClawView::clear_screen_pointers() noexcept
{
    weixin_binding_ = nullptr;
    weixin_bind_ = nullptr;
    weixin_cancel_ = nullptr;
    weixin_qr_ = nullptr;
    weixin_status_ = nullptr;
    weixin_account_ = nullptr;
    weixin_rendered_qr_.clear();
    channels_shell_ = nullptr;
    channels_scroll_ = nullptr;
    channels_list_ = nullptr;
    channels_feedback_ = nullptr;
    channels_refresh_ = nullptr;
    channels_summary_ = nullptr;
    channel_enabled_ = nullptr;
    channel_fields_.clear();
    channels_rendered_ids_.clear();
    channel_show_secrets_ = false;
    chat_shell_                 = nullptr;
    chat_feedback_              = nullptr;
    connection_badge_           = nullptr;
    conversation_               = nullptr;
    composer_                   = nullptr;
    message_input_              = nullptr;
    send_button_                = nullptr;
    new_chat_button_            = nullptr;
    conversation_dropdown_      = nullptr;
    chat_model_row_             = nullptr;
    chat_model_dropdown_        = nullptr;
    models_shell_               = nullptr;
    models_scroll_              = nullptr;
    models_list_                = nullptr;
    models_import_button_       = nullptr;
    models_feedback_label_      = nullptr;
    model_editor_id_           = nullptr;
    model_editor_base_         = nullptr;
    model_editor_key_          = nullptr;
    model_editor_provider_     = nullptr;
    model_editor_dropdown_     = nullptr;
    model_editor_test_         = nullptr;
    model_editor_fetch_        = nullptr;
    model_editor_options_.clear();
    models_swipe_row_         = nullptr;
    models_adjusting_swipe_   = false;
    models_swipe_confirm_     = false;
    settings_shell_             = nullptr;
    settings_scroll_            = nullptr;
    settings_grid_              = nullptr;
    save_button_                = nullptr;
    runtime_state_label_        = nullptr;
    runtime_version_label_      = nullptr;
    runtime_network_label_      = nullptr;
    runtime_config_label_       = nullptr;
    runtime_key_label_          = nullptr;
    runtime_service_label_      = nullptr;
    settings_feedback_label_    = nullptr;
    settings_language_dropdown_ = nullptr;
    settings_about_label_       = nullptr;
    runtime_gateway_label_      = nullptr;
    install_button_             = nullptr;
    updates_card_               = nullptr;
    update_current_label_       = nullptr;
    update_message_label_       = nullptr;
    release_dropdown_           = nullptr;
    release_detail_label_       = nullptr;
    check_updates_button_       = nullptr;
    gateway_start_button_       = nullptr;
    gateway_stop_button_        = nullptr;
    gateway_refresh_button_     = nullptr;
    logs_button_                = nullptr;
    output_label_               = nullptr;
    keyboard_                   = nullptr;
    keyboard_is_chat_           = false;
}

void PicoClawView::rebuild_messages()
{
    if (!conversation_) return;
    lv_obj_clean(conversation_);
    const auto &messages = view_model_.messages();
    if (messages.empty()) {
        auto *empty = lv_obj_create(conversation_);
        make_plain(empty);
        lv_obj_set_name_static(empty, "picoclaw_empty_state");
        lv_obj_set_size(empty, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_pad_all(empty, 20, 0);
        lv_obj_set_style_pad_row(empty, 10, 0);
        lv_obj_set_flex_flow(empty, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(empty, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        auto *mark = make_label(empty, "PicoClaw", 36, components::color_container_title);
        lv_obj_set_width(mark, LV_PCT(100));
        lv_obj_set_style_text_align(mark, LV_TEXT_ALIGN_CENTER, 0);
        auto *hint = make_label(empty, "Start a private AI conversation", 22, components::color_muted_text);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

        auto *quick_actions = lv_obj_create(empty);
        make_plain(quick_actions);
        lv_obj_set_width(quick_actions, LV_PCT(100));
        lv_obj_set_height(quick_actions, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_column(quick_actions, 8, 0);
        lv_obj_set_style_pad_row(quick_actions, 8, 0);
        lv_obj_set_flex_flow(quick_actions, LV_FLEX_FLOW_ROW_WRAP);
        const char *prompts[] = {"Explain this project", "Check device status", "Draft a shell command"};
        for (const auto *prompt : prompts) {
            auto *action = make_button(quick_actions, "picoclaw_quick_prompt", prompt, components::color_accent_soft,
                                       components::color_accent, quick_prompt_clicked, this);
            lv_obj_set_height(action, 44);
            lv_obj_set_flex_grow(action, 1);
        }
        return;
    }

    const std::size_t first = messages.size() > visible_message_count_ ? messages.size() - visible_message_count_ : 0;
    if (first > 0) {
        auto *earlier =
            make_button(conversation_, "picoclaw_earlier_messages", "Earlier messages", components::color_accent_soft,
                        components::color_accent, earlier_messages_clicked, this);
        lv_obj_set_size(earlier, LV_PCT(100), 40);
    }
    for (std::size_t index = first; index < messages.size(); ++index) {
        const auto &message = messages[index];
        const bool sent     = message.role == PicoClawMessageRole::User;
        auto *row           = lv_obj_create(conversation_);
        make_plain(row);
        lv_obj_set_name_static(row, sent ? "picoclaw_user_message" : "picoclaw_assistant_message");
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_ver(row, 5, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, sent ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        // Match picoclaw_fui's chat affordance: each side has a compact
        // identity avatar and the message bubble carries a directional tail.
        if (!sent) {
            auto *avatar = lv_obj_create(row);
            make_plain(avatar);
            lv_obj_set_name_static(avatar, "picoclaw_assistant_avatar");
            lv_obj_set_size(avatar, 32, 32);
            lv_obj_set_style_bg_color(avatar, lv_color_hex(components::color_accent_soft), 0);
            lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(avatar, 16, 0);
            auto *avatar_label = make_label(avatar, "AI", 12, components::color_accent);
            lv_obj_center(avatar_label);
            auto *spacer = lv_obj_create(row);
            make_plain(spacer);
            lv_obj_set_size(spacer, 8, 1);
        }

        auto *bubble = lv_obj_create(row);
        lv_obj_set_width(bubble, LV_PCT(compact_layout_ ? 88 : 74));
        lv_obj_set_height(bubble, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(bubble, 720, 0);
        lv_obj_set_style_bg_color(
            bubble, lv_color_hex(sent ? components::color_user_bubble : components::color_assistant_bubble), 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bubble, sent ? 0 : 1, 0);
        lv_obj_set_style_border_color(bubble, lv_color_hex(components::color_border), 0);
        // LVGL exposes one radius token for a surface; 16px matches the
        // rounded iOS message bubble on the small CM0 display.
        lv_obj_set_style_radius(bubble, 16, 0);
        lv_obj_set_style_pad_left(bubble, 16, 0);
        lv_obj_set_style_pad_right(bubble, 16, 0);
        lv_obj_set_style_pad_top(bubble, 10, 0);
        lv_obj_set_style_pad_bottom(bubble, 10, 0);
        lv_obj_set_style_pad_row(bubble, 6, 0);
        lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);

        auto *text = make_label(bubble, message.text.c_str(), 22,
                                sent ? components::color_on_user_bubble : components::color_text);
        lv_obj_set_width(text, LV_PCT(100));
        lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_line_space(text, 5, 0);
        auto *footer = make_label(bubble, sent ? "You" : (message.pending ? "Working" : "PicoClaw"), 14,
                                  sent ? components::color_on_user_bubble : components::color_muted_text);
        lv_obj_set_width(footer, LV_PCT(100));

        if (sent) {
            auto *spacer = lv_obj_create(row);
            make_plain(spacer);
            lv_obj_set_size(spacer, 8, 1);
            auto *avatar = lv_obj_create(row);
            make_plain(avatar);
            lv_obj_set_name_static(avatar, "picoclaw_user_avatar");
            lv_obj_set_size(avatar, 32, 32);
            lv_obj_set_style_bg_color(avatar, lv_color_hex(components::color_accent_soft), 0);
            lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(avatar, 16, 0);
            auto *avatar_label = make_label(avatar, "U", 12, components::color_accent);
            lv_obj_center(avatar_label);
        }
    }
    lv_obj_update_layout(conversation_);
    const std::uint32_t count = lv_obj_get_child_count(conversation_);
    if (count != 0U) {
        // A reply can be taller than the viewport. Anchor its end, not its
        // top, and scroll only the conversation rather than its parent shell.
        lv_obj_scroll_to_y(conversation_, lv_obj_get_scroll_y(conversation_) + lv_obj_get_scroll_bottom(conversation_),
                           LV_ANIM_OFF);
    }
    rendered_message_revision_ = view_model_.message_revision();
}

void PicoClawView::fill_settings_fields()
{
    const auto &settings = view_model_.settings();
    if (settings_language_dropdown_)
        lv_dropdown_set_selected(settings_language_dropdown_, settings.language == "zh" ? 1U : 0U);
}

void PicoClawView::update_model_editor_dropdown()
{
    if (!model_editor_dropdown_ || !model_editor_id_ || !model_editor_base_ || !model_editor_key_) return;
    const std::string current = lv_textarea_get_text(model_editor_id_);
    model_editor_options_.clear();
    if (view_model_.fetched_models_match(lv_textarea_get_text(model_editor_base_), lv_textarea_get_text(model_editor_key_))) {
        for (auto model : view_model_.fetched_models()) {
            if (std::find(model_editor_options_.begin(), model_editor_options_.end(), model) == model_editor_options_.end())
                model_editor_options_.push_back(std::move(model));
        }
    }
    if (!current.empty() && std::find(model_editor_options_.begin(), model_editor_options_.end(), current) == model_editor_options_.end())
        model_editor_options_.push_back(current);
    std::string options;
    for (const auto &model : model_editor_options_) {
        if (!options.empty()) options += '\n';
        options += model;
    }
    if (options.empty()) options = "No models";
    if (options != lv_dropdown_get_options(model_editor_dropdown_)) lv_dropdown_set_options(model_editor_dropdown_, options.c_str());
    const auto selected = std::find(model_editor_options_.begin(), model_editor_options_.end(), current);
    lv_dropdown_set_selected(model_editor_dropdown_, selected == model_editor_options_.end() ? 0
        : static_cast<std::size_t>(std::distance(model_editor_options_.begin(), selected)));
    set_disabled(model_editor_dropdown_, view_model_.busy() || model_editor_options_.empty());
}

void PicoClawView::send_message()
{
    if (!message_input_) return;
    const std::string text = lv_textarea_get_text(message_input_);
    hide_keyboard();
    if (view_model_.send(text)) {
        lv_textarea_set_text(message_input_, "");
        rebuild_messages();
    }
    sync_state(true);
}

void PicoClawView::show_keyboard(lv_obj_t *textarea, bool chat_input)
{
    if (!keyboard_ || !textarea) return;
    keyboard_is_chat_ = chat_input;
    lv_keyboard_set_textarea(keyboard_, textarea);
    host_text_input_.focus(textarea);
    lv_obj_clear_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
    apply_layout();
    if (!chat_input) {
        lv_obj_update_layout(surface_);
        lv_obj_scroll_to_view_recursive(textarea, LV_ANIM_OFF);
    }
}

void PicoClawView::hide_keyboard()
{
    if (!keyboard_) return;
    host_text_input_.focus(nullptr);
    lv_keyboard_set_textarea(keyboard_, nullptr);
    lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
    keyboard_is_chat_ = false;
    apply_layout();
}

void PicoClawView::update_provider_statuses()
{
    if (!models_list_ || models_editing_) return;
    const auto &profiles = view_model_.profiles();
    for (std::size_t index = 0; index < profiles.size(); ++index) {
        const auto suffix = std::to_string(index);
        auto *label = lv_obj_find_by_name(models_list_, ("picoclaw_model_status_" + suffix).c_str());
        auto *dot = lv_obj_find_by_name(models_list_, ("picoclaw_model_status_dot_" + suffix).c_str());
        auto *available = lv_obj_find_by_name(models_list_, ("picoclaw_model_available_" + suffix).c_str());
        auto *refresh = lv_obj_find_by_name(models_list_, ("picoclaw_model_status_refresh_" + suffix).c_str());
        if (!label || !dot || !available || !refresh) continue;
        const auto &status = view_model_.provider_status(profiles[index]);
        const char *text = "Not tested";
        auto color = components::color_muted_text;
        switch (status.connection) {
            case PicoClawConnectionState::Unknown: break;
            case PicoClawConnectionState::Checking: text = "Checking..."; color = components::color_warning; break;
            case PicoClawConnectionState::Connected: text = "Connected"; color = components::color_success; break;
            case PicoClawConnectionState::Failed: text = "Connection failed"; color = components::color_danger; break;
        }
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
        lv_label_set_text(available, status.available.c_str());
        set_disabled(refresh, view_model_.busy());
    }
}

void PicoClawView::update_chat_dropdown_text()
{
    const bool narrow = lv_obj_get_content_width(main_column_) < 480;
    if (chat_model_dropdown_) {
        const auto selected = view_model_.chat_profile_index();
        chat_model_display_text_ = narrow ? "Model"
                                         : selected < view_model_.profiles().size()
                                               ? "Model · " + view_model_.chat_profile().title
                                               : "Select a model";
        chat_model_display_text_ = dropdown_caption(chat_model_dropdown_, chat_model_display_text_);
        lv_dropdown_set_text(chat_model_dropdown_, chat_model_display_text_.c_str());
    }
    if (conversation_dropdown_) {
        const auto selected = view_model_.active_conversation_index();
        const auto &conversations = view_model_.conversations();
        const auto text = !narrow && selected < conversations.size()
                              ? "History · " + conversations[selected].title
                              : std::string("History");
        const auto caption = dropdown_caption(conversation_dropdown_, text);
        lv_dropdown_set_text(conversation_dropdown_, caption.c_str());
    }
}

void PicoClawView::sync_state(bool force)
{
    if (!surface_) return;
    const auto revision = view_model_.revision();
    const auto &runtime = view_model_.runtime();
    const bool busy     = view_model_.busy();
    if (!force && rendered_revision_ == revision && rendered_message_revision_ == view_model_.message_revision() &&
        rendered_busy_ == busy) {
        return;
    }

    sync_channels();
    if (chat_feedback_) {
        const bool blocked = !busy && !view_model_.chat_model_ready();
        if (blocked) {
            lv_label_set_text(chat_feedback_, lv_subject_get_string(view_model_.status_subject()));
            lv_obj_clear_flag(chat_feedback_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(chat_feedback_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (connection_badge_) {
        auto *connection_text = lv_obj_find_by_name(connection_badge_, "picoclaw_connection_text");
        auto *connection_dot  = lv_obj_find_by_name(connection_badge_, "picoclaw_connection_dot");
        const bool connected  = runtime.gateway_running && runtime.network_ready;
        if (connection_text) lv_label_set_text(connection_text, connected ? "Connected" : "Offline");
        lv_obj_set_style_bg_color(
            connection_badge_,
            lv_color_hex(connected ? components::color_accent_soft : components::color_control_background), 0);
        if (connection_text) {
            lv_obj_set_style_text_color(
                connection_text, lv_color_hex(connected ? components::color_accent : components::color_warning), 0);
        }
        if (connection_dot) {
            lv_obj_set_style_bg_color(
                connection_dot, lv_color_hex(connected ? components::color_accent : components::color_warning), 0);
        }
    }

    if (runtime_state_label_)
        lv_label_set_text(runtime_state_label_, runtime.installed ? "Installed" : "Not installed");
    if (runtime_version_label_)
        lv_label_set_text(runtime_version_label_, runtime.installed ? runtime.version.c_str() : "—");
    if (update_current_label_)
        lv_label_set_text(update_current_label_, runtime.installed ? runtime.version.c_str() : "Not installed");
    if (update_message_label_)
        lv_label_set_text(update_message_label_, runtime.update_supported ? view_model_.update_message().c_str()
            : "Version selection is available on Linux. Install / update runtime uses the configured runtime source.");
    if (release_dropdown_) {
        std::string options;
        for (const auto &release : view_model_.releases()) {
            if (!options.empty()) options += '\n';
            options += release.tag;
            if (release.channel == "prerelease") options += " (pre-release)";
            if (!release.installable) options += " (unavailable)";
        }
        if (options.empty()) options = runtime.update_supported ? "Check for updates first" : "Configured source";
        if (options != lv_dropdown_get_options(release_dropdown_)) lv_dropdown_set_options(release_dropdown_, options.c_str());
        lv_dropdown_set_selected(release_dropdown_, view_model_.selected_release_index());
    }
    update_model_editor_dropdown();
    const auto *selected_release = view_model_.selected_release();
    if (release_detail_label_) {
        std::string detail = "No release selected.";
        if (!runtime.update_supported) detail = "The configured installer will be used.";
        else if (selected_release) {
            detail = selected_release->name.empty() ? selected_release->tag : selected_release->name;
            detail += "\nChannel: " + selected_release->channel;
            if (!selected_release->published_at.empty()) detail += "\nPublished: " + selected_release->published_at;
            if (!selected_release->installable) detail += "\nNo compatible runtime is available for this device.";
        }
        lv_label_set_text(release_detail_label_, detail.c_str());
    }
    if (install_button_)
        lv_label_set_text(lv_obj_get_child(install_button_, 0), runtime.update_supported ? "Install selected" : "Install / update runtime");
    if (runtime_network_label_)
        lv_label_set_text(runtime_network_label_, runtime.network_ready ? "Connected" : "Offline");
    if (runtime_config_label_)
        lv_label_set_text(runtime_config_label_, runtime.config_ready ? "Configured" : "Not set");
    if (runtime_key_label_) lv_label_set_text(runtime_key_label_, runtime.model_key_ready ? "Configured" : "Not set");
    if (runtime_service_label_) {
        lv_label_set_text(runtime_service_label_, runtime.gateway_running ? "Running" : "Stopped");
        lv_obj_set_style_text_color(
            runtime_service_label_,
            lv_color_hex(runtime.gateway_running ? components::color_success : components::color_muted_text), 0);
    }
    if (runtime_gateway_label_) {
        const auto separator = runtime.url.find("://");
        const auto host      = separator == std::string::npos ? runtime.url : runtime.url.substr(separator + 3U);
        lv_label_set_text(runtime_gateway_label_, ("ws://" + host).c_str());
    }
    if (output_label_) {
        lv_label_set_text(output_label_, runtime.output.empty() ? "No activity yet" : runtime.output.c_str());
    }

    set_disabled(send_button_, busy || !view_model_.chat_model_ready());
    if (chat_model_dropdown_) {
        std::string options;
        for (const auto &profile : view_model_.profiles()) {
            if (!options.empty()) options += '\n';
            options += profile.title + " / ";
            options += model_providers[model_provider_index(profile)].name;
        }
        if (options != lv_dropdown_get_options(chat_model_dropdown_))
            lv_dropdown_set_options(chat_model_dropdown_, options.c_str());
        const auto selected = view_model_.chat_profile_index();
        lv_dropdown_set_selected(chat_model_dropdown_, selected < view_model_.profiles().size() ? selected : 0);
        set_disabled(chat_model_dropdown_, busy);
    }
    if (models_list_ && !models_editing_) {
        if (models_rendered_count_ != view_model_.profiles().size() || (rendered_busy_ && !busy)) queue_models_rebuild();
        update_provider_statuses();
    }
    set_disabled(model_editor_test_, busy);
    set_disabled(model_editor_fetch_, busy);
    set_disabled(model_editor_dropdown_, busy || model_editor_options_.empty());
    if (busy && models_swipe_row_) settle_model_swipe(models_swipe_row_, false);
    set_disabled(models_import_button_, busy);
    set_disabled(new_chat_button_, busy);
    set_disabled(conversation_dropdown_, busy);
    if (conversation_dropdown_) {
        std::string options;
        const auto &conversations = view_model_.conversations();
        for (std::size_t index = 0; index < conversations.size(); ++index) {
            if (!options.empty()) options += '\n';
            options += std::to_string(index + 1) + ". " + conversations[index].title;
        }
        if (options != lv_dropdown_get_options(conversation_dropdown_))
            lv_dropdown_set_options(conversation_dropdown_, options.c_str());
        const auto selected = view_model_.active_conversation_index();
        lv_dropdown_set_selected(conversation_dropdown_, selected);
    }
    update_chat_dropdown_text();
    set_disabled(save_button_, busy || (weixin_binding_ && view_model_.weixin_binding().active()));
    set_disabled(install_button_, busy || (runtime.update_supported && (!selected_release || !selected_release->installable)));
    set_disabled(check_updates_button_, busy || !runtime.update_supported);
    set_disabled(release_dropdown_, busy || !runtime.update_supported || view_model_.releases().empty());
    set_disabled(gateway_start_button_, busy || !runtime.installed || runtime.gateway_running);
    set_disabled(gateway_stop_button_, busy || !runtime.gateway_running);
    set_disabled(logs_button_, busy);
    set_disabled(gateway_refresh_button_, busy);
    if (conversation_ && (force || rendered_message_revision_ != view_model_.message_revision())) rebuild_messages();
    rendered_revision_         = revision;
    rendered_message_revision_ = view_model_.message_revision();
    rendered_busy_             = busy;
    update_navigation();
}

void PicoClawView::update_navigation()
{
    const auto screen    = view_model_.screen();
    const bool chat      = screen == PicoClawScreen::Chat;
    const bool models    = screen == PicoClawScreen::Models;
    const bool channels  = screen == PicoClawScreen::Channels;
    const bool logs      = screen == PicoClawScreen::Logs;
    const bool settings  = screen == PicoClawScreen::Settings;
    if (title_) {
        const char *provider_name = model_editor_provider_ ? lv_textarea_get_text(model_editor_provider_) : "";
        const char *editor_title = *provider_name ? provider_name : model_providers[models_provider_index_].name;
        lv_label_set_text(title_, chat ? (lv_obj_get_content_width(main_column_) < 480 ? "CHAT" : "AI CHAT") :
                         models ? (models_editing_ ? editor_title : "Models") :
                         channels ? (view_model_.channel_draft() ? view_model_.channel_draft()->title.c_str() : "Channels") :
                         logs ? "LOGS" : "Settings");
        lv_label_set_long_mode(title_, chat ? LV_LABEL_LONG_CLIP
                                           : (models && models_editing_) || channels ? LV_LABEL_LONG_DOT
                                                                                     : LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(title_, lv_color_hex(components::color_container_title), 0);
    }
    if (header_back_) {
        if (logs || (models && models_editing_) || (channels && view_model_.channel_draft()))
            lv_obj_clear_flag(header_back_, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(header_back_, LV_OBJ_FLAG_HIDDEN);
    }
    if (header_status_) {
        if (chat || settings || models || channels)
            lv_obj_add_flag(header_status_, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(header_status_, LV_OBJ_FLAG_HIDDEN);
        const bool active = view_model_.runtime().gateway_running;
        auto *label       = lv_obj_find_by_name(header_status_, "picoclaw_header_status_label");
        if (label) {
            lv_label_set_text(label, active ? "ACTIVE" : "IDLE");
            lv_obj_set_style_text_color(
                label, lv_color_hex(active ? components::color_accent : components::color_muted_text), 0);
        }
        lv_obj_set_style_bg_color(
            header_status_, lv_color_hex(active ? components::color_accent_soft : components::color_control_background),
            0);
    }
    components::set_navigation_item_selected(chat_tab_, chat);
    components::set_navigation_item_selected(models_tab_, models);
    components::set_navigation_item_selected(channels_tab_, channels);
    components::set_navigation_item_selected(settings_tab_, settings || logs);
}

void PicoClawView::apply_layout() noexcept
{
    if (applying_layout_ || !surface_ || !header_ || !content_) return;
    applying_layout_          = true;
    const std::int32_t width  = lv_obj_get_content_width(surface_);
    const std::int32_t height = lv_obj_get_content_height(surface_);
    // picoclaw_fui switches its adaptive action bar at 600 logical pixels;
    // use the same breakpoint so the 568px portrait target remains a mobile
    // layout while the 1232px target gets the wider arrangement.
    const bool compact            = width < 600 || height >= width;
    const bool wide               = !compact && width >= 600;
    const bool breakpoint_changed = compact_layout_ != compact;
    if (breakpoint_changed && models_swipe_row_) settle_model_swipe(models_swipe_row_, false);
    const bool typing             = keyboard_ && !lv_obj_has_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
    const bool typing_chat        = typing && keyboard_is_chat_;
    const bool settings           = view_model_.screen() == PicoClawScreen::Settings;
    const bool models             = view_model_.screen() == PicoClawScreen::Models;
    const bool channels           = view_model_.screen() == PicoClawScreen::Channels;
    if (conversation_dropdown_) {
        // On short displays reserve the remaining height for the transcript
        // while keeping the model selector and new-chat action available.
        if (typing_chat && height < 700) lv_obj_add_flag(conversation_dropdown_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(conversation_dropdown_, LV_OBJ_FLAG_HIDDEN);
    }
    compact_layout_ = compact;
    lv_obj_set_flex_flow(surface_, wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_size(main_column_, wide ? 0 : LV_PCT(100), wide ? LV_PCT(100) : 0);
    lv_obj_set_flex_grow(main_column_, 1);

    const bool chat = view_model_.screen() == PicoClawScreen::Chat;
    const std::int32_t main_width = width - (wide ? navigation_rail_width : 0);
    const bool narrow_chat = main_width < 480;
    if (chat && title_) lv_label_set_text(title_, narrow_chat ? "CHAT" : "AI CHAT");
    lv_obj_set_height(header_, settings && !typing ? 80 : chat ? 60 : 64);
    lv_obj_set_style_bg_color(
        header_, lv_color_hex(settings ? components::color_page_background : components::color_container_background),
        0);
    lv_obj_set_style_border_width(header_, settings ? 0 : 1, 0);
    lv_obj_set_style_border_side(header_, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_hor(header_, settings ? 20 : chat ? 8 : 16, 0);
    lv_obj_set_style_pad_column(header_, chat || models || channels ? 8 : 16, 0);
    lv_obj_set_style_pad_ver(header_, 8, 0);
    lv_obj_set_flex_flow(header_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (title_) {
        lv_obj_set_width(title_, 0);
        lv_obj_set_flex_grow(title_, 1);
        (void)components::set_text_font(title_, settings && !typing ? 36 : chat && narrow_chat ? 22 : 24);
        lv_obj_set_height(title_, chat || (models && models_editing_) || channels
                                     ? lv_font_get_line_height(lv_obj_get_style_text_font(title_, LV_PART_MAIN))
                                     : LV_SIZE_CONTENT);
    }

    const std::int32_t available   = std::max<std::int32_t>(260, main_width - (compact ? 20 : 64));
    const std::int32_t shell_width = std::min<std::int32_t>(available, 1040);
    if (chat_shell_) {
        lv_obj_set_width(chat_shell_, shell_width);
        lv_obj_set_style_pad_all(chat_shell_, compact ? 10 : 18, 0);
        lv_obj_set_style_pad_row(chat_shell_, compact ? 8 : 12, 0);
        lv_obj_update_layout(chat_shell_);
    }
    if (chat_model_row_) {
        const auto model_width = narrow_chat ? 72 : std::clamp<std::int32_t>(main_width / 4, 92, compact ? 140 : 220);
        const auto history_width = narrow_chat ? 72 : std::clamp<std::int32_t>(main_width / 4, 92, compact ? 150 : 240);
        lv_obj_set_width(chat_model_row_, LV_SIZE_CONTENT);
        if (chat_model_dropdown_) lv_obj_set_width(chat_model_dropdown_, model_width);
        if (conversation_dropdown_) lv_obj_set_width(conversation_dropdown_, history_width);
        for (auto *dropdown : {chat_model_dropdown_, conversation_dropdown_})
            if (dropdown) lv_obj_set_style_pad_hor(dropdown, narrow_chat ? 6 : 12, 0);
        if (connection_badge_) {
            lv_obj_set_size(connection_badge_, narrow_chat ? 14 : 82, narrow_chat ? 14 : 28);
            auto *label = lv_obj_find_by_name(connection_badge_, "picoclaw_connection_text");
            if (label) {
                if (narrow_chat) lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
                else lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (new_chat_button_) lv_obj_set_size(new_chat_button_, 42, 42);
        lv_obj_update_layout(header_);
        update_chat_dropdown_text();
    }
    const auto constrain_chat_dropdown = [width, height, compact](lv_obj_t *dropdown) {
        if (!dropdown) return;
        auto *list = lv_dropdown_get_list(dropdown);
        lv_area_t dropdown_area{};
        lv_obj_get_coords(dropdown, &dropdown_area);
        // A popup opens at its selector's horizontal position, so cap its
        // width to the remaining screen space on that side.
        const auto right_margin = compact ? 10 : 18;
        const auto popup_width = std::max<std::int32_t>(1, width - dropdown_area.x1 - right_margin);
        lv_obj_set_style_max_width(list, popup_width, 0);
        lv_obj_set_style_max_height(list, std::min<std::int32_t>(300, height / 2), 0);
    };
    constrain_chat_dropdown(chat_model_dropdown_);
    constrain_chat_dropdown(conversation_dropdown_);
    if (keyboard_) lv_obj_set_height(keyboard_, height < 700 ? 190 : 250);
    if (settings_shell_) lv_obj_set_width(settings_shell_, settings ? LV_PCT(100) : shell_width);
    if (models_shell_) lv_obj_set_width(models_shell_, LV_PCT(100));
    if (models_scroll_) lv_obj_set_style_pad_all(models_scroll_, compact ? 16 : 24, 0);
    if (channels_scroll_) lv_obj_set_style_pad_all(channels_scroll_, compact ? 16 : 24, 0);
    if (channels_list_ && !view_model_.channel_draft()) {
        const auto list_width = std::min<std::int32_t>(760, main_width - (compact ? 32 : 48));
        const auto card_width = wide ? (list_width - 12) / 2 : list_width;
        for (const auto &channel : view_model_.channels()) {
            if (auto *card = lv_obj_find_by_name(channels_list_, ("picoclaw_channel_" + channel.id).c_str()))
                lv_obj_set_width(card, card_width);
        }
    }
    if (models_list_ && !models_editing_) {
        // Keep the five provider sections in reading order. Only their
        // configuration rows change to two columns on wide displays.
        const auto list_width = std::min<std::int32_t>(760, main_width - (compact ? 32 : 48));
        const auto card_width = wide ? (list_width - 12) / 2 : list_width;
        for (std::size_t i = 0; i < models_rendered_count_; ++i) {
            const auto name = "picoclaw_model_card_" + std::to_string(i);
            if (auto *card = lv_obj_find_by_name(models_list_, name.c_str())) lv_obj_set_width(card, card_width);
        }
        for (std::size_t provider = 0; provider < model_provider_count; ++provider) {
            const auto name = "picoclaw_provider_add_" + std::to_string(provider);
            if (auto *add = lv_obj_find_by_name(models_list_, name.c_str())) lv_obj_set_width(add, card_width);
        }
        // Resolve card content heights after applying the responsive widths,
        // then give each add tile the same height as a card in its group.
        lv_obj_update_layout(models_list_);
        for (std::size_t provider = 0; provider < model_provider_count; ++provider) {
            const auto add_name = "picoclaw_provider_add_" + std::to_string(provider);
            auto *add = lv_obj_find_by_name(models_list_, add_name.c_str());
            if (!add) continue;
            lv_coord_t tile_height = 144;
            for (std::size_t i = 0; i < view_model_.profiles().size(); ++i) {
                if (model_provider_index(view_model_.profiles()[i]) != provider) continue;
                const auto card_name = "picoclaw_model_card_" + std::to_string(i);
                if (auto *card = lv_obj_find_by_name(models_list_, card_name.c_str())) {
                    const auto card_height = lv_obj_get_height(card);
                    if (card_height > 0) tile_height = card_height;
                }
                break;
            }
            lv_obj_set_height(add, tile_height);
        }
    }
    if (settings_scroll_) lv_obj_set_style_pad_all(settings_scroll_, compact ? 16 : 32, 0);
    if (settings) {
        // Give release names a full line on the narrowest display.
        layout_settings_control(release_dropdown_, main_width < 400);
        if (release_dropdown_)
            lv_obj_set_style_max_width(lv_dropdown_get_list(release_dropdown_), main_width - 32, 0);
    }
    if (models && models_editing_) {
        layout_settings_control(model_editor_provider_, main_width < 400);
    }
    if (bottom_navigation_) {
        // One set of labeled items adapts from an evenly divided iPhone-style
        // tab bar to a side rail without rebuilding the current page.
        lv_obj_set_width(bottom_navigation_, wide ? navigation_rail_width : LV_PCT(100));
        lv_obj_set_height(bottom_navigation_, wide ? LV_PCT(100) : 72);
        lv_obj_set_style_border_side(bottom_navigation_, wide ? LV_BORDER_SIDE_LEFT : LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_flex_flow(bottom_navigation_, wide ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_hor(bottom_navigation_, wide ? 6 : 8, 0);
        lv_obj_set_style_pad_top(bottom_navigation_, wide ? 12 : 5, 0);
        lv_obj_set_style_pad_bottom(bottom_navigation_, wide ? 12 : 6, 0);
        lv_obj_set_style_pad_column(bottom_navigation_, 0, 0);
        lv_obj_set_style_pad_row(bottom_navigation_, wide ? 12 : 0, 0);
        for (auto *button : {bottom_chat_button_, bottom_models_button_, bottom_channels_button_,
                             bottom_settings_button_}) {
            lv_obj_set_size(button, wide ? LV_PCT(100) : 0, navigation_item_height);
            lv_obj_set_flex_grow(button, wide ? 0 : 1);
        }
    }

    if (weixin_qr_ && channels_scroll_) {
        lv_obj_update_layout(surface_);
        const auto qr_size = std::max<lv_coord_t>(80, std::min<lv_coord_t>(224,
            std::min(lv_obj_get_content_width(weixin_binding_), lv_obj_get_content_height(channels_scroll_))));
        const bool resized = lv_obj_get_width(weixin_qr_) != qr_size;
        if (resized) {
            lv_qrcode_set_size(weixin_qr_, qr_size);
            lv_obj_set_size(weixin_qr_, qr_size, qr_size);
            if (!weixin_rendered_qr_.empty())
                lv_qrcode_update(weixin_qr_, weixin_rendered_qr_.data(), weixin_rendered_qr_.size());
        }
        // The entire code, including its quiet zone, must fit inside the
        // scrolling viewport after rotation; screen bounds alone are wider
        // than the area left between the header and bottom navigation.
        if (!typing && !lv_obj_has_flag(weixin_qr_, LV_OBJ_FLAG_HIDDEN) && (resized || breakpoint_changed)) {
            reveal_weixin_qr();
        }
    }

    // Settings groups size themselves from the scroll container's content
    // width, including its insets. They never use the wider outer shell width.
    if (typing && (settings || models || channels)) {
        lv_obj_update_layout(surface_);
        if (auto *input = lv_keyboard_get_textarea(keyboard_)) lv_obj_scroll_to_view_recursive(input, LV_ANIM_OFF);
    }

    if (breakpoint_changed && conversation_) rebuild_messages();
    if (typing_chat && conversation_) {
        // Flex layout changes the viewport when the keyboard opens/resizes.
        // Compute the new scroll range only after those sizes are committed.
        lv_obj_update_layout(surface_);
        lv_obj_scroll_to_y(conversation_, lv_obj_get_scroll_y(conversation_) + lv_obj_get_scroll_bottom(conversation_),
                           LV_ANIM_OFF);
    }
    applying_layout_ = false;
}
