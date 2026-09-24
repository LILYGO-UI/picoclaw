#ifndef LILYGO_UI_PICOCLAW_PAGES_PICOCLAW_VIEW_HPP
#define LILYGO_UI_PICOCLAW_PAGES_PICOCLAW_VIEW_HPP

#include "pages/picoclaw/picoclaw_view_model.hpp"

#include <lvgl.h>

#include <cstdint>

class PicoClawView {
public:
    explicit PicoClawView(PicoClawViewModel &view_model) noexcept;

    lv_obj_t *create(lv_obj_t *parent, const char *application_name);
    void destroy() noexcept;

private:
    static void chat_tab_clicked(lv_event_t *event);
    static void header_back_clicked(lv_event_t *event);
    static void models_tab_clicked(lv_event_t *event);
    static void channels_tab_clicked(lv_event_t *event);
    static void channel_edit_clicked(lv_event_t *event);
    static void channel_field_changed(lv_event_t *event);
    static void channel_enabled_changed(lv_event_t *event);
    static void channel_save_clicked(lv_event_t *event);
    static void channels_refresh_clicked(lv_event_t *event);
    static void channel_secrets_clicked(lv_event_t *event);
    static void weixin_bind_clicked(lv_event_t *event);
    static void weixin_cancel_clicked(lv_event_t *event);
    static void layout_weixin_async(void *user_data);
    static void rebuild_channels_async(void *user_data);
    static void model_add_clicked(lv_event_t *event);
    static void model_import_clicked(lv_event_t *event);
    static void model_edit_clicked(lv_event_t *event);
    static void model_editor_name_changed(lv_event_t *event);
    static void model_editor_selection_changed(lv_event_t *event);
    static void model_editor_input_changed(lv_event_t *event);
    static void model_editor_dropdown_opened(lv_event_t *event);
    static void model_save_clicked(lv_event_t *event);
    static void model_swipe_event(lv_event_t *event);
    static void model_swipe_delete_clicked(lv_event_t *event);
    static void models_scroll_started(lv_event_t *event);
    static void model_key_toggle_clicked(lv_event_t *event);
    static void rebuild_models_async(void *user_data);
    static void gateway_refresh_clicked(lv_event_t *event);
    static void settings_logs_clicked(lv_event_t *event);
    static void rebuild_screen_async(void *user_data);
    static void settings_tab_clicked(lv_event_t *event);
    static void send_clicked(lv_event_t *event);
    static void new_chat_clicked(lv_event_t *event);
    static void conversation_changed(lv_event_t *event);
    static void chat_model_changed(lv_event_t *event);
    static void chat_model_opened(lv_event_t *event);
    static void earlier_messages_clicked(lv_event_t *event);
    static void quick_prompt_clicked(lv_event_t *event);
    static void input_focused(lv_event_t *event);
    static void keyboard_finished(lv_event_t *event);
    static void test_connection_clicked(lv_event_t *event);
    static void model_status_refresh_clicked(lv_event_t *event);
    static void fetch_models_clicked(lv_event_t *event);
    static void save_clicked(lv_event_t *event);
    static void settings_language_changed(lv_event_t *event);
    static void install_clicked(lv_event_t *event);
    static void check_updates_clicked(lv_event_t *event);
    static void release_changed(lv_event_t *event);
    static void gateway_start_clicked(lv_event_t *event);
    static void gateway_stop_clicked(lv_event_t *event);
    static void logs_clicked(lv_event_t *event);
    static void size_changed(lv_event_t *event);
    static void timer_tick(lv_timer_t *timer);

    void build_screen();
    void build_chat();
    void build_models();
    void build_channels();
    void build_channel_editor();
    void build_weixin_binding();
    void sync_weixin_binding();
    void reveal_weixin_qr();
    void sync_channels();
    void build_model_editor();
    void update_model_editor_dropdown();
    void queue_models_rebuild();
    void settle_model_swipe(lv_obj_t *row, bool open);
    void build_logs();
    void build_settings();
    void clear_screen_pointers() noexcept;
    void rebuild_messages();
    void fill_settings_fields();
    void update_provider_statuses();
    void send_message();
    void show_keyboard(lv_obj_t *textarea, bool chat_input);
    void hide_keyboard();
    void sync_state(bool force = false);
    void update_navigation();
    void apply_layout() noexcept;

    PicoClawViewModel &view_model_;
    lv_obj_t *surface_                 = nullptr;
    lv_obj_t *main_column_             = nullptr;
    lv_obj_t *header_                  = nullptr;
    lv_obj_t *title_                   = nullptr;
    lv_obj_t *header_back_             = nullptr;
    lv_obj_t *header_status_           = nullptr;
    lv_obj_t *chat_tab_                = nullptr;
    lv_obj_t *models_tab_              = nullptr;
    lv_obj_t *channels_tab_            = nullptr;
    lv_obj_t *settings_tab_            = nullptr;
    lv_obj_t *content_                 = nullptr;
    lv_obj_t *bottom_navigation_       = nullptr;
    lv_obj_t *bottom_chat_button_      = nullptr;
    lv_obj_t *bottom_models_button_    = nullptr;
    lv_obj_t *bottom_channels_button_  = nullptr;
    lv_obj_t *bottom_settings_button_  = nullptr;

    lv_obj_t *chat_shell_              = nullptr;
    lv_obj_t *status_card_             = nullptr;
    lv_obj_t *status_label_            = nullptr;
    lv_obj_t *connection_badge_        = nullptr;
    lv_obj_t *profile_label_           = nullptr;
    lv_obj_t *runtime_summary_label_   = nullptr;
    lv_obj_t *conversation_            = nullptr;
    lv_obj_t *composer_                = nullptr;
    lv_obj_t *message_input_           = nullptr;
    lv_obj_t *send_button_             = nullptr;
    lv_obj_t *new_chat_button_         = nullptr;
    lv_obj_t *conversation_dropdown_   = nullptr;
    lv_obj_t *chat_model_row_          = nullptr;
    lv_obj_t *chat_model_dropdown_     = nullptr;
    std::string chat_model_display_text_;

    lv_obj_t *models_shell_             = nullptr;
    lv_obj_t *models_scroll_            = nullptr;
    lv_obj_t *models_list_              = nullptr;
    lv_obj_t *models_import_button_     = nullptr;
    lv_obj_t *models_feedback_label_    = nullptr;
    lv_obj_t *model_editor_id_           = nullptr;
    lv_obj_t *model_editor_base_         = nullptr;
    lv_obj_t *model_editor_key_          = nullptr;
    lv_obj_t *model_editor_provider_     = nullptr;
    lv_obj_t *model_editor_dropdown_     = nullptr;
    lv_obj_t *model_editor_test_         = nullptr;
    lv_obj_t *model_editor_fetch_        = nullptr;
    std::vector<std::string> model_editor_options_;
    lv_obj_t *models_swipe_row_          = nullptr;
    bool models_adjusting_swipe_        = false;
    bool models_swipe_confirm_          = false;
    std::int32_t models_restore_scroll_y_ = -1;
    bool models_reveal_saved_          = false;
    bool models_editing_                = false;
    bool models_adding_                 = false;
    bool models_feedback_visible_       = false;
    std::size_t models_edit_index_       = 0;
    std::size_t models_provider_index_   = 0;
    std::size_t models_rendered_count_   = 0;
    std::size_t visible_message_count_ = PicoClawModel::maximum_messages;

    lv_obj_t *channels_shell_          = nullptr;
    lv_obj_t *channels_scroll_         = nullptr;
    lv_obj_t *channels_list_           = nullptr;
    lv_obj_t *channels_feedback_       = nullptr;
    lv_obj_t *channels_refresh_        = nullptr;
    lv_obj_t *channels_summary_        = nullptr;
    lv_obj_t *channel_enabled_         = nullptr;
    std::vector<lv_obj_t *> channel_fields_;
    std::string channels_rendered_ids_;
    bool channel_show_secrets_ = false;
    lv_obj_t *weixin_binding_ = nullptr;
    lv_obj_t *weixin_bind_ = nullptr;
    lv_obj_t *weixin_cancel_ = nullptr;
    lv_obj_t *weixin_qr_ = nullptr;
    lv_obj_t *weixin_status_ = nullptr;
    lv_obj_t *weixin_account_ = nullptr;
    std::string weixin_rendered_qr_;

    lv_obj_t *settings_shell_             = nullptr;
    lv_obj_t *settings_scroll_            = nullptr;
    lv_obj_t *settings_grid_              = nullptr;
    lv_obj_t *save_button_                = nullptr;
    lv_obj_t *runtime_state_label_        = nullptr;
    lv_obj_t *runtime_network_label_      = nullptr;
    lv_obj_t *runtime_config_label_       = nullptr;
    lv_obj_t *runtime_key_label_          = nullptr;
    lv_obj_t *runtime_service_label_      = nullptr;
    lv_obj_t *settings_feedback_label_    = nullptr;
    lv_obj_t *runtime_version_label_      = nullptr;
    lv_obj_t *runtime_gateway_label_      = nullptr;
    lv_obj_t *settings_language_dropdown_ = nullptr;
    lv_obj_t *settings_about_label_       = nullptr;
    lv_obj_t *install_button_             = nullptr;
    lv_obj_t *updates_card_               = nullptr;
    lv_obj_t *update_current_label_       = nullptr;
    lv_obj_t *update_message_label_       = nullptr;
    lv_obj_t *release_dropdown_           = nullptr;
    lv_obj_t *release_detail_label_       = nullptr;
    lv_obj_t *check_updates_button_       = nullptr;
    lv_obj_t *gateway_start_button_       = nullptr;
    lv_obj_t *gateway_stop_button_        = nullptr;
    lv_obj_t *gateway_refresh_button_     = nullptr;
    lv_obj_t *logs_button_                = nullptr;
    lv_obj_t *output_label_               = nullptr;
    lv_obj_t *keyboard_                   = nullptr;

    lv_timer_t *timer_                       = nullptr;
    std::uint64_t rendered_revision_         = 0;
    std::uint64_t rendered_message_revision_ = 0;
    bool keyboard_is_chat_                   = false;
    bool compact_layout_                     = false;
    bool applying_layout_                    = false;
    bool rendered_busy_                      = false;
};

#endif
