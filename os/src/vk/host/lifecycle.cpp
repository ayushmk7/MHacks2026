#include "lifecycle.h"

#include "../vk.h"

namespace vk::host {

void onAppStopping(const String &appId) {
  for (AppStopListener *l = AppStopListener::first(); l; l = l->next()) {
    if (l->fn) l->fn(appId.c_str());
  }
}

bool luaPaused() { return vk::modalActive(); }

}  // namespace vk::host
