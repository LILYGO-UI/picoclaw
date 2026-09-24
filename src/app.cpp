#include "app.hpp"

#include "app_identity.h"
#include "domain/picoclaw_client.hpp"
#include "pages/picoclaw/picoclaw_view.hpp"

#include <memory>

namespace {

struct AppState {
    std::unique_ptr<PicoClawClient> client;
    std::unique_ptr<PicoClawViewModel> view_model;
    std::unique_ptr<PicoClawView> view;
};

AppState app_state;

void app_open(const cm0_app_context_t *context)
{
    app_state.client     = std::make_unique<PicoClawClient>();
    app_state.view_model = std::make_unique<PicoClawViewModel>(*app_state.client);
    app_state.view       = std::make_unique<PicoClawView>(*app_state.view_model);
    app_state.view->create(context->root, LILYGO_UI_PICOCLAW_APP_NAME);
}

void app_close()
{
    if (app_state.view) app_state.view->destroy();
    app_state.view.reset();
    app_state.view_model.reset();
    app_state.client.reset();
}

const cm0_app_descriptor_t app_descriptor{
    CM0_APP_API_VERSION, sizeof(cm0_app_descriptor_t), LILYGO_UI_PICOCLAW_APP_ID, app_open, app_close,
};

}  // namespace

extern "C" const cm0_app_descriptor_t *cm0_app_get_descriptor()
{
    return &app_descriptor;
}
