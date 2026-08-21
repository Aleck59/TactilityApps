#include "ThermalCamera.h"

#include <cstdlib>
#include <new>

#include <app/event.h>
#include <app/manager.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>

extern "C" {

int main(int /*argc*/, char* /*argv*/[]) {
    const AppInstanceId app_instance_id = app_scheduler_current_app_id();

    // Placement new on a malloc'd block rather than a plain `new`: the camera is
    // about 12 kB (the sensor calibration alone is 9 kB) so it cannot live in this
    // task's stack frame, and the ELF loader resolves only part of the operator
    // new/delete family, while malloc and free are always available.
    void* storage = malloc(sizeof(ThermalCamera));
    if (storage == nullptr) {
        return 1;
    }
    auto* camera = new (storage) ThermalCamera();

    camera->start(app_instance_id);

    struct AppEventSubscription subscription {};
    subscription.app_instance_id = app_instance_id;
    app_event_subscribe(&subscription);

    // The window manager calls back into thermalCameraCreateWidgets() whenever this
    // app owns the screen, which can happen more than once during a single run.
    const WindowId window = window_manager_create(app_instance_id, thermalCameraCreateWidgets, camera);

    bool should_close = false;
    while (!should_close) {
        struct AppEvent event;
        if (app_event_await(&subscription, &event, portMAX_DELAY) != ERROR_NONE) {
            break;
        }
        if (event.type == APP_EVENT_CLOSE) {
            should_close = true;
        }
    }

    window_manager_remove(window);
    app_event_unsubscribe(&subscription);
    camera->stop();

    camera->~ThermalCamera();
    free(storage);

    return 0;
}

}
