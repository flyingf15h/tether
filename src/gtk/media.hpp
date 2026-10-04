#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <vector>

namespace tether::ui {

    // A message that is only a link to a picture or GIF (GIPHY, Tenor, catbox.moe,
    // or anything ending in an image extension).
    bool is_media_url(const std::string& text);

    // Shows the picture behind `url` in place of a text bubble: a placeholder at
    // first, the image (animated for GIFs) once it has downloaded. Clicking opens
    // the link. Falls back to the link text if the download fails.
    GtkWidget* media_bubble_new(const std::string& url);

    // Every http(s) link in a message, in order, without trailing punctuation.
    std::vector<std::string> extract_urls(const std::string& text);

    // The picture to show for a link: itself for image/GIF links, the GIF behind
    // a GIPHY page link, empty for anything else.
    std::string media_url_for(const std::string& url);

    // A card with the page's image, title, description and site, read from its
    // Open Graph tags, like iMessage and Discord show under a link. Hidden until
    // the page answers, and left hidden if it offers nothing to preview.
    GtkWidget* link_preview_new(const std::string& url);

    // Discord-style GIF search from GIPHY, anchored to `relative_to`. Asks for a
    // GIPHY API key the first time and keeps it in gtk.json.
    void gif_picker_open(GtkWidget* relative_to, std::function<void(const std::string& url)> on_pick);

} // namespace tether::ui
