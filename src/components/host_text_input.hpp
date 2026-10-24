#ifndef LILYGO_UI_PICOCLAW_COMPONENTS_HOST_TEXT_INPUT_HPP
#define LILYGO_UI_PICOCLAW_COMPONENTS_HOST_TEXT_INPUT_HPP

#include <lvgl.h>

#if defined(CM0_APP_SIMULATOR)
#include <SDL.h>
#include <mutex>
#include <vector>
#endif

class HostTextInput {
public:
    HostTextInput() = default;
    ~HostTextInput();
    HostTextInput(const HostTextInput &)            = delete;
    HostTextInput &operator=(const HostTextInput &) = delete;

    void focus(lv_obj_t *input);

private:
#if defined(CM0_APP_SIMULATOR)
    static int watch_event(void *context, SDL_Event *event);
    static void poll_events(lv_timer_t *timer);
    void handle_key(const SDL_KeyboardEvent &event);
    bool erase_selection();
    void copy_selection(bool cut);
    void move_cursor(int position, bool selecting);

    lv_obj_t *input_   = nullptr;
    lv_timer_t *timer_ = nullptr;
    std::mutex mutex_;
    std::vector<SDL_Event> events_;
    Uint32 window_id_ = 0;
    bool watching_    = false;
#endif
};

#endif
