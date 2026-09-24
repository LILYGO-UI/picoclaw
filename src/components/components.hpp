#ifndef LILYGO_UI_TEMPLATE_COMPONENTS_HPP
#define LILYGO_UI_TEMPLATE_COMPONENTS_HPP

#include <lvgl.h>

#include <cstdint>

namespace components {

// Fixed iOS light semantic palette. Keep page code independent from concrete
// colours by using these shared roles rather than defining local colours.
inline constexpr std::uint32_t color_page_background      = 0xF2F2F7;
inline constexpr std::uint32_t color_container_background = 0xFFFFFF;
inline constexpr std::uint32_t color_container_title      = 0x1C1C1E;
inline constexpr std::uint32_t color_text                 = 0x1C1C1E;
inline constexpr std::uint32_t color_border               = 0xD1D1D6;
inline constexpr std::uint32_t color_action               = 0x007AFF;
inline constexpr std::uint32_t color_on_action            = 0xFFFFFF;
inline constexpr std::uint32_t color_accent               = 0x007AFF;
inline constexpr std::uint32_t color_on_accent            = 0xFFFFFF;
inline constexpr std::uint32_t color_accent_soft          = 0xEAF3FF;
inline constexpr std::uint32_t color_information          = 0x007AFF;
inline constexpr std::uint32_t color_muted_text           = 0x636366;
inline constexpr std::uint32_t color_control_background   = 0xEFEFF4;
inline constexpr std::uint32_t color_warning              = 0xFF9500;
inline constexpr std::uint32_t color_danger               = 0xFF3B30;
inline constexpr std::uint32_t color_success              = 0x248A3D;
inline constexpr std::uint32_t color_on_success           = 0xFFFFFF;
inline constexpr std::uint32_t color_assistant_bubble     = 0xFFFFFF;
inline constexpr std::uint32_t color_user_bubble          = 0x007AFF;
inline constexpr std::uint32_t color_on_user_bubble       = 0xFFFFFF;
inline constexpr std::uint32_t color_navigation           = 0xFFFFFF;
inline constexpr std::uint32_t color_log_background       = 0xF7F7FA;
inline constexpr std::uint32_t color_log_text             = 0x1C1C1E;
inline constexpr std::uint32_t color_scrim                = 0x000000;

[[nodiscard]] const lv_font_t *font(std::uint32_t size) noexcept;
[[nodiscard]] bool set_text_font(lv_obj_t *object, std::uint32_t size, lv_style_selector_t selector = 0) noexcept;

lv_obj_t *create_page(lv_obj_t *parent);
lv_obj_t *create_header(lv_obj_t *parent, const char *title);
lv_obj_t *create_card(lv_obj_t *parent);
lv_obj_t *create_button(lv_obj_t *parent, const char *text, lv_event_cb_t callback, void *user_data);
lv_obj_t *create_icon_button(lv_obj_t *parent, const char *symbol, const char *accessible_name,
                             std::uint32_t background, lv_event_cb_t callback, void *user_data);

enum class NavigationIcon { Chat, Models, Channels, Settings };

// Parent owns the item and its children. The callback's user_data must outlive
// the item; selection is controlled by the caller and stored in LV_STATE_CHECKED.
lv_obj_t *create_navigation_item(lv_obj_t *parent, NavigationIcon icon, const char *name,
                                 const char *text, lv_event_cb_t callback, void *user_data);
void set_navigation_item_selected(lv_obj_t *item, bool selected) noexcept;

}  // namespace components

#endif
