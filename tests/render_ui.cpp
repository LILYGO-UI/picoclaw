#include "test_environment.hpp"
#include "app.hpp"
#include "pages/picoclaw/picoclaw_model.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr int kPartialRows = 80;
// The app uses one iOS light palette. Keep the render contract anchored to
// semantic colors so a dark/legacy theme cannot silently return.
constexpr std::uint32_t kColorPageBackground      = 0xF2F2F7;
constexpr std::uint32_t kColorContainerBackground = 0xFFFFFF;

std::vector<lv_color32_t> framebuffer;
int display_width = 0;
lv_point_t pointer_position{};
bool pointer_pressed = false;

void read_pointer(lv_indev_t *, lv_indev_data_t *data)
{
    data->point = pointer_position;
    data->state = pointer_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void drag_to_earlier_messages(lv_indev_t *pointer, lv_obj_t *conversation)
{
    lv_obj_update_layout(conversation);
    if (lv_obj_get_scroll_y(conversation) <= 40) return;  // All messages already fit.
    const auto before = lv_obj_get_scroll_y(conversation);
    lv_area_t area{};
    lv_obj_get_content_coords(conversation, &area);
    pointer_position = {area.x1 + (area.x2 - area.x1) / 2, area.y1 + 10};
    pointer_pressed  = true;
    lv_indev_read(pointer);
    for (int step = 1; step <= 8; ++step) {
        pointer_position.y = area.y1 + 10 + step * (area.y2 - area.y1 - 20) / 8;
        lv_tick_inc(16);
        lv_indev_read(pointer);
    }
    pointer_pressed = false;
    lv_indev_read(pointer);
    assert(lv_obj_get_scroll_y(conversation) < before - 20);
    const auto after = lv_obj_get_scroll_y(conversation);
    lv_tick_inc(200);
    lv_timer_handler();
    assert(lv_obj_get_scroll_y(conversation) <= after + 2);
}

void flush_display(lv_display_t *display, const lv_area_t *area, std::uint8_t *pixels)
{
    const int width    = area->x2 - area->x1 + 1;
    const auto *source = reinterpret_cast<const lv_color32_t *>(pixels);
    for (int y = area->y1; y <= area->y2; ++y) {
        const auto source_offset = static_cast<std::size_t>(y - area->y1) * static_cast<std::size_t>(width);
        const auto destination_offset =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(display_width) + static_cast<std::size_t>(area->x1);
        std::copy_n(source + source_offset, width, framebuffer.begin() + destination_offset);
    }
    lv_display_flush_ready(display);
}

bool write_ppm(const char *path, int width, int height)
{
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file << "P6\n" << width << ' ' << height << "\n255\n";
    for (const auto &pixel : framebuffer) {
        file.put(static_cast<char>(pixel.red));
        file.put(static_cast<char>(pixel.green));
        file.put(static_cast<char>(pixel.blue));
    }
    return file.good();
}

bool colors_differ(const lv_color32_t &left, const lv_color32_t &right)
{
    return left.red != right.red || left.green != right.green || left.blue != right.blue || left.alpha != right.alpha;
}

void advance_ui()
{
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        lv_tick_inc(20);
        (void)lv_timer_handler();
    }
}

void advance_binding()
{
    // Binding polls are deliberately rate limited to one request per second;
    // advance several LVGL batches so the helper's start and poll workers can
    // complete without weakening that production interval.
    for (int attempt = 0; attempt < 11; ++attempt) advance_ui();
}

void assert_horizontal_settings_layout(lv_obj_t *object, const lv_area_t &viewport)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return;
    // Text inside an editable field is allowed to scroll. Its containing
    // control, and each list/group container, must fit the settings viewport.
    if (lv_obj_check_type(object, &lv_label_class)) return;
    lv_area_t area{};
    lv_obj_get_coords(object, &area);
    assert(area.x1 >= viewport.x1 && area.x2 <= viewport.x2);
    if (lv_obj_check_type(object, &lv_textarea_class) || lv_obj_check_type(object, &lv_dropdown_class)) return;
    // A swipe row clips a front panel and an offscreen delete action. Its
    // outer viewport must fit, while its intentionally scrollable contents
    // follow the same rule as text inside an editable field.
    const char *name = lv_obj_get_name(object);
    if (name && std::string(name).rfind("picoclaw_model_card_", 0) == 0) return;
    for (std::uint32_t index = 0; index < lv_obj_get_child_count(object); ++index)
        assert_horizontal_settings_layout(lv_obj_get_child(object, index), viewport);
}

void assert_on_screen(lv_obj_t *object, int width, int height)
{
    assert(!lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN));
    lv_area_t area{};
    lv_obj_get_coords(object, &area);
    assert(area.x1 >= 0 && area.x2 < width);
    assert(area.y1 >= 0 && area.y2 < height);
}

bool is_focus_color(const lv_color32_t &pixel)
{
    return pixel.blue > pixel.red + 50 && pixel.blue > pixel.green + 20;
}

void assert_focus_border_visible(lv_obj_t *object)
{
    lv_area_t area{};
    lv_obj_get_coords(object, &area);
    const int center_x  = (area.x1 + area.x2) / 2;
    const int center_y  = (area.y1 + area.y2) / 2;
    const auto pixel_at = [](int x, int y) -> const lv_color32_t & {
        assert(x >= 0 && x < display_width && y >= 0 && y < static_cast<int>(framebuffer.size() / display_width));
        return framebuffer[static_cast<std::size_t>(y) * static_cast<std::size_t>(display_width) +
                           static_cast<std::size_t>(x)];
    };
    const auto edge_has_focus_color = [&](int x1, int y1, int x2, int y2) {
        for (int y = y1; y <= y2; ++y)
            for (int x = x1; x <= x2; ++x)
                if (is_focus_color(pixel_at(x, y))) return true;
        return false;
    };

    // Check the center of each side, away from rounded corners. The focused
    // border should be painted within the control's bounds on all four sides.
    assert(edge_has_focus_color(center_x - 3, area.y1, center_x + 3, area.y1 + 3));
    assert(edge_has_focus_color(center_x - 3, area.y2 - 3, center_x + 3, area.y2));
    assert(edge_has_focus_color(area.x1, center_y - 3, area.x1 + 3, center_y + 3));
    assert(edge_has_focus_color(area.x2 - 3, center_y - 3, area.x2, center_y + 3));
}

lv_obj_t *required_object(lv_obj_t *root, const char *name)
{
    auto *object = lv_obj_find_by_name(root, name);
    assert(object);
    return object;
}

void click_and_advance(lv_obj_t *root, const char *name)
{
    lv_obj_send_event(required_object(root, name), LV_EVENT_CLICKED, nullptr);
    advance_ui();
    lv_obj_update_layout(root);
}

bool labels_contain(lv_obj_t *object, const std::string &text)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return false;
    if (lv_obj_check_type(object, &lv_label_class) &&
        std::string(lv_label_get_text(object)).find(text) != std::string::npos)
        return true;
    for (std::uint32_t index = 0; index < lv_obj_get_child_count(object); ++index)
        if (labels_contain(lv_obj_get_child(object, index), text)) return true;
    return false;
}

lv_obj_t *find_label_containing(lv_obj_t *object, const std::string &text)
{
    if (lv_obj_check_type(object, &lv_label_class) &&
        std::string(lv_label_get_text(object)).find(text) != std::string::npos)
        return object;
    for (std::uint32_t index = 0; index < lv_obj_get_child_count(object); ++index)
        if (auto *label = find_label_containing(lv_obj_get_child(object, index), text)) return label;
    return nullptr;
}

std::size_t model_card_count(lv_obj_t *root)
{
    std::size_t count = 0;
    while (lv_obj_find_by_name(root, ("picoclaw_model_card_" + std::to_string(count)).c_str())) ++count;
    return count;
}

void assert_simplified_model_editor(lv_obj_t *root)
{
    auto *page = required_object(root, "picoclaw_models_page");
    auto *provider = required_object(root, "picoclaw_model_editor_provider");
    auto *model = required_object(root, "picoclaw_model_editor_id");
    auto *choices = required_object(root, "picoclaw_model_editor_dropdown");
    auto *fetch = required_object(root, "picoclaw_model_editor_fetch");
    assert(lv_obj_check_type(provider, &lv_textarea_class));
    assert(labels_contain(page, "Provider name"));
    assert(lv_obj_check_type(model, &lv_textarea_class));
    assert(!lv_obj_has_flag(model, LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(lv_obj_get_parent(model), LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_parent(model) == lv_obj_get_parent(choices));
    assert(lv_obj_get_parent(model) == lv_obj_get_parent(fetch));
    assert(std::string(lv_dropdown_get_text(choices)).empty());
    assert(lv_obj_get_height(model) >= 44 && lv_obj_get_height(choices) >= 44);
    assert(lv_obj_get_width(choices) == 44);
    for (const char *removed : {"picoclaw_model_editor_name", "picoclaw_model_editor_alias",
                                "picoclaw_model_details", "picoclaw_model_actions", "picoclaw_model_delete"})
        assert(!lv_obj_find_by_name(root, removed));
    for (const char *removed : {"DETAILS", "MANAGE", "Alias", "Delete model", "Model ID", "Custom model"})
        assert(!labels_contain(page, removed));
}

void assert_editor_title(lv_obj_t *root, const std::string &expected)
{
    auto *title = required_object(root, "picoclaw_title");
    const std::string displayed = lv_label_get_text(title);
    if (displayed == expected) return;
    // LVGL writes the ellipsis into its label buffer after layout. The
    // visible prefix must still belong to the current provider name.
    assert(lv_label_get_long_mode(title) == LV_LABEL_LONG_DOT);
    assert(displayed.size() > 3 && displayed.substr(displayed.size() - 3) == "...");
    assert(expected.rfind(displayed.substr(0, displayed.size() - 3), 0) == 0);
}

void capture_page(lv_display_t *display, const char *base_path, const char *suffix, int width, int height)
{
    lv_tick_inc(20);
    lv_refr_now(display);
    std::string path = base_path;
    const auto extension = path.rfind(".ppm");
    path.insert(extension == std::string::npos ? path.size() : extension, suffix);
    assert(write_ppm(path.c_str(), width, height));
}

void drag_pointer(lv_indev_t *pointer, lv_point_t from, lv_point_t to)
{
    pointer_position = from;
    pointer_pressed = false;
    lv_indev_read(pointer);
    pointer_pressed = true;
    lv_tick_inc(16);
    lv_indev_read(pointer);
    for (int step = 1; step <= 8; ++step) {
        pointer_position = {from.x + (to.x - from.x) * step / 8,
                            from.y + (to.y - from.y) * step / 8};
        lv_tick_inc(16);
        lv_indev_read(pointer);
    }
    pointer_pressed = false;
    lv_tick_inc(16);
    lv_indev_read(pointer);
    advance_ui();
}

void tap_pointer(lv_indev_t *pointer, lv_obj_t *object)
{
    lv_area_t area{};
    lv_obj_get_coords(object, &area);
    pointer_position = {(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    pointer_pressed = false;
    lv_indev_read(pointer);
    pointer_pressed = true;
    lv_tick_inc(20);
    lv_indev_read(pointer);
    pointer_pressed = false;
    lv_tick_inc(20);
    lv_indev_read(pointer);
    advance_ui();
}

void press_hardware_key(lv_obj_t *object, uint32_t key)
{
    assert(lv_obj_is_group_def(object));
    auto *group = lv_group_create();
    assert(group);
    lv_group_add_obj(group, object);
    lv_group_focus_obj(object);

    struct HardwareKeyState {
        lv_indev_state_t state;
        uint32_t key;
    } state{LV_INDEV_STATE_RELEASED, key};
    auto *keyboard = lv_indev_create();
    assert(keyboard);
    lv_indev_set_type(keyboard, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_display(keyboard, lv_obj_get_display(object));
    lv_indev_set_group(keyboard, group);
    lv_indev_set_user_data(keyboard, &state);
    lv_indev_set_read_cb(keyboard, [](lv_indev_t *input, lv_indev_data_t *data) {
        const auto *state = static_cast<const HardwareKeyState *>(lv_indev_get_user_data(input));
        data->key = state->key;
        data->state = state->state;
    });

    state.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(keyboard);
    state.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(keyboard);
    lv_indev_delete(keyboard);
    lv_group_delete(group);
    advance_ui();
}

void press_hardware_enter(lv_obj_t *object)
{
    press_hardware_key(object, LV_KEY_ENTER);
}

void swipe_model_row(lv_indev_t *pointer, lv_obj_t *row, bool left, bool short_drag = false)
{
    lv_area_t area{};
    lv_obj_get_coords(row, &area);
    const auto row_width = area.x2 - area.x1 + 1;
    const auto y = (area.y1 + area.y2) / 2;
    const auto from_x = left ? area.x1 + row_width * 2 / 3 : area.x1 + 24;
    const auto distance = short_drag ? 24 : std::min<lv_coord_t>(144, row_width / 2);
    drag_pointer(pointer, {from_x, y}, {from_x + (left ? -distance : distance), y});
}

void assert_model_swipe_closed(lv_obj_t *root, std::size_t index)
{
    const auto suffix = std::to_string(index);
    auto *row = required_object(root, ("picoclaw_model_card_" + suffix).c_str());
    auto *front = required_object(root, ("picoclaw_model_front_" + suffix).c_str());
    auto *button = required_object(root, ("picoclaw_model_swipe_delete_" + suffix).c_str());
    assert(!lv_obj_find_by_name(root, "picoclaw_model_editor_provider"));
    assert(std::abs(lv_obj_get_scroll_x(row)) <= 1);
    assert(lv_obj_has_state(button, LV_STATE_DISABLED));
    assert(lv_obj_get_style_radius(front, LV_PART_MAIN) == 16);
    assert(lv_obj_get_style_radius(button, LV_PART_MAIN) == 16);
}

void assert_model_swipe_open(lv_obj_t *root, std::size_t index, int width, int height)
{
    const auto suffix = std::to_string(index);
    auto *row = required_object(root, ("picoclaw_model_card_" + suffix).c_str());
    auto *front = required_object(root, ("picoclaw_model_front_" + suffix).c_str());
    auto *button = required_object(root, ("picoclaw_model_swipe_delete_" + suffix).c_str());
    assert(!lv_obj_find_by_name(root, "picoclaw_model_editor_provider"));
    assert(lv_obj_get_scroll_x(row) >= 44);
    assert(!lv_obj_has_state(button, LV_STATE_DISABLED));
    assert_on_screen(button, width, height);
    lv_area_t row_area{}, front_area{}, button_area{};
    lv_obj_get_coords(row, &row_area);
    lv_obj_get_coords(front, &front_area);
    lv_obj_get_coords(button, &button_area);
    assert(front_area.x1 < row_area.x1);
    assert(button_area.x1 >= row_area.x1 && button_area.x2 <= row_area.x2);
    assert(button_area.y1 >= row_area.y1 && button_area.y2 <= row_area.y2);
    assert(lv_obj_get_style_radius(front, LV_PART_MAIN) == lv_obj_get_style_radius(button, LV_PART_MAIN));
}

void exercise_model_swipe_delete(lv_obj_t *root, lv_display_t *display, lv_indev_t *pointer,
                                int width, int height, const char *snapshot_path)
{
    click_and_advance(root, "picoclaw_models_tab");
    const auto initial_count = model_card_count(root);
    click_and_advance(root, "picoclaw_provider_add_0");
    lv_textarea_set_text(required_object(root, "picoclaw_model_editor_provider"), "Swipe deletion test");
    lv_textarea_set_text(required_object(root, "picoclaw_model_editor_id"), "openai/swipe-deletion-test");
    lv_textarea_set_text(required_object(root, "picoclaw_model_editor_key"), "sk-render-swipe-secret-7654");
    click_and_advance(root, "picoclaw_model_save");
    assert(model_card_count(root) == initial_count + 1);
    click_and_advance(root, "picoclaw_chat_tab");
    auto *chat_model = required_object(root, "picoclaw_chat_model_dropdown");
    assert(lv_dropdown_get_option_count(chat_model) == initial_count + 1);
    assert(std::string(lv_dropdown_get_options(chat_model)).find("Swipe deletion test") != std::string::npos);
    click_and_advance(root, "picoclaw_models_tab");

    const auto card_name = "picoclaw_model_card_" + std::to_string(initial_count);
    const auto delete_name = "picoclaw_model_swipe_delete_" + std::to_string(initial_count);
    auto *row = required_object(root, card_name.c_str());
    auto *other_row = required_object(root, "picoclaw_model_card_6");
    auto *scroll = required_object(root, "picoclaw_models_scroll");
    lv_obj_scroll_to_view_recursive(row, LV_ANIM_OFF);
    lv_obj_update_layout(root);
    assert_on_screen(row, width, height);
    assert_on_screen(other_row, width, height);
    assert_model_swipe_closed(root, initial_count);
    assert_model_swipe_closed(root, 6);

    // A drag beyond the input's scroll threshold but below the reveal
    // threshold must settle closed without being mistaken for an edit tap.
    swipe_model_row(pointer, row, true, true);
    assert_model_swipe_closed(root, initial_count);
    assert(model_card_count(root) == initial_count + 1);
    swipe_model_row(pointer, row, true);
    assert_model_swipe_open(root, initial_count, width, height);
    swipe_model_row(pointer, row, false);
    assert_model_swipe_closed(root, initial_count);

    swipe_model_row(pointer, row, true);
    assert_model_swipe_open(root, initial_count, width, height);
    swipe_model_row(pointer, other_row, true);
    assert_model_swipe_open(root, 6, width, height);
    assert_model_swipe_closed(root, initial_count);

    // The surrounding model list still scrolls vertically. A vertical drag
    // closes the revealed action and must never delete either model.
    const auto before_scroll = lv_obj_get_scroll_y(scroll);
    lv_area_t other_area{};
    lv_obj_get_coords(other_row, &other_area);
    const lv_point_t vertical_start{other_area.x1 + 40, (other_area.y1 + other_area.y2) / 2};
    drag_pointer(pointer, vertical_start, {vertical_start.x + 4, vertical_start.y - 80});
    assert(lv_obj_get_scroll_y(scroll) > before_scroll + 20);
    assert_model_swipe_closed(root, initial_count);
    assert_model_swipe_closed(root, 6);
    assert(model_card_count(root) == initial_count + 1);

    lv_obj_scroll_to_view_recursive(row, LV_ANIM_OFF);
    lv_obj_update_layout(root);
    swipe_model_row(pointer, row, true);
    assert_model_swipe_open(root, initial_count, width, height);
    auto *delete_button = required_object(root, delete_name.c_str());
    tap_pointer(pointer, delete_button);
    assert(model_card_count(root) == initial_count + 1);
    delete_button = required_object(root, delete_name.c_str());
    assert(labels_contain(delete_button, "Confirm"));
    assert_model_swipe_open(root, initial_count, width, height);
    assert(!labels_contain(required_object(root, "picoclaw_models_page"), "sk-render-swipe-secret-7654"));
    capture_page(display, snapshot_path, "-model-swipe-delete", width, height);
    tap_pointer(pointer, delete_button);
    lv_obj_update_layout(root);
    assert(model_card_count(root) == initial_count);
    assert(!labels_contain(required_object(root, "picoclaw_models_page"), "Swipe deletion test"));
    click_and_advance(root, "picoclaw_chat_tab");
    chat_model = required_object(root, "picoclaw_chat_model_dropdown");
    assert(lv_dropdown_get_option_count(chat_model) == initial_count);
    assert(std::string(lv_dropdown_get_options(chat_model)).find("Swipe deletion test") == std::string::npos);

    // Hardware keyboards cannot perform a horizontal swipe. Delete/Backspace
    // on a focused model card opens the confirmation action, then Enter
    // activates it just like the touch Delete button.
    click_and_advance(root, "picoclaw_models_tab");
    const auto keyboard_initial_count = model_card_count(root);
    click_and_advance(root, "picoclaw_provider_add_0");
    lv_textarea_set_text(required_object(root, "picoclaw_model_editor_provider"), "Keyboard deletion test");
    lv_textarea_set_text(required_object(root, "picoclaw_model_editor_id"), "openai/keyboard-deletion-test");
    lv_textarea_set_text(required_object(root, "picoclaw_model_editor_key"), "sk-render-keyboard-secret-4321");
    click_and_advance(root, "picoclaw_model_save");
    assert(model_card_count(root) == keyboard_initial_count + 1);
    const auto keyboard_index = keyboard_initial_count;
    auto *keyboard_row = required_object(root, ("picoclaw_model_card_" + std::to_string(keyboard_index)).c_str());
    lv_obj_scroll_to_view_recursive(keyboard_row, LV_ANIM_OFF);
    lv_obj_update_layout(root);
    press_hardware_key(required_object(root, ("picoclaw_model_front_" + std::to_string(keyboard_index)).c_str()),
                       LV_KEY_DEL);
    assert_model_swipe_open(root, keyboard_index, width, height);
    auto *keyboard_delete = required_object(
        root, ("picoclaw_model_swipe_delete_" + std::to_string(keyboard_index)).c_str());
    assert(labels_contain(keyboard_delete, "Confirm"));
    press_hardware_key(keyboard_delete, LV_KEY_LEFT);
    assert_model_swipe_closed(root, keyboard_index);
    press_hardware_key(required_object(root, ("picoclaw_model_front_" + std::to_string(keyboard_index)).c_str()),
                       LV_KEY_DEL);
    keyboard_delete = required_object(
        root, ("picoclaw_model_swipe_delete_" + std::to_string(keyboard_index)).c_str());
    assert(labels_contain(keyboard_delete, "Confirm"));
    press_hardware_enter(keyboard_delete);
    lv_obj_update_layout(root);
    assert(model_card_count(root) == keyboard_initial_count);
    assert(!labels_contain(required_object(root, "picoclaw_models_page"), "Keyboard deletion test"));
}

std::uint32_t dropdown_option_containing(lv_obj_t *dropdown, const std::string &text)
{
    const std::string options = lv_dropdown_get_options(dropdown);
    std::size_t offset = 0;
    for (std::uint32_t index = 0; index < lv_dropdown_get_option_count(dropdown); ++index) {
        const auto end = options.find('\n', offset);
        if (options.substr(offset, end == std::string::npos ? std::string::npos : end - offset).find(text) !=
            std::string::npos)
            return index;
        if (end == std::string::npos) break;
        offset = end + 1;
    }
    assert(false && "Expected saved model is missing from the chat selector");
    return 0;
}

void wait_until_enabled(lv_obj_t *root, const char *name)
{
    for (int attempt = 0; attempt < 200; ++attempt) {
        lv_tick_inc(20);
        (void)lv_timer_handler();
        if (!lv_obj_has_state(required_object(root, name), LV_STATE_DISABLED)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(false && "UI operation did not finish");
}

void exercise_editor_api_controls(lv_obj_t *root, const std::string &api_calls_path, int width, int height)
{
    auto *provider = required_object(root, "picoclaw_model_editor_provider");
    auto *id = required_object(root, "picoclaw_model_editor_id");
    auto *base = required_object(root, "picoclaw_model_editor_base");
    auto *key = required_object(root, "picoclaw_model_editor_key");
    auto *dropdown = required_object(root, "picoclaw_model_editor_dropdown");
    const std::string original_provider = lv_textarea_get_text(provider);
    const std::string original_id = lv_textarea_get_text(id);
    const std::string original_base = lv_textarea_get_text(base);
    const std::string original_key = lv_textarea_get_text(key);
    auto change_field = [&](lv_obj_t *field, const char *value) {
        lv_textarea_set_text(field, value);
        lv_obj_send_event(field, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_update_layout(root);
    };
    auto assert_drafts = [&](const std::string &expected_key) {
        assert(required_object(root, "picoclaw_model_editor_provider") == provider);
        assert(required_object(root, "picoclaw_model_editor_key") == key);
        assert(std::string(lv_textarea_get_text(provider)) == original_provider);
        assert_editor_title(root, original_provider);
        assert(std::string(lv_textarea_get_text(key)) == expected_key);
        assert(lv_textarea_get_password_mode(key));
    };
    auto assert_choices = [&] {
        const std::string options = "\n" + std::string(lv_dropdown_get_options(dropdown)) + "\n";
        const std::string current = lv_textarea_get_text(id);
        assert(options.find("\n" + current + "\n") != std::string::npos);
        assert(options.find("Custom model") == std::string::npos);
        assert(!lv_obj_has_flag(id, LV_OBJ_FLAG_HIDDEN));
        assert(!lv_obj_has_state(dropdown, LV_STATE_DISABLED));
    };
    auto request = [&](const char *button_name) {
        auto *button = required_object(root, button_name);
        assert(!lv_obj_has_state(button, LV_STATE_DISABLED));
        assert(lv_obj_get_width(button) >= 44 && lv_obj_get_height(button) >= 44);
        lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
        assert(lv_obj_has_state(button, LV_STATE_DISABLED));
        advance_ui();
        wait_until_enabled(root, button_name);
        lv_obj_update_layout(root);
    };

    assert_choices();
    request("picoclaw_model_editor_test");
    assert(labels_contain(required_object(root, "picoclaw_models_feedback"), "successful"));
    assert_drafts(original_key);
    change_field(base, "https://models.example.test/failure/v1");
    request("picoclaw_model_editor_test");
    assert(labels_contain(required_object(root, "picoclaw_models_feedback"), "failed"));
    assert_drafts(original_key);

    change_field(base, original_base.c_str());
    request("picoclaw_model_editor_fetch");
    assert_drafts(original_key);
    assert(std::string(lv_textarea_get_text(id)) == original_id);
    assert_choices();
    const std::string fetched_options = "\n" + std::string(lv_dropdown_get_options(dropdown)) + "\n";
    assert(fetched_options.find("\nfetched-alpha\n") != std::string::npos);
    assert(fetched_options.find("\nvendor/fetched-beta\n") != std::string::npos);
    assert(fetched_options.find("openai/fetched-alpha") == std::string::npos);
    assert(fetched_options.find("openai/vendor/fetched-beta") == std::string::npos);
    lv_obj_scroll_to_view_recursive(dropdown, LV_ANIM_OFF);
    lv_obj_update_layout(root);
    lv_dropdown_open(dropdown);
    lv_obj_update_layout(root);
    assert_on_screen(lv_dropdown_get_list(dropdown), width, height);
    lv_dropdown_close(dropdown);
    for (const char *returned_id : {"fetched-alpha", "vendor/fetched-beta"}) {
        lv_dropdown_set_selected(dropdown, dropdown_option_containing(dropdown, returned_id));
        lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_update_layout(root);
        char selected_id[128]{};
        lv_dropdown_get_selected_str(dropdown, selected_id, sizeof(selected_id));
        assert(std::string(selected_id) == returned_id);
        assert(std::string(lv_textarea_get_text(id)) == returned_id);
        assert(!lv_obj_has_flag(id, LV_OBJ_FLAG_HIDDEN));
    }
    assert_drafts(original_key);
    assert_choices();
    // The same field remains directly editable after choosing a fetched model.
    change_field(id, original_id.c_str());
    assert_choices();

    // Fetched choices belong to the draft endpoint and key that produced
    // them. Editing either credential must discard those choices while
    // retaining the currently entered model in the editable field and choices.
    change_field(base, "https://alternate.example.test/v1");
    assert(std::string(lv_dropdown_get_options(dropdown)).find("fetched-alpha") == std::string::npos);
    assert(std::string(lv_dropdown_get_options(dropdown)).find("vendor/fetched-beta") == std::string::npos);
    assert_choices();
    request("picoclaw_model_editor_fetch");
    assert(std::string(lv_dropdown_get_options(dropdown)).find("openai/alternate-alpha") != std::string::npos);
    assert(std::string(lv_textarea_get_text(id)) == original_id);
    assert_drafts(original_key);
    assert_choices();
    lv_dropdown_set_selected(dropdown, dropdown_option_containing(dropdown, "openai/alternate-alpha"));
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_update_layout(root);
    char prefixed_id[128]{};
    lv_dropdown_get_selected_str(dropdown, prefixed_id, sizeof(prefixed_id));
    assert(std::string(prefixed_id) == "openai/alternate-alpha");
    assert(std::string(lv_textarea_get_text(id)) == "openai/alternate-alpha");
    change_field(id, original_id.c_str());
    constexpr const char *updated_key = "sk-render-draft-secret-9999";
    change_field(key, updated_key);
    assert(std::string(lv_dropdown_get_options(dropdown)).find("openai/alternate-alpha") == std::string::npos);
    assert_choices();
    request("picoclaw_model_editor_fetch");
    assert(std::string(lv_dropdown_get_options(dropdown)).find("openai/new-key-model") != std::string::npos);
    assert(std::string(lv_textarea_get_text(id)) == original_id);
    assert(std::string(lv_dropdown_get_options(dropdown)).find("openai/alternate-alpha") == std::string::npos);
    assert_drafts(updated_key);
    assert_choices();

    std::ifstream recorded(api_calls_path);
    const std::string metadata((std::istreambuf_iterator<char>(recorded)), {});
    assert(metadata.find("test-connection base=" + original_base + " auth=primary\n") != std::string::npos);
    assert(metadata.find("test-connection base=https://models.example.test/failure/v1 auth=primary\n") !=
           std::string::npos);
    assert(metadata.find("fetch-models base=" + original_base + " auth=primary\n") != std::string::npos);
    assert(metadata.find("fetch-models base=https://alternate.example.test/v1 auth=primary\n") != std::string::npos);
    assert(metadata.find("fetch-models base=https://alternate.example.test/v1 auth=updated\n") != std::string::npos);
    assert(metadata.find("sk-render") == std::string::npos);

    change_field(base, original_base.c_str());
    change_field(key, original_key.c_str());
    change_field(id, "");
    assert(std::string(lv_dropdown_get_options(dropdown)) == "No models");
    assert(lv_obj_has_state(dropdown, LV_STATE_DISABLED));
    assert(!lv_obj_has_flag(id, LV_OBJ_FLAG_HIDDEN));
    change_field(id, original_id.c_str());
    assert(std::string(lv_dropdown_get_options(dropdown)).find("openai/new-key-model") == std::string::npos);
    assert_drafts(original_key);
    assert_choices();
}

void assert_chat_model_layout(lv_obj_t *root, int width, int height)
{
    auto *row = required_object(root, "picoclaw_chat_model_row");
    auto *dropdown = required_object(root, "picoclaw_chat_model_dropdown");
    auto *history = required_object(root, "picoclaw_history");
    auto *new_chat = required_object(root, "picoclaw_new_chat");
    auto *title = required_object(root, "picoclaw_title");
    auto *connection = required_object(root, "picoclaw_connection_badge");
    assert_on_screen(row, width, height);
    assert_on_screen(dropdown, width, height);
    assert_on_screen(history, width, height);
    assert_on_screen(new_chat, width, height);
    assert_on_screen(connection, width, height);
    assert(lv_obj_get_height(row) >= 44 && lv_obj_get_height(dropdown) >= 44 && lv_obj_get_height(history) >= 44);
    assert(lv_obj_get_width(dropdown) >= 72 && lv_obj_get_width(history) >= 72);
    assert(lv_obj_get_parent(row) == required_object(root, "picoclaw_header"));
    assert(lv_obj_get_parent(connection) == row && lv_obj_get_parent(new_chat) == row);
    assert(!lv_obj_find_by_name(root, "picoclaw_status_card"));
    assert(!lv_obj_find_by_name(root, "picoclaw_status"));
    assert(lv_obj_has_flag(required_object(root, "picoclaw_chat_feedback"), LV_OBJ_FLAG_HIDDEN));
    lv_area_t header_area{}, title_area{}, row_area{}, model_area{}, history_area{}, connection_area{}, new_chat_area{};
    lv_obj_get_content_coords(required_object(root, "picoclaw_header"), &header_area);
    assert_horizontal_settings_layout(row, header_area);
    lv_obj_get_coords(row, &row_area);
    lv_obj_get_coords(title, &title_area);
    lv_obj_get_coords(dropdown, &model_area);
    lv_obj_get_coords(history, &history_area);
    lv_obj_get_coords(connection, &connection_area);
    lv_obj_get_coords(new_chat, &new_chat_area);
    assert(title_area.x2 < row_area.x1);
    assert(model_area.y1 >= row_area.y1 && model_area.y2 <= row_area.y2);
    assert(history_area.y1 >= row_area.y1 && history_area.y2 <= row_area.y2);
    assert(connection_area.y1 >= row_area.y1 && connection_area.y2 <= row_area.y2);
    assert(new_chat_area.y1 >= row_area.y1 && new_chat_area.y2 <= row_area.y2);
    assert(connection_area.x2 < model_area.x1 && model_area.x2 < history_area.x1 &&
           history_area.x2 < new_chat_area.x1);
    assert(!std::string(lv_label_get_text(title)).empty());
    assert(lv_obj_get_height(title) <= lv_font_get_line_height(lv_obj_get_style_text_font(title, LV_PART_MAIN)) + 2);
    lv_area_t transcript_area{}, header_bounds{};
    lv_obj_get_coords(required_object(root, "picoclaw_conversation"), &transcript_area);
    lv_obj_get_coords(required_object(root, "picoclaw_header"), &header_bounds);
    assert(transcript_area.y1 > header_bounds.y2 && transcript_area.y1 <= header_bounds.y2 + 20);
    assert(lv_obj_get_style_text_font(dropdown, LV_PART_MAIN));
}

void assert_chat_focus_borders(lv_obj_t *root, lv_display_t *display)
{
    for (const char *name : {"picoclaw_chat_model_dropdown", "picoclaw_history", "picoclaw_message_input",
                             "picoclaw_send_button"}) {
        auto *object = required_object(root, name);
        for (const auto state : {LV_STATE_FOCUS_KEY, LV_STATE_FOCUSED}) {
            lv_obj_add_state(object, state);
            lv_obj_update_layout(root);
            assert(lv_obj_get_style_outline_width(object, LV_PART_MAIN) == 0);
            assert(lv_obj_get_style_outline_opa(object, LV_PART_MAIN) == LV_OPA_TRANSP);
            assert(lv_obj_get_style_outline_pad(object, LV_PART_MAIN) == 0);
            lv_refr_now(display);
            assert_focus_border_visible(object);
            lv_obj_remove_state(object, state);
        }
        lv_obj_add_state(object, LV_STATE_EDITED);
        lv_obj_update_layout(root);
        assert(lv_obj_get_style_outline_width(object, LV_PART_MAIN) == 0);
        assert(lv_obj_get_style_outline_opa(object, LV_PART_MAIN) == LV_OPA_TRANSP);
        assert(lv_obj_get_style_outline_pad(object, LV_PART_MAIN) == 0);
        lv_obj_remove_state(object, LV_STATE_EDITED);
    }
}

void choose_chat_model(lv_obj_t *root, const char *model_title)
{
    auto *dropdown = required_object(root, "picoclaw_chat_model_dropdown");
    auto *page = required_object(root, "picoclaw_chat_page");
    auto *input = required_object(root, "picoclaw_message_input");
    auto *send = required_object(root, "picoclaw_send_button");
    auto *conversation = required_object(root, "picoclaw_conversation");
    auto *history = required_object(root, "picoclaw_history");
    const auto selected = dropdown_option_containing(dropdown, model_title);
    const auto previous = lv_dropdown_get_selected(dropdown);
    const std::string draft = lv_textarea_get_text(input);
    const auto scroll = lv_obj_get_scroll_y(conversation);
    const auto message_count = lv_obj_get_child_count(conversation);
    const auto history_selection = lv_dropdown_get_selected(history);
    assert(selected != previous);
    assert(!lv_obj_has_state(dropdown, LV_STATE_DISABLED));
    lv_dropdown_set_selected(dropdown, selected);
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
    assert(lv_obj_has_state(dropdown, LV_STATE_DISABLED));
    assert(lv_obj_has_state(send, LV_STATE_DISABLED));
    // The selected label remains truthful until the helper confirms that the
    // saved credentials and model have actually become the active config.
    assert(lv_dropdown_get_selected(dropdown) == previous);
    advance_ui();
    wait_until_enabled(root, "picoclaw_chat_model_dropdown");
    assert(required_object(root, "picoclaw_chat_page") == page);
    assert(required_object(root, "picoclaw_chat_model_dropdown") == dropdown);
    assert(required_object(root, "picoclaw_message_input") == input);
    assert(required_object(root, "picoclaw_conversation") == conversation);
    assert(required_object(root, "picoclaw_history") == history);
    assert(std::string(lv_textarea_get_text(input)) == draft);
    assert(lv_obj_get_scroll_y(conversation) == scroll);
    assert(lv_obj_get_child_count(conversation) == message_count);
    assert(lv_dropdown_get_selected(history) == history_selection);
    assert(lv_dropdown_get_selected(dropdown) == selected);
    assert(!lv_obj_has_state(dropdown, LV_STATE_DISABLED));
    assert(!lv_obj_has_state(send, LV_STATE_DISABLED));
}

void exercise_models_page(lv_obj_t *root, lv_display_t *display, lv_obj_t *surface, lv_indev_t *pointer,
                          int width, int height, const char *snapshot_path, const std::string &api_calls_path)
{
    lv_obj_send_event(required_object(root, "picoclaw_models_tab"), LV_EVENT_CLICKED, nullptr);
    lv_obj_update_layout(root);
    assert(labels_contain(required_object(root, "picoclaw_model_card_5"), "Checking..."));
    assert(!lv_obj_has_state(required_object(root, "picoclaw_model_import"), LV_STATE_DISABLED));
    advance_ui();
    auto *page = required_object(root, "picoclaw_models_page");
    auto *scroll = required_object(root, "picoclaw_models_scroll");
    auto *list = required_object(root, "picoclaw_models_list");
    lv_area_t viewport{};
    lv_obj_get_content_coords(scroll, &viewport);
    assert_horizontal_settings_layout(list, viewport);
    assert(!lv_obj_find_by_name(root, "picoclaw_model_add"));
    auto *header_import = required_object(root, "picoclaw_model_import");
    assert_on_screen(header_import, width, height);
    assert(lv_obj_get_width(header_import) >= 44 && lv_obj_get_height(header_import) >= 44);
    const auto initial_count = model_card_count(root);
    assert(initial_count > 0);
    assert(labels_contain(required_object(root, "picoclaw_model_card_5"), "Connected"));
    assert(labels_contain(required_object(root, "picoclaw_model_card_5"), "Available: Not metered"));
    assert(labels_contain(required_object(root, "picoclaw_model_card_0"), "Not tested"));
    {
        std::ifstream calls(api_calls_path);
        const std::string text((std::istreambuf_iterator<char>(calls)), {});
        assert(text.find("test-connection base=http://127.0.0.1:11434/v1 auth=local\n") != std::string::npos);
        assert(text.find("base=https://api.openai.com/v1") == std::string::npos);
    }
    for (std::size_t index = 0; index < initial_count; ++index) {
        assert(!lv_obj_find_by_name(root, ("picoclaw_model_edit_" + std::to_string(index)).c_str()));
        assert(!lv_obj_find_by_name(root, ("picoclaw_model_use_" + std::to_string(index)).c_str()));
    }

    struct ProviderExpectation {
        const char *title;
        const char *prefix;
        const char *base;
    };
    constexpr ProviderExpectation providers[] = {
        {"OpenAI", "openai/", "https://api.openai.com/v1"},
        {"Anthropic", "anthropic/", "https://api.anthropic.com/v1"},
        {"Google Gemini", "gemini/", "https://generativelanguage.googleapis.com/v1beta"},
        {"DeepSeek", "deepseek/", "https://api.deepseek.com/v1"},
        {"Ollama", "ollama/", "http://127.0.0.1:11434/v1"},
    };
    auto assert_provider_groups = [&] {
        lv_area_t previous{};
        for (std::size_t provider = 0; provider < 5; ++provider) {
            const auto suffix = std::to_string(provider);
            auto *group = required_object(root, ("picoclaw_provider_group_" + suffix).c_str());
            auto *heading = required_object(group, ("picoclaw_provider_title_" + suffix).c_str());
            auto *add = required_object(group, ("picoclaw_provider_add_" + suffix).c_str());
            auto *configurations = required_object(group, ("picoclaw_provider_configs_" + suffix).c_str());
            assert(std::string(lv_label_get_text(heading)) == providers[provider].title);
            assert(lv_obj_get_parent(add) == configurations);
            assert(lv_obj_get_width(add) >= 44 && lv_obj_get_height(add) >= 44);
            lv_obj_t *reference_card = nullptr;
            for (std::uint32_t child = 0; child < lv_obj_get_child_count(configurations); ++child) {
                auto *candidate = lv_obj_get_child(configurations, child);
                const char *name = lv_obj_get_name(candidate);
                if (name && std::string(name).rfind("picoclaw_model_card_", 0) == 0) {
                    reference_card = candidate;
                    break;
                }
            }
            if (reference_card) {
                assert(lv_obj_get_width(add) == lv_obj_get_width(reference_card));
                assert(lv_obj_get_height(add) == lv_obj_get_height(reference_card));
            } else {
                // Empty provider groups retain a card-sized add tile.
                assert(lv_obj_get_height(add) == 144);
            }
            lv_area_t area{};
            lv_obj_get_coords(group, &area);
            if (provider > 0) assert(area.y1 > previous.y2);
            previous = area;
        }
        auto *openai = required_object(root, "picoclaw_provider_group_0");
        lv_area_t first_openai{}, second_openai{};
        lv_obj_get_coords(required_object(openai, "picoclaw_model_card_2"), &first_openai);
        lv_obj_get_coords(required_object(openai, "picoclaw_model_card_6"), &second_openai);
        if (width >= 600 && height < width) {
            assert(first_openai.y1 == second_openai.y1 && first_openai.x2 < second_openai.x1);
        } else {
            assert(second_openai.y1 > first_openai.y2);
        }
        assert(lv_obj_find_by_name(required_object(root, "picoclaw_provider_group_3"), "picoclaw_model_card_0"));
    };
    assert_provider_groups();

    // All four destinations retain readable captions and full touch targets
    // in both the bottom bar and the landscape rail, including at 320 px.
    constexpr std::pair<const char *, const char *> destinations[] = {
        {"picoclaw_chat_tab", "Chat"},
        {"picoclaw_models_tab", "Models"},
        {"picoclaw_channels_tab", "Channels"},
        {"picoclaw_settings_tab", "Settings"},
    };
    std::vector<lv_area_t> tab_areas;
    for (const auto &destination : destinations) {
        auto *tab = required_object(root, destination.first);
        assert_on_screen(tab, width, height);
        assert(lv_obj_get_width(tab) >= 44 && lv_obj_get_height(tab) >= 44);
        lv_area_t area{};
        lv_obj_get_coords(tab, &area);
        auto *caption = find_label_containing(tab, destination.second);
        assert(caption && std::string(lv_label_get_text(caption)) == destination.second);
        assert_on_screen(caption, width, height);
        lv_area_t caption_area{};
        lv_obj_get_coords(caption, &caption_area);
        assert(caption_area.x1 >= area.x1 && caption_area.x2 <= area.x2);
        assert(caption_area.y1 >= area.y1 && caption_area.y2 <= area.y2);
        for (const auto &previous : tab_areas)
            assert(area.x1 > previous.x2 || area.x2 < previous.x1 ||
                   area.y1 > previous.y2 || area.y2 < previous.y1);
        tab_areas.push_back(area);
    }
    assert(!lv_obj_find_by_name(root, "picoclaw_dashboard_tab"));
    assert(!lv_obj_find_by_name(root, "picoclaw_logs_tab"));

    // Import updates an existing alias in place. The asynchronous completion
    // must refresh its card even though the number of models did not change.
    constexpr const char *imported_key = "sk-render-imported-secret-2468";
    assert(labels_contain(required_object(root, "picoclaw_model_card_0"), "Key not saved"));
    auto *import_button = required_object(root, "picoclaw_model_import");
    assert_on_screen(import_button, width, height);
    lv_obj_send_event(import_button, LV_EVENT_CLICKED, nullptr);
    assert(lv_obj_has_state(import_button, LV_STATE_DISABLED));
    assert(model_card_count(root) == initial_count);
    advance_ui();
    lv_obj_update_layout(root);
    assert(model_card_count(root) == initial_count);
    assert(!lv_obj_has_state(required_object(root, "picoclaw_model_import"), LV_STATE_DISABLED));
    assert(labels_contain(required_object(root, "picoclaw_model_card_0"), "Key"));
    assert(!labels_contain(required_object(root, "picoclaw_model_card_0"), "Key not saved"));
    assert(!labels_contain(required_object(root, "picoclaw_models_page"), imported_key));
    auto *imported_front = required_object(root, "picoclaw_model_front_0");
    auto *imported_title = find_label_containing(imported_front, "DeepSeek Chat");
    assert(imported_title);
    lv_obj_scroll_to_view_recursive(imported_title, LV_ANIM_OFF);
    lv_obj_update_layout(root);
    assert_on_screen(imported_title, width, height);
    // Model cards clip their swipe contents; the focus frame must still be
    // visible on all four sides before Enter opens the editor.
    for (const auto state : {LV_STATE_FOCUS_KEY, LV_STATE_FOCUSED}) {
        lv_obj_add_state(imported_front, state);
        lv_obj_update_layout(root);
        lv_obj_scroll_to_view_recursive(imported_front, LV_ANIM_OFF);
        lv_refr_now(display);
        assert_focus_border_visible(imported_front);
        if (state == LV_STATE_FOCUS_KEY)
            capture_page(display, snapshot_path, "-model-focus", width, height);
        lv_obj_remove_state(imported_front, state);
    }
    press_hardware_enter(imported_front);
    lv_obj_update_layout(root);
    auto *imported_field = required_object(root, "picoclaw_model_editor_key");
    assert(std::string(lv_textarea_get_text(imported_field)) == imported_key);
    assert(lv_textarea_get_password_mode(imported_field));
    assert(std::string(lv_textarea_get_text(required_object(root, "picoclaw_model_editor_provider"))) ==
           "DeepSeek Chat");
    assert_editor_title(root, "DeepSeek Chat");
    assert_simplified_model_editor(root);
    click_and_advance(root, "picoclaw_header_back");

    // Each category's add action supplies its own endpoint and model prefix,
    // without reusing a saved credential. Returning discards every draft.
    for (std::size_t provider = 0; provider < 5; ++provider) {
        const auto add_name = "picoclaw_provider_add_" + std::to_string(provider);
        auto *add = required_object(root, add_name.c_str());
        lv_obj_scroll_to_view_recursive(add, LV_ANIM_OFF);
        lv_obj_update_layout(root);
        assert_on_screen(add, width, height);
        assert_on_screen(required_object(root, "picoclaw_model_import"), width, height);
        click_and_advance(root, add_name.c_str());
        auto *provider_name = required_object(root, "picoclaw_model_editor_provider");
        assert(std::string(lv_textarea_get_text(provider_name)) == providers[provider].title);
        assert_editor_title(root, providers[provider].title);
        assert_simplified_model_editor(root);
        assert(std::string(lv_textarea_get_text(required_object(root, "picoclaw_model_editor_base"))) ==
               providers[provider].base);
        const std::string model_id = lv_textarea_get_text(required_object(root, "picoclaw_model_editor_id"));
        assert(model_id.rfind(providers[provider].prefix, 0) == 0);
        auto *new_key = required_object(root, "picoclaw_model_editor_key");
        assert(std::string(lv_textarea_get_text(new_key)).empty());
        assert(lv_textarea_get_password_mode(new_key));
        click_and_advance(root, "picoclaw_header_back");
        assert(model_card_count(root) == initial_count);
        assert_provider_groups();
    }

    click_and_advance(root, "picoclaw_provider_add_0");
    auto *provider_name = required_object(root, "picoclaw_model_editor_provider");
    auto *id = required_object(root, "picoclaw_model_editor_id");
    auto *base = required_object(root, "picoclaw_model_editor_base");
    auto *key = required_object(root, "picoclaw_model_editor_key");
    auto *save = required_object(root, "picoclaw_model_save");
    auto *keyboard = required_object(root, "picoclaw_models_keyboard");
    constexpr const char *fake_key = "sk-render-test-secret-9876";
    constexpr const char *long_model = "model-with-a-long-identifier-for-small-device-layout";
    assert_simplified_model_editor(root);

    // A custom provider name is also the live editor title. Long titles must
    // fit beside the back/save actions before any configuration is saved.
    constexpr const char *long_provider = "My custom provider with a long display name for a small device";
    lv_textarea_set_text(provider_name, long_provider);
    lv_obj_update_layout(root);
    auto *title = required_object(root, "picoclaw_title");
    assert_editor_title(root, long_provider);
    assert(lv_label_get_long_mode(title) == LV_LABEL_LONG_DOT);
    assert_on_screen(title, width, height);
    assert_on_screen(save, width, height);
    lv_area_t title_area{}, save_area{};
    lv_obj_get_coords(title, &title_area);
    lv_obj_get_coords(save, &save_area);
    assert(title_area.x2 < save_area.x1);
    capture_page(display, snapshot_path, "-model-provider-title", width, height);

    lv_textarea_set_text(provider_name, "");
    click_and_advance(root, "picoclaw_model_save");
    assert(required_object(root, "picoclaw_model_editor_provider") == provider_name);
    assert(labels_contain(required_object(root, "picoclaw_models_feedback"),
                          "Provider name, model ID, and API base are required."));
    lv_textarea_set_text(provider_name, "Device test model");
    assert_editor_title(root, "Device test model");
    lv_textarea_set_text(id, long_model);
    lv_textarea_set_text(base, "https://models.example.test/openai/compatible/v1");
    lv_textarea_set_text(key, fake_key);
    assert(lv_textarea_get_password_mode(key));
    click_and_advance(root, "picoclaw_model_key_toggle");
    assert(!lv_textarea_get_password_mode(key));
    click_and_advance(root, "picoclaw_model_key_toggle");
    assert(lv_textarea_get_password_mode(key));
    exercise_editor_api_controls(root, api_calls_path, width, height);
    assert_on_screen(save, width, height);
    capture_page(display, snapshot_path, "-model-editor", width, height);

    auto assert_draft = [&] {
        assert(required_object(root, "picoclaw_model_editor_provider") == provider_name);
        assert(required_object(root, "picoclaw_model_editor_key") == key);
        assert(std::string(lv_textarea_get_text(provider_name)) == "Device test model");
        assert_editor_title(root, "Device test model");
        assert(std::string(lv_textarea_get_text(id)) == long_model);
        assert(std::string(lv_textarea_get_text(base)) == "https://models.example.test/openai/compatible/v1");
        assert(std::string(lv_textarea_get_text(key)) == fake_key);
    };
    lv_obj_send_event(id, LV_EVENT_FOCUSED, nullptr);
    lv_obj_update_layout(root);
    assert(!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    assert(lv_keyboard_get_textarea(keyboard) == id);
    assert_on_screen(id, width, height);
    assert_on_screen(save, width, height);
    lv_area_t model_area{}, model_keyboard_area{};
    lv_obj_get_coords(id, &model_area);
    lv_obj_get_coords(keyboard, &model_keyboard_area);
    assert(model_area.y2 < model_keyboard_area.y1);
    assert_draft();

    lv_obj_send_event(key, LV_EVENT_FOCUSED, nullptr);
    lv_obj_update_layout(root);
    scroll = required_object(root, "picoclaw_models_scroll");
    assert(!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    assert(lv_keyboard_get_textarea(keyboard) == key);
    lv_area_t key_area{}, keyboard_area{};
    lv_obj_get_content_coords(scroll, &viewport);
    lv_obj_get_coords(key, &key_area);
    lv_obj_get_coords(keyboard, &keyboard_area);
    assert(key_area.y1 >= viewport.y1 && key_area.y2 <= viewport.y2);
    assert(key_area.y2 < keyboard_area.y1);
    for (std::uint32_t child = 0; child < lv_obj_get_child_count(scroll); ++child)
        assert_horizontal_settings_layout(lv_obj_get_child(scroll, child), viewport);
    assert_on_screen(keyboard, width, height);
    assert_on_screen(save, width, height);
    capture_page(display, snapshot_path, "-model-keyboard", width, height);

    // A structural width change must preserve live fields, password state,
    // focus and every draft value, even with the keyboard already visible.
    lv_obj_set_width(surface, width == 320 ? 568 : 320);
    lv_obj_update_layout(root);
    assert_draft();
    assert(lv_keyboard_get_textarea(keyboard) == key);
    assert(lv_textarea_get_password_mode(key));
    lv_obj_set_width(surface, width);
    lv_obj_update_layout(root);
    assert_draft();
    lv_obj_send_event(keyboard, LV_EVENT_CANCEL, nullptr);
    lv_obj_update_layout(root);
    assert(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));

    click_and_advance(root, "picoclaw_model_save");
    assert(model_card_count(root) == initial_count + 1);
    const auto card_name = "picoclaw_model_card_" + std::to_string(initial_count);
    const auto front_name = "picoclaw_model_front_" + std::to_string(initial_count);
    auto *card = required_object(root, card_name.c_str());
    assert_on_screen(card, width, height);
    assert(lv_obj_find_by_name(required_object(root, "picoclaw_provider_group_0"), card_name.c_str()) == card);
    assert_provider_groups();
    assert(labels_contain(card, "Device test model"));
    assert(!labels_contain(required_object(root, "picoclaw_models_page"), fake_key));

    const auto status_suffix = std::to_string(initial_count);
    const auto refresh_name = "picoclaw_model_status_refresh_" + status_suffix;
    auto *refresh = required_object(root, refresh_name.c_str());
    assert(lv_obj_get_width(refresh) == 44 && lv_obj_get_height(refresh) == 44);
    lv_obj_send_event(refresh, LV_EVENT_CLICKED, nullptr);
    assert(labels_contain(required_object(root, card_name.c_str()), "Checking..."));
    advance_ui();
    wait_until_enabled(root, refresh_name.c_str());
    card = required_object(root, card_name.c_str());
    assert(labels_contain(card, "Connected"));
    assert(labels_contain(card, "Available: 12.50 USD"));
    auto *dot = required_object(root, ("picoclaw_model_status_dot_" + status_suffix).c_str());
    assert(lv_color_eq(lv_obj_get_style_bg_color(dot, LV_PART_MAIN), lv_color_hex(0x248A3D)));
    auto *available = required_object(root, ("picoclaw_model_available_" + status_suffix).c_str());
    lv_obj_scroll_to_view_recursive(card, LV_ANIM_OFF);
    lv_obj_update_layout(root);
    assert_on_screen(available, width, height);
    capture_page(display, snapshot_path, "-model-connected", width, height);

    // Newly saved entries are immediately selectable from Chat. Switching
    // applies the configuration in place while retaining an unsent draft.
    click_and_advance(root, "picoclaw_chat_tab");
    auto *chat_model = required_object(root, "picoclaw_chat_model_dropdown");
    assert(lv_dropdown_get_option_count(chat_model) == initial_count + 1);
    assert(std::string(lv_dropdown_get_options(chat_model)).find("OpenAI") != std::string::npos);
    assert(!std::string(lv_dropdown_get_options(chat_model)).empty());
    auto *chat_draft = required_object(root, "picoclaw_message_input");
    lv_textarea_set_text(chat_draft, "Keep this unsent model-switch draft.");
    choose_chat_model(root, "Device test model");
    assert_chat_model_layout(root, width, height);
    capture_page(display, snapshot_path, "-chat-model-selector", width, height);
    lv_dropdown_open(chat_model);
    lv_obj_update_layout(root);
    assert_on_screen(lv_dropdown_get_list(chat_model), width, height);
    capture_page(display, snapshot_path, "-chat-model-selector-open", width, height);
    lv_dropdown_close(chat_model);
    lv_textarea_set_text(chat_draft, "");
    click_and_advance(root, "picoclaw_models_tab");

    click_and_advance(root, front_name.c_str());
    assert(std::string(lv_textarea_get_text(required_object(root, "picoclaw_model_editor_provider"))) ==
           "Device test model");
    assert_editor_title(root, "Device test model");
    assert_simplified_model_editor(root);
    assert(std::string(lv_textarea_get_text(required_object(root, "picoclaw_model_editor_id"))) == long_model);
    key = required_object(root, "picoclaw_model_editor_key");
    assert(std::string(lv_textarea_get_text(key)) == fake_key);
    assert(lv_textarea_get_password_mode(key));
    lv_textarea_set_text(required_object(root, "picoclaw_model_editor_provider"), "Updated device model");
    assert_editor_title(root, "Updated device model");
    lv_textarea_set_text(key, "sk-render-updated-secret-4321");
    constexpr const char *edited_model = "edited-model-after-fetch";
    constexpr const char *edited_base = "https://saved.example.test/v1";
    id = required_object(root, "picoclaw_model_editor_id");
    lv_textarea_set_text(id, edited_model);
    lv_textarea_set_text(required_object(root, "picoclaw_model_editor_base"), edited_base);
    lv_obj_send_event(id, LV_EVENT_FOCUSED, nullptr);
    lv_obj_update_layout(root);
    assert(!lv_obj_has_flag(required_object(root, "picoclaw_models_keyboard"), LV_OBJ_FLAG_HIDDEN));
    tap_pointer(pointer, required_object(root, "picoclaw_model_save"));
    lv_obj_update_layout(root);
    assert(model_card_count(root) == initial_count + 1);
    const PicoClawModel saved_models;
    assert(saved_models.profiles()[initial_count].title == "Updated device model");
    assert(saved_models.profiles()[initial_count].api_key == "sk-render-updated-secret-4321");
    assert(saved_models.profiles()[initial_count].model_id == edited_model);
    assert(saved_models.profiles()[initial_count].api_base == edited_base);
    card = required_object(root, card_name.c_str());
    assert_on_screen(card, width, height);
    assert(labels_contain(card, "Updated device model"));
    assert(labels_contain(card, edited_model));
    assert(!labels_contain(card, long_model));
    capture_page(display, snapshot_path, "-model-saved", width, height);

    // A stale runtime response must not bring the old saved configuration
    // back as another row after returning to the Models list.
    click_and_advance(root, "picoclaw_settings_tab");
    click_and_advance(root, "picoclaw_gateway_refresh");
    click_and_advance(root, "picoclaw_models_tab");
    assert(model_card_count(root) == initial_count + 1);
    card = required_object(root, card_name.c_str());
    assert(labels_contain(card, "Updated device model"));
    assert(labels_contain(card, edited_model));
    page = required_object(root, "picoclaw_models_page");
    assert(!labels_contain(page, fake_key));
    assert(!labels_contain(page, "sk-render-updated-secret-4321"));
    assert_provider_groups();
    scroll = required_object(root, "picoclaw_models_scroll");
    lv_obj_scroll_to_y(scroll, 0, LV_ANIM_OFF);
    lv_obj_update_layout(root);
    capture_page(display, snapshot_path, "-models-top", width, height);
    lv_obj_scroll_to_view_recursive(card, LV_ANIM_OFF);
    lv_obj_update_layout(root);
    scroll = required_object(root, "picoclaw_models_scroll");
    lv_obj_get_content_coords(scroll, &viewport);
    assert_horizontal_settings_layout(required_object(root, "picoclaw_models_list"), viewport);
    capture_page(display, snapshot_path, "-models", width, height);

    click_and_advance(root, "picoclaw_chat_tab");
    chat_model = required_object(root, "picoclaw_chat_model_dropdown");
    assert(lv_dropdown_get_option_count(chat_model) == initial_count + 1);
    assert(std::string(lv_dropdown_get_options(chat_model)).find("Device test model") == std::string::npos);
    assert(std::string(lv_dropdown_get_options(chat_model)).find("Updated device model") != std::string::npos);
    assert(std::string(lv_dropdown_get_options(chat_model)).find("sk-render") == std::string::npos);
    choose_chat_model(root, "Updated device model");
    choose_chat_model(root, "DeepSeek Chat");
    click_and_advance(root, "picoclaw_models_tab");

    // Returning from an editor cancels its unsaved changes. List swipe deletion
    // then requires confirmation and restores the original model list.
    click_and_advance(root, front_name.c_str());
    lv_textarea_set_text(required_object(root, "picoclaw_model_editor_provider"), "Discard this name");
    assert_editor_title(root, "Discard this name");
    click_and_advance(root, "picoclaw_header_back");
    assert(labels_contain(required_object(root, card_name.c_str()), "Updated device model"));
    click_and_advance(root, front_name.c_str());
    assert(std::string(lv_textarea_get_text(required_object(root, "picoclaw_model_editor_key"))) ==
           "sk-render-updated-secret-4321");
    assert(std::string(lv_textarea_get_text(required_object(root, "picoclaw_model_editor_provider"))) ==
           "Updated device model");
    assert(std::string(lv_textarea_get_text(required_object(root, "picoclaw_model_editor_id"))) == edited_model);
    assert(std::string(lv_textarea_get_text(required_object(root, "picoclaw_model_editor_base"))) == edited_base);
    assert_editor_title(root, "Updated device model");
    assert_simplified_model_editor(root);
    click_and_advance(root, "picoclaw_header_back");
    card = required_object(root, card_name.c_str());
    lv_obj_scroll_to_view_recursive(card, LV_ANIM_OFF);
    lv_obj_update_layout(root);
    swipe_model_row(pointer, card, true);
    assert_model_swipe_open(root, initial_count, width, height);
    const auto delete_name = "picoclaw_model_swipe_delete_" + std::to_string(initial_count);
    tap_pointer(pointer, required_object(root, delete_name.c_str()));
    assert(model_card_count(root) == initial_count + 1);
    assert(labels_contain(required_object(root, delete_name.c_str()), "Confirm"));
    tap_pointer(pointer, required_object(root, delete_name.c_str()));
    lv_obj_update_layout(root);
    assert(model_card_count(root) == initial_count);
    assert_provider_groups();
    assert(!labels_contain(required_object(root, "picoclaw_models_page"), "Updated device model"));
    click_and_advance(root, "picoclaw_chat_tab");
    chat_model = required_object(root, "picoclaw_chat_model_dropdown");
    assert(lv_dropdown_get_option_count(chat_model) == initial_count);
    assert(std::string(lv_dropdown_get_options(chat_model)).find("Updated device model") == std::string::npos);
    exercise_model_swipe_delete(root, display, pointer, width, height, snapshot_path);
}

lv_obj_t *named_ancestor(lv_obj_t *object, const char *name)
{
    for (auto *current = object; current; current = lv_obj_get_parent(current)) {
        const char *current_name = lv_obj_get_name(current);
        if (current_name && std::string(current_name) == name) return current;
    }
    return nullptr;
}

void assert_settings_gateway_layout(lv_obj_t *root, int width, int height)
{
    auto *grid = required_object(root, "picoclaw_settings_grid");
    auto *gateway = required_object(root, "picoclaw_gateway_card");
    auto *status = required_object(root, "picoclaw_runtime_status_card");
    // Section captions share a wrapper with their group. Gateway controls and
    // runtime status must be the first two groups in the settings hierarchy.
    assert(lv_obj_get_child(grid, 0) == lv_obj_get_parent(gateway));
    assert(lv_obj_get_child(grid, 1) == lv_obj_get_parent(status));
    assert_on_screen(required_object(root, "picoclaw_runtime_service"), width, height);
    lv_area_t previous{};
    bool first = true;
    for (const char *name : {"picoclaw_runtime_service", "picoclaw_gateway_start", "picoclaw_gateway_stop",
                             "picoclaw_gateway_refresh", "picoclaw_settings_logs"}) {
        auto *control = required_object(root, name);
        assert(named_ancestor(control, "picoclaw_gateway_card") == gateway);
        lv_area_t area{};
        lv_obj_get_coords(control, &area);
        if (!first) assert(area.y1 > previous.y2);
        if (first) first = false;
        else assert(lv_obj_get_height(control) >= 44);
        previous = area;
    }
    for (const char *name : {"picoclaw_runtime_state", "picoclaw_runtime_version", "picoclaw_runtime_network",
                             "picoclaw_runtime_config", "picoclaw_runtime_key", "picoclaw_runtime_gateway"})
        assert(named_ancestor(required_object(root, name), "picoclaw_runtime_status_card") == status);
    assert(!lv_obj_find_by_name(root, "picoclaw_runtime_url"));
    assert(!lv_obj_find_by_name(status, "picoclaw_runtime_service"));
    for (const char *removed : {"picoclaw_profile_card", "picoclaw_profile_dropdown", "picoclaw_profile_title",
                                "picoclaw_model_name", "picoclaw_model_id", "picoclaw_api_base", "picoclaw_api_key",
                                "picoclaw_model_dropdown", "picoclaw_test_connection", "picoclaw_fetch_models",
                                "picoclaw_save_profile", "picoclaw_actions_card", "picoclaw_dashboard_page"})
        assert(!lv_obj_find_by_name(root, removed));
}

void exercise_settings_gateway_logs(lv_obj_t *root, lv_display_t *display, int width, int height,
                                     const char *snapshot_path)
{
    click_and_advance(root, "picoclaw_settings_tab");
    assert_settings_gateway_layout(root, width, height);
    auto *service = required_object(root, "picoclaw_runtime_service");
    assert(std::string(lv_label_get_text(service)) == "Stopped");
    assert(!lv_obj_has_state(required_object(root, "picoclaw_gateway_start"), LV_STATE_DISABLED));
    assert(lv_obj_has_state(required_object(root, "picoclaw_gateway_stop"), LV_STATE_DISABLED));
    lv_obj_send_event(required_object(root, "picoclaw_gateway_start"), LV_EVENT_CLICKED, nullptr);
    assert(lv_obj_has_state(required_object(root, "picoclaw_gateway_start"), LV_STATE_DISABLED));
    assert(lv_obj_has_state(required_object(root, "picoclaw_gateway_refresh"), LV_STATE_DISABLED));
    advance_ui();
    wait_until_enabled(root, "picoclaw_gateway_stop");
    assert(std::string(lv_label_get_text(service)) == "Running");
    assert(lv_obj_has_state(required_object(root, "picoclaw_gateway_start"), LV_STATE_DISABLED));
    click_and_advance(root, "picoclaw_gateway_stop");
    wait_until_enabled(root, "picoclaw_gateway_start");
    assert(std::string(lv_label_get_text(service)) == "Stopped");
    assert(lv_obj_has_state(required_object(root, "picoclaw_gateway_stop"), LV_STATE_DISABLED));
    click_and_advance(root, "picoclaw_gateway_refresh");
    assert(required_object(root, "picoclaw_runtime_service") == service);
    capture_page(display, snapshot_path, "-settings-gateway", width, height);

    auto *settings_tab = required_object(root, "picoclaw_settings_tab");
    auto *chat_tab = required_object(root, "picoclaw_chat_tab");
    assert(lv_obj_has_state(settings_tab, LV_STATE_CHECKED));
    assert(!lv_obj_has_state(chat_tab, LV_STATE_CHECKED));
    const auto selected_color = lv_obj_get_style_text_color(settings_tab, LV_PART_MAIN);
    assert(!lv_color_eq(selected_color,
                       lv_obj_get_style_text_color(chat_tab, LV_PART_MAIN)));
    auto *logs_entry = required_object(root, "picoclaw_settings_logs");
    lv_obj_scroll_to_view_recursive(logs_entry, LV_ANIM_OFF);
    lv_obj_update_layout(root);
    assert_on_screen(logs_entry, width, height);
    click_and_advance(root, "picoclaw_settings_logs");
    assert(required_object(root, "picoclaw_logs_page"));
    assert(!lv_obj_find_by_name(root, "picoclaw_settings_page"));
    assert(std::string(lv_label_get_text(required_object(root, "picoclaw_title"))) == "LOGS");
    assert_on_screen(required_object(root, "picoclaw_header_back"), width, height);
    assert(lv_obj_has_state(settings_tab, LV_STATE_CHECKED));
    assert(lv_color_eq(lv_obj_get_style_text_color(settings_tab, LV_PART_MAIN), selected_color));
    assert(labels_contain(required_object(root, "picoclaw_output"), "Mock gateway activity"));
    auto *output = required_object(root, "picoclaw_output");
    auto *refresh = required_object(root, "picoclaw_logs_refresh");
    assert_on_screen(refresh, width, height);
    lv_obj_send_event(refresh, LV_EVENT_CLICKED, nullptr);
    assert(lv_obj_has_state(refresh, LV_STATE_DISABLED));
    advance_ui();
    wait_until_enabled(root, "picoclaw_logs_refresh");
    assert(required_object(root, "picoclaw_output") == output);
    assert(labels_contain(output, "Mock gateway activity"));
    capture_page(display, snapshot_path, "-logs", width, height);

    click_and_advance(root, "picoclaw_header_back");
    assert(required_object(root, "picoclaw_settings_page"));
    assert(!lv_obj_find_by_name(root, "picoclaw_logs_page"));
    assert(lv_obj_has_flag(required_object(root, "picoclaw_header_back"), LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_state(settings_tab, LV_STATE_CHECKED));
    assert(lv_color_eq(lv_obj_get_style_text_color(settings_tab, LV_PART_MAIN), selected_color));
    assert_settings_gateway_layout(root, width, height);
    click_and_advance(root, "picoclaw_chat_tab");
    assert(lv_obj_has_state(chat_tab, LV_STATE_CHECKED));
    assert(!lv_obj_has_state(settings_tab, LV_STATE_CHECKED));
    assert(lv_color_eq(lv_obj_get_style_text_color(chat_tab, LV_PART_MAIN), selected_color));
    assert(lv_obj_has_flag(required_object(root, "picoclaw_header_back"), LV_OBJ_FLAG_HIDDEN));
}

const char *channel_section_caption(const char *section)
{
    if (std::string(section) == "credentials") return "CREDENTIALS";
    if (std::string(section) == "security") return "SECURITY";
    if (std::string(section) == "access") return "ACCESS";
    if (std::string(section) == "behavior") return "BEHAVIOR";
    if (std::string(section) == "connection") return "CONNECTION";
    if (std::string(section) == "webhook") return "WEBHOOK";
    return "ADVANCED";
}

void assert_channel_editor_schema(lv_obj_t *root,
                                  const std::vector<std::pair<const char *, const char *>> &fields,
                                  const std::vector<const char *> &sections)
{
    std::vector<lv_obj_t *> section_objects;
    auto *general = required_object(root, "picoclaw_channel_general");
    assert(labels_contain(lv_obj_get_parent(general), "CHANNEL"));
    for (const char *section : sections) {
        const auto name = std::string("picoclaw_channel_") + section;
        auto *object = required_object(root, name.c_str());
        // make_settings_section keeps its caption immediately above the white
        // inset group, so inspect the section wrapper as well as the group.
        assert(labels_contain(lv_obj_get_parent(object), channel_section_caption(section)));
        section_objects.push_back(object);
    }
    for (std::size_t index = 1; index < section_objects.size(); ++index) {
        lv_area_t previous{}, current{};
        lv_obj_get_coords(section_objects[index - 1], &previous);
        lv_obj_get_coords(section_objects[index], &current);
        assert(current.y1 > previous.y2);
    }
    if (!section_objects.empty()) {
        lv_area_t general_area{}, first_section{};
        lv_obj_get_coords(general, &general_area);
        lv_obj_get_coords(section_objects.front(), &first_section);
        assert(first_section.y1 > general_area.y2);
    }
    for (const auto &[field, section] : fields) {
        auto *input = required_object(root, (std::string("picoclaw_channel_field_") + field).c_str());
        auto *expected = required_object(root, (std::string("picoclaw_channel_") + section).c_str());
        assert(named_ancestor(input, lv_obj_get_name(expected)) == expected);
    }
}

void exercise_channels_page(lv_obj_t *root, lv_display_t *display, lv_obj_t *surface, lv_indev_t *pointer,
                            int width, int height, const char *snapshot_path, const std::string &failure_path,
                            const std::string &channel_request)
{
    click_and_advance(root, "picoclaw_channels_tab");
    auto *page = required_object(root, "picoclaw_channels_page");
    auto *scroll = required_object(root, "picoclaw_channels_scroll");
    auto *list = required_object(root, "picoclaw_channels_list");
    auto *refresh = required_object(root, "picoclaw_channels_refresh");
    assert_on_screen(refresh, width, height);
    assert(lv_obj_get_width(refresh) >= 44 && lv_obj_get_height(refresh) >= 44);
    assert(!lv_color_eq(lv_obj_get_style_text_color(lv_obj_get_child(refresh, 0), LV_PART_MAIN),
                         lv_obj_get_style_bg_color(refresh, LV_PART_MAIN)));
    lv_area_t viewport{};
    lv_obj_get_content_coords(scroll, &viewport);
    assert_horizontal_settings_layout(list, viewport);
    for (const char *id : {"feishu", "weixin", "discord", "telegram", "slack", "dingtalk", "qq", "whatsapp", "line", "onebot"}) {
        auto *row = required_object(root, (std::string("picoclaw_channel_") + id).c_str());
        assert(lv_obj_get_height(row) >= 44);
        assert(labels_contain(row, "Disabled"));
    }
    assert(labels_contain(page, "Telegram"));
    capture_page(display, snapshot_path, "-channels", width, height);

    struct ChannelSchema {
        const char *id;
        std::vector<std::pair<const char *, const char *>> fields;
        std::vector<const char *> sections;
    };
    const std::vector<ChannelSchema> schemas = {
        {"feishu", {{"app_id", "credentials"}, {"app_secret", "credentials"},
                     {"encrypt_key", "security"}, {"verification_token", "security"},
                     {"is_lark", "behavior"}, {"allow_from", "access"}},
                    {"credentials", "security", "access", "behavior"}},
        {"weixin", {{"proxy", "connection"}, {"allow_from", "access"}},
                    {"access", "connection"}},
        {"discord", {{"token", "credentials"}, {"proxy", "connection"},
                      {"mention_only", "behavior"}, {"allow_from", "access"}},
                     {"credentials", "access", "behavior", "connection"}},
        {"telegram", {{"token", "credentials"}, {"base_url", "connection"},
                       {"proxy", "connection"}, {"allow_from", "access"}},
                      {"credentials", "access", "connection"}},
        {"slack", {{"bot_token", "credentials"}, {"app_token", "credentials"}, {"allow_from", "access"}},
                   {"credentials", "access"}},
        {"dingtalk", {{"client_id", "credentials"}, {"client_secret", "credentials"}, {"allow_from", "access"}},
                      {"credentials", "access"}},
        {"qq", {{"app_id", "credentials"}, {"app_secret", "credentials"}, {"allow_from", "access"}},
               {"credentials", "access"}},
        {"whatsapp", {{"bridge_url", "connection"}, {"allow_from", "access"}},
                     {"access", "connection"}},
        {"line", {{"channel_secret", "credentials"}, {"channel_access_token", "credentials"},
                   {"webhook_host", "webhook"}, {"webhook_port", "webhook"},
                   {"webhook_path", "webhook"}, {"allow_from", "access"}},
                  {"credentials", "access", "webhook"}},
        {"onebot", {{"ws_url", "connection"}, {"access_token", "credentials"}, {"allow_from", "access"}},
                    {"credentials", "access", "connection"}},
    };
    // Every catalog field has a stable input and is placed under the same
    // section as the Web UI schema. Exercise each channel before the detailed
    // Telegram interaction below so missing provider fields cannot hide.
    for (const auto &schema : schemas) {
        auto *row = required_object(root, (std::string("picoclaw_channel_") + schema.id).c_str());
        lv_obj_scroll_to_view_recursive(row, LV_ANIM_OFF);
        lv_obj_update_layout(root);
        tap_pointer(pointer, row);
        lv_obj_update_layout(root);
        assert_channel_editor_schema(root, schema.fields, schema.sections);
        if (std::string(schema.id) == "weixin") {
            auto *binding = required_object(root, "picoclaw_weixin_binding");
            auto *bind = required_object(root, "picoclaw_weixin_bind");
            auto *cancel = required_object(root, "picoclaw_weixin_cancel");
            auto *qr = required_object(root, "picoclaw_weixin_qr");
            auto *bind_status = required_object(root, "picoclaw_weixin_bind_status");
            auto *account = required_object(root, "picoclaw_weixin_account");
            auto *proxy = required_object(root, "picoclaw_channel_field_proxy");
            assert(labels_contain(lv_obj_get_parent(binding), "WEIXIN ACCOUNT"));
            assert(lv_obj_check_type(qr, &lv_qrcode_class));
            assert(lv_obj_has_flag(qr, LV_OBJ_FLAG_HIDDEN));
            assert(lv_obj_has_flag(cancel, LV_OBJ_FLAG_HIDDEN));
            assert(lv_obj_get_width(bind) >= 44 && lv_obj_get_height(bind) >= 44);
            assert(labels_contain(bind_status, "QR"));
            assert(labels_contain(account, "No Weixin account"));

            constexpr const char *proxy_draft = "https://proxy.example.test:8443";
            lv_textarea_set_text(proxy, proxy_draft);
            lv_obj_send_event(proxy, LV_EVENT_VALUE_CHANGED, nullptr);
            lv_obj_send_event(proxy, LV_EVENT_FOCUSED, nullptr);
            lv_obj_update_layout(root);
            auto *keyboard = required_object(root, "picoclaw_channels_keyboard");
            assert(lv_keyboard_get_textarea(keyboard) == proxy);
            assert_on_screen(required_object(root, "picoclaw_channel_save"), width, height);
            assert_on_screen(required_object(root, "picoclaw_header_back"), width, height);

            // The first request creates a QR and is cancelled before a scan;
            // this must hide the QR and leave the edited proxy draft intact.
            click_and_advance(root, "picoclaw_weixin_bind");
            advance_ui();
            scroll = required_object(root, "picoclaw_channels_scroll");
            assert(!lv_obj_has_flag(qr, LV_OBJ_FLAG_HIDDEN));
            assert(lv_obj_get_width(qr) <= lv_obj_get_width(scroll));
            assert(lv_obj_get_height(qr) <= lv_obj_get_height(scroll));
            assert(labels_contain(bind_status, "Scan this QR"));
            assert(!lv_obj_has_flag(cancel, LV_OBJ_FLAG_HIDDEN));
            assert(lv_obj_has_state(required_object(root, "picoclaw_channel_save"), LV_STATE_DISABLED));
            assert_on_screen(required_object(root, "picoclaw_header_back"), width, height);
            capture_page(display, snapshot_path, "-weixin-binding", width, height);

            auto resize_binding_viewport = [&](int viewport_width, int viewport_height) {
                display_width = viewport_width;
                framebuffer.resize(static_cast<std::size_t>(viewport_width) * static_cast<std::size_t>(viewport_height));
                lv_display_set_resolution(display, viewport_width, viewport_height);
                lv_obj_set_size(surface, viewport_width, viewport_height);
                lv_obj_update_layout(root);
                advance_ui();
            };
            resize_binding_viewport(height, width);
            assert(!lv_obj_has_flag(qr, LV_OBJ_FLAG_HIDDEN));
            assert_on_screen(qr, height, width);
            const auto assert_qr_in_viewport = [&] {
                lv_area_t qr_area{}, binding_viewport{};
                lv_obj_get_coords(qr, &qr_area);
                lv_obj_get_content_coords(required_object(root, "picoclaw_channels_scroll"), &binding_viewport);
                assert(qr_area.x1 >= binding_viewport.x1 && qr_area.x2 <= binding_viewport.x2);
                assert(qr_area.y1 >= binding_viewport.y1 && qr_area.y2 <= binding_viewport.y2);
            };
            assert_qr_in_viewport();
            resize_binding_viewport(width, height);
            assert(!lv_obj_has_flag(qr, LV_OBJ_FLAG_HIDDEN));
            assert_on_screen(qr, width, height);
            assert_qr_in_viewport();
            assert_on_screen(required_object(root, "picoclaw_channel_save"), width, height);
            assert_on_screen(required_object(root, "picoclaw_header_back"), width, height);
            click_and_advance(root, "picoclaw_weixin_cancel");
            assert(lv_obj_has_flag(qr, LV_OBJ_FLAG_HIDDEN));
            assert(lv_obj_has_flag(cancel, LV_OBJ_FLAG_HIDDEN));
            assert(labels_contain(bind_status, "cancelled"));
            assert(std::string(lv_textarea_get_text(proxy)) == proxy_draft);

            // A second QR reaches the scanned and confirmed states. The
            // credentials stay hidden from the editor, while the account
            // summary and save payload expose only the result needed to save.
            click_and_advance(root, "picoclaw_weixin_bind");
            advance_ui();
            assert(!lv_obj_has_flag(qr, LV_OBJ_FLAG_HIDDEN));
            assert(labels_contain(bind_status, "Scan this QR"));
            advance_binding();
            assert(!lv_obj_has_flag(qr, LV_OBJ_FLAG_HIDDEN));
            assert(labels_contain(bind_status, "QR code scanned"));
            advance_binding();
            assert(lv_obj_has_flag(qr, LV_OBJ_FLAG_HIDDEN));
            assert(lv_obj_has_flag(cancel, LV_OBJ_FLAG_HIDDEN));
            assert(labels_contain(bind_status, "Save to apply"));
            assert(labels_contain(account, "render-weixin-account"));
            assert(std::string(lv_textarea_get_text(proxy)) == proxy_draft);
            assert(!lv_obj_find_by_name(root, "picoclaw_channel_field_token"));
            assert(!lv_obj_find_by_name(root, "picoclaw_channel_field_account_id"));
            click_and_advance(root, "picoclaw_channel_save");
            std::ifstream binding_request(channel_request);
            const std::string binding_payload((std::istreambuf_iterator<char>(binding_request)), {});
            assert(binding_payload.find("render-weixin-token-7890") != std::string::npos);
            assert(binding_payload.find("render-weixin-account") != std::string::npos);
            assert(binding_payload.find(proxy_draft) != std::string::npos);

            // Expiry hides the QR and enables a retry; a failed poll follows
            // the same path and keeps the saved account summary.
            click_and_advance(root, "picoclaw_weixin_bind");
            advance_ui();
            assert(!lv_obj_has_flag(qr, LV_OBJ_FLAG_HIDDEN));
            advance_binding();
            assert(lv_obj_has_flag(qr, LV_OBJ_FLAG_HIDDEN));
            assert(labels_contain(bind_status, "expired"));
            click_and_advance(root, "picoclaw_weixin_bind");
            advance_ui();
            assert(!lv_obj_has_flag(qr, LV_OBJ_FLAG_HIDDEN));
            advance_binding();
            assert(lv_obj_has_flag(qr, LV_OBJ_FLAG_HIDDEN));
            assert(labels_contain(bind_status, "Could not bind Weixin"));
            assert(labels_contain(account, "render-weixin-account"));
        }
        click_and_advance(root, "picoclaw_header_back");
    }

    auto *telegram = required_object(root, "picoclaw_channel_telegram");
    lv_obj_scroll_to_view_recursive(telegram, LV_ANIM_OFF);
    lv_obj_update_layout(root);
    assert_on_screen(telegram, width, height);
    tap_pointer(pointer, telegram);
    lv_obj_update_layout(root);
    assert_editor_title(root, "Telegram");
    auto *token = required_object(root, "picoclaw_channel_field_token");
    auto *allow_from = required_object(root, "picoclaw_channel_field_allow_from");
    auto *enabled = required_object(root, "picoclaw_channel_enabled");
    auto *save = required_object(root, "picoclaw_channel_save");
    auto *back = required_object(root, "picoclaw_header_back");
    auto *keyboard = required_object(root, "picoclaw_channels_keyboard");
    assert(lv_obj_check_type(enabled, &lv_switch_class));
    assert(!lv_obj_has_state(enabled, LV_STATE_CHECKED));
    assert(lv_textarea_get_password_mode(token));
    assert_on_screen(save, width, height);
    assert_on_screen(back, width, height);
    scroll = required_object(root, "picoclaw_channels_scroll");
    lv_obj_get_content_coords(scroll, &viewport);
    for (std::uint32_t index = 0; index < lv_obj_get_child_count(scroll); ++index)
        assert_horizontal_settings_layout(lv_obj_get_child(scroll, index), viewport);

    constexpr const char *draft_token = "render-channel-secret-7890";
    constexpr const char *draft_allow_from = "123456789, 987654321";
    lv_textarea_set_text(token, draft_token);
    lv_obj_send_event(token, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_textarea_set_text(allow_from, draft_allow_from);
    lv_obj_send_event(allow_from, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_state(enabled, LV_STATE_CHECKED);
    lv_obj_send_event(enabled, LV_EVENT_VALUE_CHANGED, nullptr);
    advance_ui();
    lv_obj_update_layout(root);
    capture_page(display, snapshot_path, "-channel-editor", width, height);

    auto assert_draft = [&] {
        assert(required_object(root, "picoclaw_channel_field_token") == token);
        assert(required_object(root, "picoclaw_channel_field_allow_from") == allow_from);
        assert(std::string(lv_textarea_get_text(token)) == draft_token);
        assert(std::string(lv_textarea_get_text(allow_from)) == draft_allow_from);
        assert(lv_obj_has_state(enabled, LV_STATE_CHECKED));
        assert(lv_textarea_get_password_mode(token));
    };
    lv_obj_send_event(token, LV_EVENT_FOCUSED, nullptr);
    lv_obj_update_layout(root);
    assert(!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    assert(lv_keyboard_get_textarea(keyboard) == token);
    scroll = required_object(root, "picoclaw_channels_scroll");
    lv_area_t field_area{}, keyboard_area{};
    lv_obj_get_content_coords(scroll, &viewport);
    lv_obj_get_coords(token, &field_area);
    lv_obj_get_coords(keyboard, &keyboard_area);
    assert(field_area.y1 >= viewport.y1 && field_area.y2 <= viewport.y2);
    assert(field_area.y2 < keyboard_area.y1);
    assert_on_screen(keyboard, width, height);
    assert_on_screen(save, width, height);
    assert_on_screen(back, width, height);
    assert_draft();
    capture_page(display, snapshot_path, "-channel-keyboard", width, height);

    // Fields at the end of a channel's options remain reachable as the
    // keyboard reduces the scrollable area on the smallest display.
    lv_obj_send_event(allow_from, LV_EVENT_FOCUSED, nullptr);
    lv_obj_update_layout(root);
    assert(lv_keyboard_get_textarea(keyboard) == allow_from);
    lv_obj_get_content_coords(scroll, &viewport);
    lv_obj_get_coords(allow_from, &field_area);
    assert(field_area.y1 >= viewport.y1 && field_area.y2 <= viewport.y2);
    assert(field_area.y2 < keyboard_area.y1);
    assert_on_screen(save, width, height);
    lv_obj_send_event(token, LV_EVENT_FOCUSED, nullptr);
    lv_obj_update_layout(root);

    // Rotate the surface and cross the compact form breakpoint while the
    // keyboard owns the secret field. Live values and focus must survive.
    auto resize_viewport = [&](int viewport_width, int viewport_height) {
        // The parent screen must rotate too: enlarging only the child
        // surface would test overflow scrolling instead of a display change.
        display_width = viewport_width;
        framebuffer.resize(static_cast<std::size_t>(viewport_width) * static_cast<std::size_t>(viewport_height));
        lv_display_set_resolution(display, viewport_width, viewport_height);
        lv_obj_set_size(surface, viewport_width, viewport_height);
        lv_obj_update_layout(root);
    };
    resize_viewport(height, width);
    assert_draft();
    assert(lv_keyboard_get_textarea(keyboard) == token);
    resize_viewport(width == 320 ? 568 : 320, height);
    assert_draft();
    assert(lv_keyboard_get_textarea(keyboard) == token);
    resize_viewport(width, height);
    assert_draft();
    assert_on_screen(surface, width, height);
    assert_on_screen(back, width, height);

    // A backend error must preserve credentials and the enabled draft so it
    // can be retried from the same editor without retyping sensitive values.
    std::ofstream(failure_path) << "fail\n";
    lv_obj_send_event(save, LV_EVENT_CLICKED, nullptr);
    assert(lv_obj_has_state(save, LV_STATE_DISABLED));
    advance_ui();
    wait_until_enabled(root, "picoclaw_channel_save");
    lv_obj_update_layout(root);
    assert_draft();
    assert(labels_contain(required_object(root, "picoclaw_channel_feedback"), "Could not save channel"));
    assert_on_screen(save, width, height);
    capture_page(display, snapshot_path, "-channel-error", width, height);
    std::filesystem::remove(failure_path);

    click_and_advance(root, "picoclaw_channel_save");
    assert(std::string(lv_textarea_get_text(required_object(root, "picoclaw_channel_field_token"))) == draft_token);
    assert(labels_contain(required_object(root, "picoclaw_channel_feedback"), "Restart the gateway"));
    assert(lv_obj_has_state(required_object(root, "picoclaw_channel_enabled"), LV_STATE_CHECKED));
    click_and_advance(root, "picoclaw_header_back");
    assert(!lv_obj_find_by_name(root, "picoclaw_channel_field_token"));
    assert(labels_contain(required_object(root, "picoclaw_channel_telegram"), "Enabled"));
    assert(!labels_contain(required_object(root, "picoclaw_channels_page"), draft_token));
    click_and_advance(root, "picoclaw_channels_refresh");
    assert(labels_contain(required_object(root, "picoclaw_channel_telegram"), "Enabled"));
    click_and_advance(root, "picoclaw_channel_telegram");
    token = required_object(root, "picoclaw_channel_field_token");
    assert(std::string(lv_textarea_get_text(token)) == draft_token);
    assert(lv_textarea_get_password_mode(token));

    // Back discards an unsaved change and returns to the same channel list.
    lv_textarea_set_text(token, "unsaved-channel-token");
    lv_obj_send_event(token, LV_EVENT_VALUE_CHANGED, nullptr);
    click_and_advance(root, "picoclaw_header_back");
    click_and_advance(root, "picoclaw_channel_telegram");
    assert(std::string(lv_textarea_get_text(required_object(root, "picoclaw_channel_field_token"))) == draft_token);
    click_and_advance(root, "picoclaw_header_back");
    click_and_advance(root, "picoclaw_chat_tab");
}

}  // namespace

int main(int argc, char **argv)
{
    TestEnvironment environment;
    environment.set("PICOCLAW_HOME", environment.root + "/runtime");
    assert(argc == 4);
    const int width  = std::atoi(argv[1]);
    const int height = std::atoi(argv[2]);
    assert(width >= 320 && height >= 240);
    const auto helper = environment.root + "/control.sh";
    const auto commands = environment.root + "/commands.txt";
    const auto model_status = environment.root + "/runtime-model-status.txt";
    const auto gateway_status = environment.root + "/gateway-status.txt";
    const auto api_calls = environment.root + "/api-calls.txt";
    const auto channel_failure = environment.root + "/channel-failure";
    const auto channel_values = environment.root + "/channels.txt";
    const auto saved_channel_values = environment.root + "/saved-channels.txt";
    const auto channel_request = environment.root + "/channel-request.json";
    const auto weixin_bind_starts = environment.root + "/weixin-bind-starts.txt";
    const auto weixin_bind_polls = environment.root + "/weixin-bind-polls.txt";
    const auto weixin_bind_start_wire = environment.root + "/weixin-bind-start.txt";
    const auto weixin_bind_scan_wire = environment.root + "/weixin-bind-scan.txt";
    const auto weixin_bind_confirmed_wire = environment.root + "/weixin-bind-confirmed.txt";
    const auto weixin_bind_expired_wire = environment.root + "/weixin-bind-expired.txt";
    std::ofstream(gateway_status) << "gateway=stopped\n";
    const std::string other_channels =
        "\"feishu\" 0 6\n\"app_id\" \"\"\n\"app_secret\" \"\"\n\"encrypt_key\" \"\"\n"
        "\"verification_token\" \"\"\n\"is_lark\" \"false\"\n\"allow_from\" \"\"\n"
        "\"weixin\" 0 6\n\"token\" \"\"\n\"account_id\" \"\"\n\"base_url\" \"\"\n"
        "\"cdn_base_url\" \"\"\n\"proxy\" \"\"\n\"allow_from\" \"\"\n"
        "\"discord\" 0 4\n\"token\" \"\"\n\"proxy\" \"\"\n"
        "\"mention_only\" \"false\"\n\"allow_from\" \"\"\n";
    const std::string more_channels =
        "\"slack\" 0 3\n\"bot_token\" \"\"\n\"app_token\" \"\"\n\"allow_from\" \"\"\n"
        "\"dingtalk\" 0 3\n\"client_id\" \"\"\n\"client_secret\" \"\"\n\"allow_from\" \"\"\n"
        "\"qq\" 0 3\n\"app_id\" \"\"\n\"app_secret\" \"\"\n\"allow_from\" \"\"\n"
        "\"whatsapp\" 0 2\n\"bridge_url\" \"\"\n\"allow_from\" \"\"\n"
        "\"line\" 0 6\n\"channel_secret\" \"\"\n\"channel_access_token\" \"\"\n"
        "\"webhook_host\" \"0.0.0.0\"\n\"webhook_port\" \"18791\"\n\"webhook_path\" \"/webhook\"\n\"allow_from\" \"\"\n"
        "\"onebot\" 0 3\n\"ws_url\" \"\"\n\"access_token\" \"\"\n\"allow_from\" \"\"\n";
    std::string saved_other_channels = other_channels;
    const std::string weixin_empty_prefix = "\"weixin\" 0 6\n\"token\" \"\"\n\"account_id\" \"\"";
    const auto weixin_prefix = saved_other_channels.find(weixin_empty_prefix);
    assert(weixin_prefix != std::string::npos);
    saved_other_channels.replace(weixin_prefix, weixin_empty_prefix.size(),
                                 "\"weixin\" 1 6\n\"token\" \"render-weixin-token-7890\"\n\"account_id\" \"render-weixin-account\"");
    std::ofstream(weixin_bind_start_wire)
        << "PICOCLAW_WEIXIN_BINDING 1\nstatus \"wait\"\nqrcode \"render-weixin-qrcode\"\n"
           "qr_url \"https://ilinkai.weixin.qq.com/qr/render-weixin-qrcode\"\n"
           "base_url \"https://ilinkai.weixin.qq.com/\"\nEND_PICOCLAW_WEIXIN_BINDING\n";
    std::ofstream(weixin_bind_scan_wire)
        << "PICOCLAW_WEIXIN_BINDING 1\nstatus \"scaned\"\nqrcode \"render-weixin-qrcode\"\n"
           "qr_url \"https://ilinkai.weixin.qq.com/qr/render-weixin-qrcode\"\nEND_PICOCLAW_WEIXIN_BINDING\n";
    std::ofstream(weixin_bind_confirmed_wire)
        << "PICOCLAW_WEIXIN_BINDING 1\nstatus \"confirmed\"\ntoken \"render-weixin-token-7890\"\n"
           "account_id \"render-weixin-account\"\nbase_url \"https://ilinkai.weixin.qq.com/\"\nEND_PICOCLAW_WEIXIN_BINDING\n";
    std::ofstream(weixin_bind_expired_wire)
        << "PICOCLAW_WEIXIN_BINDING 1\nstatus \"expired\"\nEND_PICOCLAW_WEIXIN_BINDING\n";
    std::ofstream(channel_values) << "PICOCLAW_CHANNELS 1 10\n" << other_channels <<
        "\"telegram\" 0 4\n\"token\" \"\"\n\"base_url\" \"\"\n\"proxy\" \"\"\n"
        "\"allow_from\" \"\"\n" << more_channels << "END_CHANNELS\n";
    std::ofstream(saved_channel_values) << "PICOCLAW_CHANNELS 1 10\n" << saved_other_channels <<
        "\"telegram\" 1 4\n\"token\" \"render-channel-secret-7890\"\n\"base_url\" \"\"\n"
        "\"proxy\" \"\"\n\"allow_from\" \"123456789, 987654321\"\n" << more_channels << "END_CHANNELS\n";
    std::ofstream(helper) << "#!/bin/sh\nprintf '%s\\n' \"$*\" >> '" << commands << "'\n"
                            "if [ \"$1\" = ask ]; then\nsleep 0.05\ncat <<'REPLY'\n"
                             "Here is a longer response to check the conversation while typing.\n\n"
                             "The first paragraph explains the project and its main components.\n\n"
                             "The second paragraph describes how the device connects to the runtime.\n\n"
                             "The final paragraph stays visible above the input and keyboard.\n"
                             "Latest reply ends here.\nREPLY\n"
                             "elif [ \"$1\" = check-updates ]; then\nsleep 0.05\ncat <<'UPDATES'\n"
                             "updates=ready\nlatest_stable=v0.3.1\nupdate_status=available\n"
                             "update_message=A new stable release is available.\n"
                             "release=nightly\tNightly development build\tnightly\t2026-09-14\tyes\n"
                             "release=v0.3.1\tStable version with a long descriptive release title\tstable\t2026-09-12\tyes\n"
                             "release=v0.2.0-rc1\tPreview\tprerelease\t2026-08-01\tno\n"
                             "end_releases=yes\nUPDATES\n"
                             "elif [ \"$1\" = export-models ]; then\nsleep 0.05\ncat <<'MODELS'\n"
                             "PICOCLAW_MODELS 1 1\n"
                             "\"DeepSeek Chat\" \"deepseek-chat\" \"deepseek/deepseek-chat\" "
                             "\"https://api.deepseek.com/v1\" \"sk-render-imported-secret-2468\"\nMODELS\n"
                             "elif [ \"$1\" = load-channels ]; then\nsleep 0.05\ncat '" << channel_values << "'\n"
                             "elif [ \"$1\" = weixin-bind-start ]; then\n"
                             "cat >/dev/null\n"
                             "weixin_bind_start_count=1\n"
                             "if [ -f '" << weixin_bind_starts << "' ]; then weixin_bind_start_count=$(cat '" << weixin_bind_starts << "'); weixin_bind_start_count=$((weixin_bind_start_count + 1)); fi\n"
                             "printf '%s\\n' \"$weixin_bind_start_count\" > '" << weixin_bind_starts << "'\n"
                             "cat '" << weixin_bind_start_wire << "'\n"
                             "elif [ \"$1\" = weixin-bind-poll ]; then\n"
                             "cat >/dev/null\n"
                             "weixin_bind_poll_count=1\n"
                             "if [ -f '" << weixin_bind_polls << "' ]; then weixin_bind_poll_count=$(cat '" << weixin_bind_polls << "'); weixin_bind_poll_count=$((weixin_bind_poll_count + 1)); fi\n"
                             "printf '%s\\n' \"$weixin_bind_poll_count\" > '" << weixin_bind_polls << "'\n"
                             "weixin_bind_start_count=$(cat '" << weixin_bind_starts << "' 2>/dev/null || printf 1)\n"
                             "if [ \"$weixin_bind_start_count\" = 1 ]; then cat '" << weixin_bind_scan_wire << "'; exit 0; fi\n"
                             "if [ \"$weixin_bind_start_count\" = 2 ] && [ \"$weixin_bind_poll_count\" = 1 ]; then cat '" << weixin_bind_scan_wire << "'; exit 0; fi\n"
                             "if [ \"$weixin_bind_start_count\" = 2 ]; then cat '" << weixin_bind_confirmed_wire << "'; exit 0; fi\n"
                             "if [ \"$weixin_bind_start_count\" = 3 ]; then cat '" << weixin_bind_expired_wire << "'; exit 0; fi\n"
                             "printf 'Mock Weixin binding failed.\\n' >&2; exit 1\n"
                             "elif [ \"$1\" = save-channel ]; then\nsleep 0.05\ncat > '" << channel_request << "'\n"
                             "if [ -f '" << channel_failure << "' ]; then\n"
                             "printf 'Mock channel write failed.\\n'; exit 1; fi\n"
                             "cp '" << saved_channel_values << "' '" << channel_values << "'\n"
                             "printf 'OK: channel configuration saved. Restart the gateway to apply changes.\\n'\n"
                             "elif [ \"$1\" = test-connection ] || [ \"$1\" = fetch-models ]; then\n"
                             "sleep 0.05\nrender_api_key=$(cat)\n"
                             "case \"$render_api_key\" in\n"
                             "sk-render-test-secret-9876) render_auth=primary ;;\n"
                             "sk-render-draft-secret-9999) render_auth=updated ;;\n"
                             "'') render_auth=local ;;\n"
                             "*) render_auth=unexpected ;;\nesac\n"
                             "printf '%s base=%s auth=%s\\n' \"$1\" \"$2\" \"$render_auth\" >> '"
                          << api_calls << "'\n"
                             "if [ \"$render_auth\" = unexpected ]; then printf 'Mock authentication failed.\\n'; exit 1; fi\n"
                             "if [ \"$1\" = test-connection ]; then\n"
                             "  if [ \"$render_auth\" = local ]; then printf 'connection=ok\\nquota=unmetered\\n'; exit 0; fi\n"
                             "  case \"$2\" in *failure*) printf 'Mock connection rejected.\\n'; exit 1 ;; esac\n"
                             "  printf 'connection=ok\\nquota=available\\nbalance=USD\\t12.50\\n'\n"
                             "elif [ \"$render_auth\" = updated ]; then\n"
                             "  printf 'model=openai/new-key-model\\n'\n"
                             "elif [ \"$2\" = https://alternate.example.test/v1 ]; then\n"
                             "  printf 'model=openai/alternate-alpha\\nmodel=openai/alternate-beta\\n'\n"
                             "else\n  printf 'model=fetched-alpha\\nmodel=vendor/fetched-beta\\n'\nfi\n"
                             "elif [ \"$1\" = gateway-start ]; then\nsleep 0.05\n"
                             "printf 'gateway=running\\n' > '" << gateway_status << "'\n"
                             "cat '" << gateway_status << "'\n"
                             "elif [ \"$1\" = gateway-stop ]; then\nsleep 0.05\n"
                             "printf 'gateway=stopped\\n' > '" << gateway_status << "'\n"
                             "cat '" << gateway_status << "'\n"
                             "elif [ \"$1\" = log ]; then\nsleep 0.05\nprintf 'Mock gateway activity\\n'\n"
                             "elif [ \"$1\" = save-config ]; then\nsleep 0.05\ncat >/dev/null\n"
                             "printf 'model_name=%s\\nmodel_id=%s\\napi_base=%s\\n' \"$2\" \"$3\" \"$4\" > '"
                          << model_status << "'\nprintf 'Configuration saved.\\n'\n"
                             "elif [ \"$1\" = status ] && [ -f '" << model_status << "' ]; then\n"
                             "printf 'installed=yes\\nversion=v0.3.0\\nnetwork=ready\\nupdate_supported=yes\\nconfig=ready\\nmodel_key=ready\\n'\n"
                             "cat '" << gateway_status << "'\n"
                             "cat '" << model_status << "'\n"
                             "else\nprintf 'installed=no\\nupdate_supported=yes\\n'\nfi\n";
    (void)setenv("LILYGO_PICOCLAW_CONTROL", helper.c_str(), 1);

    display_width = width;
    framebuffer.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    std::vector<lv_color32_t> draw_buffer(static_cast<std::size_t>(width) * kPartialRows);

    lv_init();
    auto *display = lv_display_create(width, height);
    assert(display);
    lv_display_set_buffers(display, draw_buffer.data(), nullptr,
                           static_cast<std::uint32_t>(draw_buffer.size() * sizeof(lv_color32_t)),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_display);
    lv_display_set_default(display);
    auto *pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer, display);
    lv_indev_set_read_cb(pointer, read_pointer);

    cm0_app_context_t context{};
    context.root           = lv_screen_active();
    const auto *descriptor = cm0_app_get_descriptor();
    descriptor->open(&context);
    lv_obj_update_layout(context.root);
    advance_ui();

    auto *surface       = lv_obj_find_by_name(context.root, "picoclaw_surface");
    auto *header        = lv_obj_find_by_name(context.root, "picoclaw_header");
    auto *title         = lv_obj_find_by_name(context.root, "picoclaw_title");
    auto *chat          = lv_obj_find_by_name(context.root, "picoclaw_chat_page");
    auto *settings_tab  = lv_obj_find_by_name(context.root, "picoclaw_settings_tab");
    auto *bottom_nav    = lv_obj_find_by_name(context.root, "picoclaw_bottom_navigation");
    assert(surface && header && title && chat && settings_tab && bottom_nav);
    assert(lv_obj_get_child_count(bottom_nav) == 4);
    assert(!lv_obj_find_by_name(context.root, "picoclaw_dashboard_tab"));
    assert(!lv_obj_find_by_name(context.root, "picoclaw_logs_tab"));
    assert(lv_obj_has_flag(required_object(context.root, "picoclaw_header_back"), LV_OBJ_FLAG_HIDDEN));
    assert(std::string(lv_label_get_text(title)) == (width < 480 ? "CHAT" : "AI CHAT"));
    assert(lv_obj_get_style_text_font(title, LV_PART_MAIN) != nullptr);
    assert(lv_color_eq(lv_obj_get_style_bg_color(surface, LV_PART_MAIN), lv_color_hex(kColorPageBackground)));
    assert(lv_color_eq(lv_obj_get_style_bg_color(header, LV_PART_MAIN), lv_color_hex(kColorContainerBackground)));

    // Updates are reached directly from Settings. Checking releases never
    // installs a default version before the user selects one.
    click_and_advance(context.root, "picoclaw_settings_tab");
    assert_settings_gateway_layout(context.root, width, height);
    auto *initial_updates = required_object(context.root, "picoclaw_check_updates");
    lv_obj_scroll_to_view_recursive(initial_updates, LV_ANIM_OFF);
    lv_obj_update_layout(context.root);
    assert_on_screen(initial_updates, width, height);
    lv_obj_send_event(initial_updates, LV_EVENT_CLICKED, nullptr);
    lv_obj_update_layout(context.root);
    assert(lv_obj_find_by_name(context.root, "picoclaw_updates_card"));
    auto *initial_release = lv_obj_find_by_name(context.root, "picoclaw_release_dropdown");
    auto *initial_install = lv_obj_find_by_name(context.root, "picoclaw_install");
    assert(initial_release && initial_install && lv_obj_has_state(initial_release, LV_STATE_DISABLED));
    advance_ui();
    assert(lv_dropdown_get_option_count(initial_release) == 3);
    assert(lv_dropdown_get_selected(initial_release) == 1);
    assert(!lv_obj_has_state(initial_install, LV_STATE_DISABLED));
    {
        std::ifstream recorded(commands);
        const std::string text((std::istreambuf_iterator<char>(recorded)), {});
        assert(text.find("check-updates\n") != std::string::npos);
        assert(text.find("install-release") == std::string::npos);
    }
    auto *chat_tab_initial = lv_obj_find_by_name(context.root, "picoclaw_chat_tab");
    assert(chat_tab_initial);
    lv_obj_send_event(chat_tab_initial, LV_EVENT_CLICKED, nullptr);
    lv_obj_update_layout(context.root);
    auto *conversation   = lv_obj_find_by_name(context.root, "picoclaw_conversation");
    auto *input          = lv_obj_find_by_name(context.root, "picoclaw_message_input");
    auto *send           = lv_obj_find_by_name(context.root, "picoclaw_send_button");
    auto *new_chat       = lv_obj_find_by_name(context.root, "picoclaw_new_chat");
    auto *connection     = lv_obj_find_by_name(context.root, "picoclaw_connection_badge");
    auto *connection_dot = lv_obj_find_by_name(context.root, "picoclaw_connection_dot");
    assert(conversation && input && send && new_chat && connection && connection_dot);
    assert(std::string(lv_label_get_text(title)) == (width < 480 ? "CHAT" : "AI CHAT"));
    assert(lv_obj_get_width(conversation) > 200);
    assert(lv_obj_get_height(conversation) > 100);
    assert(lv_obj_get_width(connection) == (width < 480 ? 14 : 82));
    assert_chat_model_layout(context.root, width, height);
    assert_chat_focus_borders(context.root, display);

    exercise_models_page(context.root, display, surface, pointer, width, height, argv[3], api_calls);
    exercise_channels_page(context.root, display, surface, pointer, width, height, argv[3], channel_failure,
                           channel_request);
    {
        std::ifstream request(channel_request);
        const std::string saved((std::istreambuf_iterator<char>(request)), {});
        assert(saved.find("render-channel-secret-7890") != std::string::npos);
        assert(saved.find("123456789, 987654321") != std::string::npos);
        std::ifstream recorded(commands);
        const std::string calls((std::istreambuf_iterator<char>(recorded)), {});
        assert(calls.find("load-channels\n") != std::string::npos);
        assert(calls.find("save-channel telegram") != std::string::npos);
        assert(calls.find("weixin-bind-start\n") != std::string::npos);
        assert(calls.find("weixin-bind-poll\n") != std::string::npos);
        assert(calls.find("render-channel-secret-7890") == std::string::npos);
        assert(calls.find("render-weixin-token-7890") == std::string::npos);
        assert(calls.find("render-weixin-account") == std::string::npos);
    }

    exercise_settings_gateway_logs(context.root, display, width, height, argv[3]);

    input = lv_obj_find_by_name(context.root, "picoclaw_message_input");
    send  = lv_obj_find_by_name(context.root, "picoclaw_send_button");
    assert(input && send);

    advance_ui();
    lv_textarea_set_text(input, "Hello PicoClaw");
    lv_obj_send_event(send, LV_EVENT_CLICKED, nullptr);
    assert(lv_obj_find_by_name(context.root, "picoclaw_user_message"));
    assert(lv_obj_find_by_name(context.root, "picoclaw_assistant_message"));
    assert(lv_obj_find_by_name(context.root, "picoclaw_user_avatar"));
    assert(lv_obj_find_by_name(context.root, "picoclaw_assistant_avatar"));
    assert(lv_obj_has_state(required_object(context.root, "picoclaw_chat_model_dropdown"), LV_STATE_DISABLED));
    assert(lv_obj_has_state(send, LV_STATE_DISABLED));

    lv_obj_send_event(settings_tab, LV_EVENT_CLICKED, nullptr);
    lv_obj_update_layout(context.root);
    assert(lv_obj_find_by_name(context.root, "picoclaw_settings_page"));
    assert(!lv_obj_find_by_name(context.root, "picoclaw_runtime_card"));
    assert(!lv_obj_find_by_name(context.root, "picoclaw_settings_host"));
    assert(!lv_obj_find_by_name(context.root, "picoclaw_settings_port"));
    assert(!lv_obj_find_by_name(context.root, "picoclaw_settings_public"));
    assert(!lv_obj_find_by_name(context.root, "picoclaw_settings_keyboard"));
    assert_settings_gateway_layout(context.root, width, height);
    assert(lv_obj_find_by_name(context.root, "picoclaw_install"));
    assert(!lv_obj_find_by_name(context.root, "picoclaw_output_card"));
    assert(!lv_obj_find_by_name(context.root, "picoclaw_logs"));
    assert(!lv_obj_find_by_name(context.root, "picoclaw_advanced_card"));
    assert(!lv_obj_find_by_name(context.root, "picoclaw_settings_path"));
    assert(!lv_obj_find_by_name(context.root, "picoclaw_settings_args"));

    auto *settings_scroll = lv_obj_find_by_name(context.root, "picoclaw_settings_scroll");
    auto *settings_grid   = lv_obj_find_by_name(context.root, "picoclaw_settings_grid");
    auto *save_settings  = lv_obj_find_by_name(context.root, "picoclaw_save_settings");
    assert(settings_scroll && settings_grid && save_settings);
    lv_area_t settings_viewport{};
    lv_obj_get_content_coords(settings_scroll, &settings_viewport);
    assert_horizontal_settings_layout(settings_grid, settings_viewport);
    assert_on_screen(save_settings, width, height);

    // Verify Settings focus feedback is complete at both ends of its clipped
    // scroll viewport, rather than only checking the control's geometry.
    auto *gateway_start = required_object(context.root, "picoclaw_gateway_start");
    lv_obj_add_state(gateway_start, LV_STATE_FOCUS_KEY);
    lv_refr_now(display);
    assert_focus_border_visible(gateway_start);
    lv_obj_remove_state(gateway_start, LV_STATE_FOCUS_KEY);

    auto *install_settings = required_object(context.root, "picoclaw_install");
    lv_obj_scroll_to_view_recursive(install_settings, LV_ANIM_OFF);
    lv_obj_update_layout(context.root);
    assert_on_screen(install_settings, width, height);
    lv_obj_add_state(install_settings, LV_STATE_FOCUSED);
    lv_refr_now(display);
    assert_focus_border_visible(install_settings);
    lv_obj_remove_state(install_settings, LV_STATE_FOCUSED);
    lv_obj_scroll_to_y(settings_scroll, 0, LV_ANIM_OFF);
    lv_obj_update_layout(context.root);

    // Theme controls were removed in the single-palette redesign. Check the
    // whole settings subtree so hidden legacy objects do not survive.
    assert(!lv_obj_find_by_name(context.root, "picoclaw_settings_theme"));
    assert(!lv_obj_find_by_name(context.root, "picoclaw_theme_selection"));
    assert(!lv_obj_find_by_name(context.root, "picoclaw_theme_CARBON"));
    assert(!lv_obj_find_by_name(context.root, "picoclaw_theme_SLATE"));
    assert(!lv_obj_find_by_name(context.root, "picoclaw_theme_OBSIDIAN"));
    assert(!lv_obj_find_by_name(context.root, "picoclaw_theme_EBONY"));
    assert(!lv_obj_find_by_name(context.root, "picoclaw_theme_NORD"));
    assert(!lv_obj_find_by_name(context.root, "picoclaw_theme_SAKURA"));

    auto *language = lv_obj_find_by_name(context.root, "picoclaw_settings_language");
    assert(language && lv_dropdown_get_option_count(language) == 2);
    assert(lv_obj_get_style_text_font(language, LV_PART_MAIN) != nullptr);
    const auto language_before = lv_dropdown_get_selected(language);
    lv_dropdown_set_selected(language, language_before == 0 ? 1 : 0);
    lv_obj_send_event(language, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_update_layout(context.root);
    lv_dropdown_set_selected(language, language_before);
    lv_obj_send_event(language, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_tick_inc(20);
    lv_refr_now(display);
    std::string settings_path   = argv[3];
    const std::size_t extension = settings_path.rfind(".ppm");
    settings_path.insert(extension == std::string::npos ? settings_path.size() : extension, "-settings");
    assert(write_ppm(settings_path.c_str(), width, height));

    // Refreshing runtime metadata must preserve an unsaved language selection.
    const auto edited_language = language_before == 0 ? 1U : 0U;
    lv_dropdown_set_selected(language, edited_language);
    lv_obj_send_event(language, LV_EVENT_VALUE_CHANGED, nullptr);
    auto assert_drafts_preserved = [&] {
        assert(lv_dropdown_get_selected(language) == edited_language);
    };
    auto *release = lv_obj_find_by_name(context.root, "picoclaw_release_dropdown");
    auto *check_updates = lv_obj_find_by_name(context.root, "picoclaw_check_updates");
    auto *install_release = lv_obj_find_by_name(context.root, "picoclaw_install");
    assert(release && check_updates && install_release);
    advance_ui();
    lv_dropdown_set_selected(release, 2);
    lv_obj_send_event(release, LV_EVENT_VALUE_CHANGED, nullptr);
    assert(lv_obj_has_state(install_release, LV_STATE_DISABLED));
    assert(std::string(lv_dropdown_get_options(release)).find("unavailable") != std::string::npos);
    lv_dropdown_set_selected(release, 0);
    lv_obj_send_event(release, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_send_event(check_updates, LV_EVENT_CLICKED, nullptr);
    assert(lv_obj_has_state(release, LV_STATE_DISABLED));
    assert(lv_obj_has_state(install_release, LV_STATE_DISABLED));
    assert(lv_obj_has_state(save_settings, LV_STATE_DISABLED));
    advance_ui();
    assert(lv_dropdown_get_selected(release) == 0);
    assert(!lv_obj_has_state(install_release, LV_STATE_DISABLED));
    assert_drafts_preserved();

    // Resizing preserves the language control and its unsaved selection.
    lv_obj_set_width(surface, width == 320 ? 568 : 320);
    lv_obj_update_layout(context.root);
    assert(lv_obj_find_by_name(context.root, "picoclaw_settings_language") == language);
    assert_drafts_preserved();
    lv_obj_set_width(surface, width);
    lv_obj_update_layout(context.root);
    assert_drafts_preserved();

    // Saving language settings stays reachable after scrolling and must never
    // rewrite saved models or submit their credentials to the runtime helper.
    const auto read_file = [](const std::string &path) {
        std::ifstream file(path);
        return std::string((std::istreambuf_iterator<char>(file)), {});
    };
    const auto models_before_save = read_file(environment.root + "/models.conf");
    const auto active_model_before_save = read_file(model_status);
    const auto commands_before_save = read_file(commands);
    assert(!models_before_save.empty() && !active_model_before_save.empty());
    lv_obj_scroll_to_y(settings_scroll,
                       lv_obj_get_scroll_y(settings_scroll) + lv_obj_get_scroll_bottom(settings_scroll), LV_ANIM_OFF);
    lv_obj_update_layout(context.root);
    assert_on_screen(save_settings, width, height);
    advance_ui();
    assert(!lv_obj_has_state(save_settings, LV_STATE_DISABLED));
    lv_obj_send_event(save_settings, LV_EVENT_CLICKED, nullptr);
    lv_obj_update_layout(context.root);
    {
        std::ifstream saved_settings(environment.root + "/ui-settings.conf");
        const std::string saved_text((std::istreambuf_iterator<char>(saved_settings)),
                                     std::istreambuf_iterator<char>());
        assert(saved_text.find(edited_language == 1U ? "language=zh\n" : "language=en\n") != std::string::npos);
    }
    advance_ui();
    assert(read_file(environment.root + "/models.conf") == models_before_save);
    assert(read_file(model_status) == active_model_before_save);
    assert(read_file(commands).substr(commands_before_save.size()).find("save-config") == std::string::npos);
    lv_dropdown_set_selected(language, language_before);
    lv_obj_send_event(language, LV_EVENT_VALUE_CHANGED, nullptr);
    wait_until_enabled(context.root, "picoclaw_save_settings");
    assert(!lv_obj_has_state(save_settings, LV_STATE_DISABLED));
    lv_obj_send_event(save_settings, LV_EVENT_CLICKED, nullptr);
    advance_ui();

    // Capture the runtime status and update groups after scrolling.
    auto *runtime_status = lv_obj_find_by_name(context.root, "picoclaw_runtime_status_card");
    assert(runtime_status);
    lv_area_t runtime_status_area{};
    lv_obj_get_coords(runtime_status, &runtime_status_area);
    lv_obj_get_content_coords(settings_scroll, &settings_viewport);
    lv_obj_scroll_to_y(settings_scroll,
                       lv_obj_get_scroll_y(settings_scroll) + runtime_status_area.y1 - settings_viewport.y1 - 24,
                       LV_ANIM_OFF);
    lv_refr_now(display);
    std::string settings_status_path = argv[3];
    settings_status_path.insert(settings_status_path.rfind(".ppm"), "-settings-status");
    assert(write_ppm(settings_status_path.c_str(), width, height));

    auto *updates_group = lv_obj_find_by_name(context.root, "picoclaw_updates_card");
    lv_area_t updates_area{};
    lv_obj_get_coords(updates_group, &updates_area);
    lv_obj_get_content_coords(settings_scroll, &settings_viewport);
    lv_obj_scroll_to_y(settings_scroll,
                      lv_obj_get_scroll_y(settings_scroll) + updates_area.y1 - settings_viewport.y1 - 24,
                      LV_ANIM_OFF);
    lv_refr_now(display);
    assert_horizontal_settings_layout(settings_grid, settings_viewport);
    std::string updates_path = argv[3];
    updates_path.insert(updates_path.rfind(".ppm"), "-updates");
    assert(write_ppm(updates_path.c_str(), width, height));

    auto *chat_tab = lv_obj_find_by_name(context.root, "picoclaw_chat_tab");
    assert(chat_tab);
    lv_obj_send_event(chat_tab, LV_EVENT_CLICKED, nullptr);
    lv_obj_update_layout(context.root);
    assert(lv_obj_find_by_name(context.root, "picoclaw_user_message"));

    lv_tick_inc(20);
    lv_refr_now(display);
    const bool has_contrast = std::any_of(framebuffer.cbegin() + 1, framebuffer.cend(),
                                          [](const auto &pixel) { return colors_differ(pixel, framebuffer[0]); });
    assert(has_contrast);
    assert(write_ppm(argv[3], width, height));

    // Wait for the request to finish before changing its active conversation.
    advance_ui();
    // A new conversation preserves the old one in the history selector.
    auto *new_chat_after_navigation = lv_obj_find_by_name(context.root, "picoclaw_new_chat");
    assert(new_chat_after_navigation);
    lv_obj_send_event(new_chat_after_navigation, LV_EVENT_CLICKED, nullptr);
    lv_obj_update_layout(context.root);
    assert(!lv_obj_find_by_name(context.root, "picoclaw_user_message"));
    assert(lv_obj_find_by_name(context.root, "picoclaw_empty_state"));

    auto *history = lv_obj_find_by_name(context.root, "picoclaw_history");
    assert(history);
    assert(lv_dropdown_get_option_count(history) == 2);
    assert(std::string(lv_dropdown_get_options(history)).find("Hello PicoClaw") != std::string::npos);
    lv_dropdown_set_selected(history, 0);
    lv_obj_send_event(history, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_update_layout(context.root);
    assert(lv_obj_find_by_name(context.root, "picoclaw_user_message"));
    assert(!lv_obj_find_by_name(context.root, "picoclaw_empty_state"));
    lv_dropdown_open(history);
    lv_obj_update_layout(context.root);
    assert_on_screen(lv_dropdown_get_list(history), width, height);
    lv_tick_inc(20);
    lv_refr_now(display);
    std::string history_path = argv[3];
    history_path.insert(history_path.rfind(".ppm"), "-history");
    assert(write_ppm(history_path.c_str(), width, height));
    lv_dropdown_close(history);

    // Switching a model in an existing conversation preserves its transcript,
    // current history selection, scroll position and unsent message contents.
    input = required_object(context.root, "picoclaw_message_input");
    conversation = required_object(context.root, "picoclaw_conversation");
    lv_obj_scroll_to_y(conversation, 0, LV_ANIM_OFF);
    lv_textarea_set_text(input, "Continue this conversation after switching models.");
    choose_chat_model(context.root, "Ollama Llama 3.2");
    assert(lv_obj_find_by_name(context.root, "picoclaw_user_message"));
    assert_chat_model_layout(context.root, width, height);
    choose_chat_model(context.root, "DeepSeek Chat");
    lv_textarea_set_text(input, "");

    // Opening the keyboard must keep the end of a long reply inside the
    // resized viewport, with the composer and keyboard fully on screen.
    input          = lv_obj_find_by_name(context.root, "picoclaw_message_input");
    conversation   = lv_obj_find_by_name(context.root, "picoclaw_conversation");
    auto *keyboard = lv_obj_find_by_name(context.root, "picoclaw_keyboard");
    auto *composer = lv_obj_find_by_name(context.root, "picoclaw_composer");
    assert(input && conversation && keyboard && composer);
    const auto height_without_keyboard = lv_obj_get_height(conversation);
    for (int attempt = 0; attempt < 2; ++attempt) {
        lv_obj_scroll_to_y(conversation, 0, LV_ANIM_OFF);
        lv_obj_send_event(input, LV_EVENT_FOCUSED, nullptr);
        lv_obj_update_layout(context.root);
        assert(!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
        lv_area_t viewport{}, latest{}, composer_area{}, keyboard_area{}, navigation_area{};
        lv_obj_get_content_coords(conversation, &viewport);
        lv_obj_get_coords(lv_obj_get_child(conversation, -1), &latest);
        lv_obj_get_coords(composer, &composer_area);
        lv_obj_get_coords(keyboard, &keyboard_area);
        lv_obj_get_coords(bottom_nav, &navigation_area);
        auto *history = required_object(context.root, "picoclaw_history");
        auto *chat_model = required_object(context.root, "picoclaw_chat_model_dropdown");
        auto *new_chat = required_object(context.root, "picoclaw_new_chat");
        assert(!lv_obj_has_flag(bottom_nav, LV_OBJ_FLAG_HIDDEN));
        if (height < 700) {
            assert(lv_obj_has_flag(history, LV_OBJ_FLAG_HIDDEN));
            assert(!lv_obj_has_flag(chat_model, LV_OBJ_FLAG_HIDDEN));
            assert(!lv_obj_has_flag(new_chat, LV_OBJ_FLAG_HIDDEN));
            assert_on_screen(chat_model, width, height);
            assert_on_screen(new_chat, width, height);
        }
        assert_on_screen(bottom_nav, width, height);
        assert(lv_obj_get_height(conversation) < height_without_keyboard);
        assert(lv_obj_get_height(conversation) >= 64);
        assert(latest.y2 <= viewport.y2 && latest.y2 >= viewport.y1);
        assert(viewport.y2 < composer_area.y1);
        assert(composer_area.y2 < keyboard_area.y1);
        assert(keyboard_area.y2 < height);
        if (lv_obj_get_width(bottom_nav) > lv_obj_get_height(bottom_nav))
            assert(keyboard_area.y2 < navigation_area.y1);
        lv_tick_inc(20);
        lv_refr_now(display);
        std::string keyboard_path = argv[3];
        keyboard_path.insert(keyboard_path.rfind(".ppm"), "-keyboard");
        assert(write_ppm(keyboard_path.c_str(), width, height));
        drag_to_earlier_messages(pointer, conversation);
        lv_obj_send_event(keyboard, LV_EVENT_CANCEL, nullptr);
        lv_obj_update_layout(context.root);
        assert(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
        assert(!lv_obj_has_flag(history, LV_OBJ_FLAG_HIDDEN));
        assert(lv_obj_get_height(conversation) == height_without_keyboard);
        lv_obj_scroll_to_y(conversation, lv_obj_get_scroll_y(conversation) + lv_obj_get_scroll_bottom(conversation),
                           LV_ANIM_OFF);
        drag_to_earlier_messages(pointer, conversation);
    }

    descriptor->close();
    lv_indev_delete(pointer);
    lv_display_delete(display);
    lilygo_ui_fonts_deinit();
    lv_deinit();
    return 0;
}
