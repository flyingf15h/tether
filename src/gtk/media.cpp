#include "media.hpp"
#include "prefs.hpp"
#include "ui_util.hpp"

#include <tether/i18n.hpp>

#include <glib/gstdio.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <map>
#include <regex>
#include <cctype>
#include <memory>
#include <vector>

namespace tether::ui {

    namespace {

        constexpr int MEDIA_MAX_WIDTH = 260;
        constexpr const char* GIPHY_API = "https://api.giphy.com/v1/gifs/";
        constexpr const char* GIPHY_KEY_PREF = "giphy_api_key";

        std::string lower(std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
            return text;
        }

        std::string cache_dir() {
            const std::string dir = std::string(g_get_user_cache_dir()) + "/tether/media";
            g_mkdir_with_parents(dir.c_str(), 0700);
            return dir;
        }

        std::string cache_path(const std::string& url) {
            gchar* digest = g_compute_checksum_for_string(G_CHECKSUM_SHA1, url.c_str(), -1);
            const std::string path = cache_dir() + "/" + digest;
            g_free(digest);
            return path;
        }

        struct Fetch {
            std::string url;
            std::string path;
            std::string tmp;
            std::function<void(bool ok, const std::string& path, const std::string& body)> done;
            bool to_memory = false;
            std::string agent;
        };

        constexpr const char* BROWSER_AGENT =
            "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126 Safari/537.36";
        // What link previewers identify as; X and Instagram only serve their preview
        // tags to it, and to nothing that looks like a browser.
        constexpr const char* PREVIEW_AGENT = "facebookexternalhit/1.1";

        // Opening a long conversation can ask for dozens of pictures at once; a
        // few at a time keeps the machine and the network calm.
        constexpr int MAX_FETCHES = 4;
        int g_running = 0;
        std::deque<Fetch*> g_waiting;

        void start(Fetch* job);

        void next_fetch() {
            while (g_running < MAX_FETCHES && !g_waiting.empty()) {
                Fetch* job = g_waiting.front();
                g_waiting.pop_front();
                start(job);
            }
        }

        void on_fetch_done(GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<Fetch> fetch(static_cast<Fetch*>(data));
            GSubprocess* proc = G_SUBPROCESS(source);
            GBytes* out = nullptr;
            GError* error = nullptr;
            const bool ran = g_subprocess_communicate_finish(proc, result, &out, nullptr, &error);
            const bool ok = ran && g_subprocess_get_successful(proc);
            std::string body;
            if (out) {
                gsize size = 0;
                const char* bytes = static_cast<const char*>(g_bytes_get_data(out, &size));
                body.assign(bytes ? bytes : "", size);
                g_bytes_unref(out);
            }
            g_clear_error(&error);
            g_object_unref(proc);
            if (!fetch->to_memory) {
                if (ok)
                    g_rename(fetch->tmp.c_str(), fetch->path.c_str());
                else
                    g_remove(fetch->tmp.c_str());
            }
            --g_running;
            fetch->done(ok, fetch->path, body);
            next_fetch();
        }

        void start(Fetch* job) {
            // A browser's user agent: plenty of sites hide their preview tags from curl.
            std::vector<const gchar*> argv = {"curl", "-sSL", "--fail", "--max-time", "20", "--compressed",
                                              "-A", job->agent.empty() ? BROWSER_AGENT : job->agent.c_str(),
                                              "--max-filesize", "26214400"};
            if (!job->to_memory) {
                job->tmp = job->path + ".part";
                argv.push_back("-o");
                argv.push_back(job->tmp.c_str());
            }
            argv.push_back(job->url.c_str());
            argv.push_back(nullptr);
            GError* error = nullptr;
            GSubprocess* proc = g_subprocess_newv(
                argv.data(), GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE), &error);
            if (!proc) {
                g_clear_error(&error);
                std::unique_ptr<Fetch> owned(job);
                owned->done(false, owned->path, "");
                return;
            }
            ++g_running;
            g_subprocess_communicate_async(proc, nullptr, nullptr, on_fetch_done, job);
        }

        // curl in the background: into the media cache, or into memory for pages and API calls.
        void fetch(const std::string& url,
                   bool to_memory,
                   std::function<void(bool ok, const std::string& path, const std::string& body)> done,
                   const std::string& agent = "") {
            auto* job = new Fetch{url, to_memory ? "" : cache_path(url), "", std::move(done), to_memory, agent};
            if (!to_memory && g_file_test(job->path.c_str(), G_FILE_TEST_EXISTS)) {
                std::unique_ptr<Fetch> owned(job);
                owned->done(true, owned->path, "");
                return;
            }
            g_waiting.push_back(job);
            next_fetch();
        }

        // giphy.com/gifs/some-title-ID is a web page; its GIF lives at media.giphy.com.
        std::string giphy_page_gif(const std::string& url) {
            const std::string l = lower(url);
            const size_t at = l.find("giphy.com/gifs/");
            if (at == std::string::npos)
                return {};
            std::string slug = url.substr(at + strlen("giphy.com/gifs/"));
            slug = slug.substr(0, slug.find_first_of("/?#"));
            const size_t dash = slug.rfind('-');
            const std::string id = dash == std::string::npos ? slug : slug.substr(dash + 1);
            return id.empty() ? std::string{} : "https://media.giphy.com/media/" + id + "/giphy.gif";
        }

        // GIPHY serves a 200px-wide copy of every GIF, much lighter in a chat.
        std::string display_url(const std::string& url) {
            const std::string l = lower(url);
            if (l.find("giphy.com/media/") != std::string::npos) {
                const size_t slash = url.rfind('/');
                if (slash != std::string::npos)
                    return url.substr(0, slash) + "/200w.gif";
            }
            return url;
        }

        // Fills `image` from a downloaded file: animated if it is a GIF, scaled otherwise.
        bool show_file(GtkWidget* image, const std::string& path) {
            GdkPixbufAnimation* animation = gdk_pixbuf_animation_new_from_file(path.c_str(), nullptr);
            if (!animation)
                return false;
            if (!gdk_pixbuf_animation_is_static_image(animation) &&
                gdk_pixbuf_animation_get_width(animation) <= MEDIA_MAX_WIDTH * 2) {
                gtk_image_set_from_animation(GTK_IMAGE(image), animation);
            } else {
                GdkPixbuf* still = gdk_pixbuf_animation_get_static_image(animation);
                const int w = gdk_pixbuf_get_width(still), h = gdk_pixbuf_get_height(still);
                const double scale = w > MEDIA_MAX_WIDTH ? double(MEDIA_MAX_WIDTH) / w : 1.0;
                GdkPixbuf* scaled = gdk_pixbuf_scale_simple(
                    still, std::max(1, int(w * scale)), std::max(1, int(h * scale)), GDK_INTERP_BILINEAR);
                gtk_image_set_from_pixbuf(GTK_IMAGE(image), scaled);
                g_object_unref(scaled);
            }
            g_object_unref(animation);
            return true;
        }

        gboolean open_link(GtkWidget* widget, GdkEventButton* event, gpointer) {
            if (event->button != 1)
                return FALSE;
            const char* url = static_cast<const char*>(g_object_get_data(G_OBJECT(widget), "url"));
            GtkWidget* toplevel = gtk_widget_get_toplevel(widget);
            if (url)
                gtk_show_uri_on_window(GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : nullptr, url, GDK_CURRENT_TIME, nullptr);
            return TRUE;
        }

    } // namespace

    bool is_media_url(const std::string& text) {
        if (text.find_first_of(" \n\t") != std::string::npos || text.rfind("https://", 0) != 0)
            return false;
        const std::string l = lower(text);
        const std::string path = l.substr(0, l.find('?'));
        for (const char* ext : {".gif", ".png", ".jpg", ".jpeg", ".webp"})
            if (path.size() > strlen(ext) && path.compare(path.size() - strlen(ext), strlen(ext), ext) == 0)
                return true;
        return l.find("giphy.com/media/") != std::string::npos || l.rfind("https://i.giphy.com/", 0) == 0 ||
               l.find(".tenor.com/") != std::string::npos;
    }

    GtkWidget* media_bubble_new(const std::string& url) {
        GtkWidget* box = gtk_event_box_new();
        gtk_style_context_add_class(gtk_widget_get_style_context(box), "tether-media");
        g_object_set_data_full(G_OBJECT(box), "url", g_strdup(url.c_str()), g_free);
        gtk_widget_set_tooltip_text(box, url.c_str());
        g_signal_connect(box, "button-press-event", G_CALLBACK(open_link), nullptr);

        GtkWidget* image = gtk_image_new_from_icon_name("image-loading-symbolic", GTK_ICON_SIZE_DIALOG);
        gtk_widget_set_size_request(image, 120, 80);
        gtk_container_add(GTK_CONTAINER(box), image);

        // The row may be gone (conversation switched) before the download lands.
        GWeakRef* weak = g_new0(GWeakRef, 1);
        g_weak_ref_init(weak, image);
        fetch(display_url(url), false, [weak, url](bool ok, const std::string& path, const std::string&) {
            GtkWidget* image = GTK_WIDGET(g_weak_ref_get(weak));
            g_weak_ref_clear(weak);
            g_free(weak);
            if (!image)
                return;
            if (!ok || !show_file(image, path)) {
                gtk_image_set_from_icon_name(GTK_IMAGE(image), "image-missing-symbolic", GTK_ICON_SIZE_DIALOG);
            } else {
                gtk_widget_set_size_request(image, -1, -1);
            }
            g_object_unref(image);
        });
        return box;
    }

    std::vector<std::string> extract_urls(const std::string& text) {
        std::vector<std::string> urls;
        size_t at = 0;
        while ((at = text.find("http", at)) != std::string::npos) {
            if (text.compare(at, 8, "https://") != 0 && text.compare(at, 7, "http://") != 0) {
                at += 4;
                continue;
            }
            // Must start a word, so "xhttp://" inside other text is not a link.
            if (at > 0 && !std::isspace(static_cast<unsigned char>(text[at - 1])) && text[at - 1] != '(') {
                at += 4;
                continue;
            }
            size_t end = at;
            while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])))
                ++end;
            std::string url = text.substr(at, end - at);
            // Sentence punctuation and quotes around a link are not part of it.
            while (!url.empty() && std::strchr(".,;:!?)]}'\"", url.back()))
                url.pop_back();
            if (url.size() > 10)
                urls.push_back(url);
            at = end;
        }
        return urls;
    }

    std::string media_url_for(const std::string& url) {
        if (is_media_url(url))
            return url;
        return giphy_page_gif(url);
    }

    namespace {

        // Named entities pages actually use, plus numeric ones (&#8217; &#x1f300;),
        // which is how many sites write emoji and curly quotes in their titles.
        std::string html_unescape(const std::string& text) {
            static const std::pair<const char*, const char*> named[] = {
                {"amp", "&"}, {"quot", "\""}, {"apos", "'"}, {"lt", "<"}, {"gt", ">"}, {"nbsp", " "},
                {"hellip", "\u2026"}, {"mdash", "\u2014"}, {"ndash", "\u2013"}, {"rsquo", "\u2019"},
                {"lsquo", "\u2018"}, {"rdquo", "\u201d"}, {"ldquo", "\u201c"}, {"middot", "\u00b7"}};
            std::string out;
            out.reserve(text.size());
            for (size_t i = 0; i < text.size();) {
                if (text[i] != '&') {
                    out += text[i++];
                    continue;
                }
                const size_t semi = text.find(';', i);
                if (semi == std::string::npos || semi - i > 12) {
                    out += text[i++];
                    continue;
                }
                const std::string entity = text.substr(i + 1, semi - i - 1);
                std::string decoded;
                if (!entity.empty() && entity[0] == '#') {
                    const bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
                    const std::string digits = entity.substr(hex ? 2 : 1);
                    char* end = nullptr;
                    const unsigned long code = std::strtoul(digits.c_str(), &end, hex ? 16 : 10);
                    if (!digits.empty() && end && *end == '\0' && code > 0 && g_unichar_validate(code)) {
                        char buf[8] = {0};
                        decoded.assign(buf, g_unichar_to_utf8(static_cast<gunichar>(code), buf));
                    }
                } else {
                    for (const auto& [name, value] : named)
                        if (entity == name)
                            decoded = value;
                }
                if (decoded.empty()) {
                    out += text[i++];
                    continue;
                }
                out += decoded;
                i = semi + 1;
            }
            return out;
        }

        // The value of `name="…"` (or single quotes) inside one tag.
        std::string attribute(const std::string& tag, const char* name) {
            const std::string l = lower(tag);
            size_t at = 0;
            const std::string key = std::string(name) + "=";
            while ((at = l.find(key, at)) != std::string::npos) {
                if (at > 0 && !std::isspace(static_cast<unsigned char>(l[at - 1]))) {
                    at += key.size();
                    continue;
                }
                size_t start = at + key.size();
                if (start >= tag.size())
                    return {};
                const char quote = tag[start];
                if (quote == '"' || quote == '\'') {
                    const size_t end = tag.find(quote, start + 1);
                    return end == std::string::npos ? std::string{} : tag.substr(start + 1, end - start - 1);
                }
                const size_t end = tag.find_first_of(" \t\n/>", start);
                return tag.substr(start, end - start);
            }
            return {};
        }

        struct Preview {
            std::string title;
            std::string description;
            std::string image;
            std::string site;
            bool done = false;
            bool ok = false;
        };

        // Open Graph first, then Twitter cards, then the page title.
        Preview parse_preview(const std::string& html, const std::string& url) {
            Preview p;
            const std::string head = html.substr(0, std::min<size_t>(html.size(), 400000));
            const std::string l = lower(head);
            std::map<std::string, std::string> meta;
            for (size_t at = 0; (at = l.find("<meta", at)) != std::string::npos;) {
                const size_t end = head.find('>', at);
                if (end == std::string::npos)
                    break;
                const std::string tag = head.substr(at, end - at);
                std::string key = lower(attribute(tag, "property"));
                if (key.empty())
                    key = lower(attribute(tag, "name"));
                const std::string content = attribute(tag, "content");
                if (!key.empty() && !content.empty() && !meta.count(key))
                    meta[key] = html_unescape(content);
                at = end;
            }
            auto first = [&](std::initializer_list<const char*> keys) {
                for (const char* k : keys)
                    if (auto it = meta.find(k); it != meta.end())
                        return it->second;
                return std::string{};
            };
            p.title = first({"og:title", "twitter:title"});
            if (p.title.empty()) {
                const size_t t = l.find("<title");
                const size_t gt = t == std::string::npos ? t : head.find('>', t);
                const size_t close = gt == std::string::npos ? gt : l.find("</title", gt);
                if (close != std::string::npos)
                    p.title = html_unescape(head.substr(gt + 1, close - gt - 1));
            }
            p.description = first({"og:description", "twitter:description", "description"});
            p.image = first({"og:image:secure_url", "og:image", "twitter:image", "twitter:image:src"});
            p.site = first({"og:site_name"});

            // Relative image paths are resolved against the page's origin.
            const size_t scheme = url.find("://");
            const size_t path = scheme == std::string::npos ? std::string::npos : url.find('/', scheme + 3);
            const std::string origin = path == std::string::npos ? url : url.substr(0, path);
            if (p.image.rfind("//", 0) == 0)
                p.image = "https:" + p.image;
            else if (!p.image.empty() && p.image[0] == '/')
                p.image = origin + p.image;
            if (p.site.empty() && scheme != std::string::npos) {
                p.site = origin.substr(scheme + 3);
                if (p.site.rfind("www.", 0) == 0)
                    p.site = p.site.substr(4);
            }
            auto trim = [](std::string& t, size_t max) {
                for (char& c : t)
                    if (c == '\n' || c == '\r' || c == '\t')
                        c = ' ';
                g_strstrip(t.data());
                t = t.c_str();
                if (g_utf8_strlen(t.c_str(), -1) > static_cast<glong>(max)) {
                    gchar* cut = g_utf8_substring(t.c_str(), 0, max);
                    t = std::string(cut) + "…";
                    g_free(cut);
                }
                if (!g_utf8_validate(t.c_str(), -1, nullptr))
                    t.clear();
            };
            trim(p.title, 120);
            trim(p.description, 200);
            p.ok = !p.title.empty() || !p.image.empty();
            return p;
        }

        // One fetch per link for the life of the app, however often it is drawn.
        std::map<std::string, Preview>& previews() {
            static std::map<std::string, Preview> cache;
            return cache;
        }
        std::map<std::string, std::vector<GWeakRef*>>& waiting_cards() {
            static std::map<std::string, std::vector<GWeakRef*>> waiting;
            return waiting;
        }

        void fill_card(GtkWidget* card, const Preview& p) {
            GtkWidget* image = GTK_WIDGET(g_object_get_data(G_OBJECT(card), "image"));
            GtkWidget* site = GTK_WIDGET(g_object_get_data(G_OBJECT(card), "site"));
            GtkWidget* title = GTK_WIDGET(g_object_get_data(G_OBJECT(card), "title"));
            GtkWidget* description = GTK_WIDGET(g_object_get_data(G_OBJECT(card), "description"));
            if (!p.ok) {
                // Nothing to show beyond the link itself, which the bubble already has.
                gtk_widget_hide(card);
                return;
            }
            gtk_label_set_text(GTK_LABEL(site), p.site.c_str());
            gtk_label_set_text(GTK_LABEL(title), p.title.c_str());
            gtk_widget_set_visible(title, !p.title.empty());
            gtk_label_set_text(GTK_LABEL(description), p.description.c_str());
            gtk_widget_set_visible(description, !p.description.empty());
            if (!p.image.empty()) {
                GWeakRef* weak = g_new0(GWeakRef, 1);
                g_weak_ref_init(weak, image);
                fetch(p.image, false, [weak](bool ok, const std::string& path, const std::string&) {
                    GtkWidget* image = GTK_WIDGET(g_weak_ref_get(weak));
                    g_weak_ref_clear(weak);
                    g_free(weak);
                    if (!image)
                        return;
                    if (ok && show_file(image, path))
                        gtk_widget_show(image);
                    g_object_unref(image);
                });
            }
            gtk_widget_show(card);
        }

    } // namespace

    GtkWidget* link_preview_new(const std::string& url) {
        GtkWidget* card = gtk_event_box_new();
        gtk_style_context_add_class(gtk_widget_get_style_context(card), "tether-link-card");
        g_object_set_data_full(G_OBJECT(card), "url", g_strdup(url.c_str()), g_free);
        gtk_widget_set_tooltip_text(card, url.c_str());
        g_signal_connect(card, "button-press-event", G_CALLBACK(open_link), nullptr);

        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
        gtk_widget_set_size_request(box, 260, -1);
        GtkWidget* image = gtk_image_new();
        gtk_widget_set_no_show_all(image, TRUE);
        gtk_widget_set_halign(image, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(box), image, FALSE, FALSE, 0);
        GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_container_set_border_width(GTK_CONTAINER(text), 8);
        GtkWidget* site = gtk_label_new("");
        GtkWidget* title = gtk_label_new("");
        GtkWidget* description = gtk_label_new("");
        for (GtkWidget* label : {site, title, description}) {
            gtk_label_set_xalign(GTK_LABEL(label), 0.0);
            gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
            gtk_label_set_max_width_chars(GTK_LABEL(label), 36);
            gtk_label_set_lines(GTK_LABEL(label), 3);
            gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
            gtk_box_pack_start(GTK_BOX(text), label, FALSE, FALSE, 0);
        }
        gtk_style_context_add_class(gtk_widget_get_style_context(site), "tether-link-site");
        gtk_style_context_add_class(gtk_widget_get_style_context(title), "tether-link-title");
        gtk_style_context_add_class(gtk_widget_get_style_context(description), "tether-link-description");
        gtk_box_pack_start(GTK_BOX(box), text, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(card), box);
        g_object_set_data(G_OBJECT(card), "image", image);
        g_object_set_data(G_OBJECT(card), "site", site);
        g_object_set_data(G_OBJECT(card), "title", title);
        g_object_set_data(G_OBJECT(card), "description", description);
        gtk_widget_show_all(box);
        // Hidden until there is something to show, so a dead link leaves no empty box.
        gtk_widget_set_no_show_all(card, TRUE);

        auto& cache = previews();
        if (auto it = cache.find(url); it != cache.end() && it->second.done) {
            fill_card(card, it->second);
            return card;
        }
        GWeakRef* weak = g_new0(GWeakRef, 1);
        g_weak_ref_init(weak, card);
        auto& waiting = waiting_cards()[url];
        waiting.push_back(weak);
        if (waiting.size() > 1 || cache.count(url))
            return card;
        cache[url] = Preview{};
        auto publish = [url](Preview p) {
            p.done = true;
            previews()[url] = p;
            auto cards = std::move(waiting_cards()[url]);
            waiting_cards().erase(url);
            for (GWeakRef* weak : cards) {
                if (GtkWidget* card = GTK_WIDGET(g_weak_ref_get(weak))) {
                    fill_card(card, p);
                    g_object_unref(card);
                }
                g_weak_ref_clear(weak);
                g_free(weak);
            }
        };
        // As a previewer first, then as a browser for sites that only answer those.
        fetch(url, true, [url, publish](bool ok, const std::string&, const std::string& body) {
            Preview p = ok ? parse_preview(body, url) : Preview{};
            if (p.ok && !p.image.empty()) {
                publish(p);
                return;
            }
            fetch(url, true, [url, publish, p](bool ok, const std::string&, const std::string& body) {
                Preview second = ok ? parse_preview(body, url) : Preview{};
                publish(second.ok && (!p.ok || !second.image.empty()) ? second : p);
            });
        }, PREVIEW_AGENT);
        return card;
    }

    namespace {

        struct Picker {
            GtkWidget* popover = nullptr;
            GtkWidget* stack = nullptr;
            GtkWidget* search = nullptr;
            GtkWidget* grid = nullptr;
            GtkWidget* status = nullptr;
            std::function<void(const std::string&)> on_pick;
            unsigned generation = 0;
        };

        std::string giphy_key() {
            auto& p = prefs();
            return p.contains(GIPHY_KEY_PREF) && p[GIPHY_KEY_PREF].is_string() ? p[GIPHY_KEY_PREF].get<std::string>() : "";
        }

        void load_results(Picker* picker, const std::string& query) {
            const unsigned generation = ++picker->generation;
            gchar* escaped = g_uri_escape_string(query.c_str(), nullptr, FALSE);
            const std::string key = giphy_key();
            const std::string url = query.empty()
                                        ? std::string(GIPHY_API) + "trending?limit=24&rating=pg-13&api_key=" + key
                                        : std::string(GIPHY_API) + "search?limit=24&rating=pg-13&api_key=" + key +
                                              "&q=" + escaped;
            g_free(escaped);
            gtk_label_set_text(GTK_LABEL(picker->status), _("Loading…"));
            gtk_widget_show(picker->status);

            // The picker owns its popover; a weak ref drops answers after it closes.
            GWeakRef* weak = g_new0(GWeakRef, 1);
            g_weak_ref_init(weak, picker->popover);
            fetch(url, true, [weak, picker, generation](bool ok, const std::string&, const std::string& body) {
                GObject* alive = static_cast<GObject*>(g_weak_ref_get(weak));
                g_weak_ref_clear(weak);
                g_free(weak);
                if (!alive)
                    return;
                g_object_unref(alive);
                if (generation != picker->generation)
                    return;
                clear_list_box(picker->grid);
                auto json = nlohmann::json::parse(body, nullptr, false);
                if (!ok || json.is_discarded() || !json.contains("data")) {
                    gtk_label_set_text(GTK_LABEL(picker->status),
                                       _("GIPHY did not answer. Check the API key in the menu (⋯) or your connection."));
                    return;
                }
                gtk_widget_hide(picker->status);
                for (const auto& gif : json["data"]) {
                    const auto& images = gif.value("images", nlohmann::json::object());
                    const std::string preview =
                        images.value("fixed_width_small", nlohmann::json::object()).value("url", "");
                    std::string full = images.value("original", nlohmann::json::object()).value("url", "");
                    full = full.substr(0, full.find('?'));
                    if (preview.empty() || full.empty())
                        continue;
                    GtkWidget* button = gtk_button_new();
                    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
                    gtk_widget_set_tooltip_text(button, gif.value("title", "").c_str());
                    GtkWidget* image = gtk_image_new_from_icon_name("image-loading-symbolic", GTK_ICON_SIZE_DIALOG);
                    gtk_widget_set_size_request(image, 100, 80);
                    gtk_container_add(GTK_CONTAINER(button), image);
                    g_object_set_data_full(G_OBJECT(button), "url", g_strdup(full.c_str()), g_free);
                    g_signal_connect(button,
                                     "clicked",
                                     G_CALLBACK(+[](GtkButton* b, gpointer data) {
                                         auto* picker = static_cast<Picker*>(data);
                                         const std::string url = (const char*)g_object_get_data(G_OBJECT(b), "url");
                                         auto pick = picker->on_pick;
                                         gtk_popover_popdown(GTK_POPOVER(picker->popover));
                                         pick(url);
                                     }),
                                     picker);
                    gtk_flow_box_insert(GTK_FLOW_BOX(picker->grid), button, -1);
                    GWeakRef* image_ref = g_new0(GWeakRef, 1);
                    g_weak_ref_init(image_ref, image);
                    fetch(preview, false, [image_ref](bool ok, const std::string& path, const std::string&) {
                        GtkWidget* image = GTK_WIDGET(g_weak_ref_get(image_ref));
                        g_weak_ref_clear(image_ref);
                        g_free(image_ref);
                        if (!image)
                            return;
                        if (ok)
                            show_file(image, path);
                        g_object_unref(image);
                    });
                }
                gtk_widget_show_all(picker->grid);
            });
        }

        GtkWidget* build_key_page(Picker* picker) {
            GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
            gtk_container_set_border_width(GTK_CONTAINER(page), 12);
            GtkWidget* title = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(title), (std::string("<b>") + _("Connect GIPHY") + "</b>").c_str());
            gtk_label_set_xalign(GTK_LABEL(title), 0.0);
            gtk_box_pack_start(GTK_BOX(page), title, FALSE, FALSE, 0);
            GtkWidget* hint = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(hint),
                                 _("GIF search needs a free API key. Create one at "
                                   "<a href=\"https://developers.giphy.com/dashboard/\">developers.giphy.com</a> "
                                   "and paste it here."));
            gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
            gtk_label_set_max_width_chars(GTK_LABEL(hint), 40);
            gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
            gtk_box_pack_start(GTK_BOX(page), hint, FALSE, FALSE, 0);
            GtkWidget* entry = gtk_entry_new();
            gtk_entry_set_placeholder_text(GTK_ENTRY(entry), _("GIPHY API key"));
            gtk_box_pack_start(GTK_BOX(page), entry, FALSE, FALSE, 0);
            GtkWidget* save = gtk_button_new_with_label(_("Save"));
            gtk_style_context_add_class(gtk_widget_get_style_context(save), "suggested-action");
            gtk_widget_set_halign(save, GTK_ALIGN_END);
            g_object_set_data(G_OBJECT(save), "entry", entry);
            auto on_save = +[](GtkWidget* w, gpointer data) {
                auto* picker = static_cast<Picker*>(data);
                GtkWidget* entry = GTK_IS_ENTRY(w) ? w : GTK_WIDGET(g_object_get_data(G_OBJECT(w), "entry"));
                std::string key = gtk_entry_get_text(GTK_ENTRY(entry));
                key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) { return std::isspace(c); }),
                          key.end());
                if (key.empty())
                    return;
                prefs()[GIPHY_KEY_PREF] = key;
                prefs_save();
                gtk_stack_set_visible_child_name(GTK_STACK(picker->stack), "gifs");
                gtk_widget_grab_focus(picker->search);
                load_results(picker, "");
            };
            g_signal_connect(save, "clicked", G_CALLBACK(on_save), picker);
            g_signal_connect(entry, "activate", G_CALLBACK(on_save), picker);
            gtk_box_pack_start(GTK_BOX(page), save, FALSE, FALSE, 0);
            return page;
        }

    } // namespace

    void gif_picker_open(GtkWidget* relative_to, std::function<void(const std::string& url)> on_pick) {
        auto* picker = new Picker;
        picker->on_pick = std::move(on_pick);
        picker->popover = gtk_popover_new(relative_to);
        gtk_style_context_add_class(gtk_widget_get_style_context(picker->popover), "tether-gif-picker");
        g_object_set_data_full(G_OBJECT(picker->popover), "picker", picker, [](gpointer p) { delete static_cast<Picker*>(p); });

        picker->stack = gtk_stack_new();
        gtk_stack_add_named(GTK_STACK(picker->stack), build_key_page(picker), "key");

        GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(page), 8);
        GtkWidget* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        picker->search = gtk_search_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(picker->search), _("Search GIPHY"));
        g_signal_connect(picker->search,
                         "search-changed",
                         G_CALLBACK(+[](GtkSearchEntry* entry, gpointer data) {
                             load_results(static_cast<Picker*>(data), gtk_entry_get_text(GTK_ENTRY(entry)));
                         }),
                         picker);
        gtk_box_pack_start(GTK_BOX(top), picker->search, TRUE, TRUE, 0);
        GtkWidget* change_key = gtk_button_new_from_icon_name("view-more-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_button_set_relief(GTK_BUTTON(change_key), GTK_RELIEF_NONE);
        gtk_widget_set_tooltip_text(change_key, _("Change GIPHY API key"));
        g_signal_connect(change_key,
                         "clicked",
                         G_CALLBACK(+[](GtkButton*, gpointer data) {
                             gtk_stack_set_visible_child_name(GTK_STACK(static_cast<Picker*>(data)->stack), "key");
                         }),
                         picker);
        gtk_box_pack_start(GTK_BOX(top), change_key, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(page), top, FALSE, FALSE, 0);

        picker->status = gtk_label_new("");
        gtk_label_set_line_wrap(GTK_LABEL(picker->status), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(picker->status), 40);
        gtk_style_context_add_class(gtk_widget_get_style_context(picker->status), "muted");
        gtk_box_pack_start(GTK_BOX(page), picker->status, FALSE, FALSE, 0);

        GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_size_request(scroll, 360, 340);
        picker->grid = gtk_flow_box_new();
        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(picker->grid), GTK_SELECTION_NONE);
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(picker->grid), 3);
        gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(picker->grid), TRUE);
        gtk_container_add(GTK_CONTAINER(scroll), picker->grid);
        gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);
        GtkWidget* credit = gtk_label_new(_("Powered by GIPHY"));
        gtk_style_context_add_class(gtk_widget_get_style_context(credit), "muted");
        gtk_widget_set_halign(credit, GTK_ALIGN_END);
        gtk_box_pack_start(GTK_BOX(page), credit, FALSE, FALSE, 0);
        gtk_stack_add_named(GTK_STACK(picker->stack), page, "gifs");

        gtk_container_add(GTK_CONTAINER(picker->popover), picker->stack);
        gtk_widget_show_all(picker->stack);
        g_signal_connect(picker->popover, "closed", G_CALLBACK(+[](GtkPopover* p, gpointer) { gtk_widget_destroy(GTK_WIDGET(p)); }), nullptr);

        if (giphy_key().empty()) {
            gtk_stack_set_visible_child_name(GTK_STACK(picker->stack), "key");
        } else {
            gtk_stack_set_visible_child_name(GTK_STACK(picker->stack), "gifs");
            load_results(picker, "");
        }
        gtk_popover_popup(GTK_POPOVER(picker->popover));
        if (!giphy_key().empty())
            gtk_widget_grab_focus(picker->search);
    }

} // namespace tether::ui
