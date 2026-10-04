#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>

namespace tether::ui {

    // A message that is only a link to a picture or GIF (GIPHY, Tenor, catbox.moe,
    // or anything ending in an image extension).
    bool is_media_url(const std::string& text);

    // Shows the picture behind `url` in place of a text bubble: a placeholder at
    // first, the image (animated for GIFs) once it has downloaded. Clicking opens
    // the link. Falls back to the link text if the download fails.
    GtkWidget* media_bubble_new(const std::string& url);

    // Discord-style GIF search from GIPHY, anchored to `relative_to`. Asks for a
    // GIPHY API key the first time and keeps it in gtk.json.
    void gif_picker_open(GtkWidget* relative_to, std::function<void(const std::string& url)> on_pick);

} // namespace tether::ui
