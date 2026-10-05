#include "messages_view.hpp"
#include <tether/i18n.hpp>

#include "contact_completion.hpp"
#include "daemon_client.hpp"
#include "media.hpp"
#include "message_format.hpp"
#include "prefs.hpp"
#include "tray.hpp"
#include "ui_util.hpp"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <gdk/gdkkeysyms.h>
#include <glib/gstdio.h>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <tether/bluetooth/bmessage.hpp>
#include <tether/bluetooth/contacts.hpp>
#include <vector>

namespace tether::ui {

    namespace {

        struct MessagesState {
            GtkWidget* banner = nullptr;
            GtkWidget* banner_label = nullptr;
            GtkWidget* banner_action = nullptr;
            GtkWidget* thread_list = nullptr;
            GtkWidget* conversation = nullptr;
            GtkWidget* conversation_header = nullptr;
            GtkWidget* conversation_scroll = nullptr;
            GtkWidget* composer = nullptr;
            GtkWidget* send_button = nullptr;
            GtkWidget* placeholder_stack = nullptr;
            GtkWidget* placeholder_icon = nullptr;
            GtkWidget* placeholder_label = nullptr;
            GtkWidget* paned = nullptr;
            GtkWidget* thread_scroll = nullptr;
            GtkWidget* search_entry = nullptr;
            GtkWidget* send_error = nullptr;
            GtkWidget* send_error_label = nullptr;
            GtkWidget* header_avatar = nullptr;
            // History search: results across every conversation in the sidebar, and
            // a find bar inside the open conversation.
            GtkWidget* results_header = nullptr;
            GtkWidget* results_list = nullptr;
            guint search_timer = 0;
            std::string pending_jump;  // message to scroll to once its conversation renders
            // The row being jumped to; layout keeps re-aiming at it until this clears.
            GtkWidget* jump_row = nullptr;
            guint jump_timer = 0;
            GtkWidget* find_bar = nullptr;
            GtkWidget* find_entry = nullptr;
            GtkWidget* find_count = nullptr;
            std::vector<GtkWidget*> find_hits;
            int find_index = -1;
            GtkWidget* composer_placeholder = nullptr;
            GtkWidget* reply_bar = nullptr;
            GtkWidget* reply_label = nullptr;
            // The "Sent" under the newest outgoing bubble, moved as new ones arrive.
            GtkWidget* last_status = nullptr;

            // Body of the message the next send replies to, or empty.
            std::string reply_to;
            // Messages waiting for the phone, sent one at a time in order. The
            // front one is in flight while `sending` is set.
            struct Outgoing {
                std::string thread;
                std::string body;
                GtkWidget* row = nullptr;  // its optimistic bubble, if on screen
            };
            std::deque<Outgoing> outbox;
            // Where each rendered message shows its reactions, by body text.
            std::map<std::string, GtkWidget*> reaction_slots;
            // "N replies" under each rendered message, by body text.
            std::map<std::string, GtkWidget*> reply_counters;

            std::string selected_thread;
            std::string selected_name;
            bool visible = false;
            bool map_open = false;
            bool threads_known = false;
            size_t thread_count = 0;
            bool sending = false;
            GtkWidget* composer_notice = nullptr;
            // Rebuilding the thread list destroys and recreates the selected row.
            // The handler is blocked across that so the churn is not mistaken for
            // the user opening a conversation.
            gulong thread_selected_handler = 0;
            // A send whose result never arrives must not lock the composer for the
            // rest of the session.
            guint send_watchdog_id = 0;
            // Handles already asked about, so reopening a conversation does not
            // re-issue a blocking OBEX write per message every time.
            std::set<std::string> marked_read;

            bool scroll_pin = true;
            bool scroll_restore = false;
            double scroll_from_bottom = 0.0;
            double scroll_last_bottom = 0.0;
            // The user's own message belongs on screen no matter where they had
            // scrolled to when they sent it.
            bool pin_next = false;

            // Composer text per conversation
            std::map<std::string, std::string> drafts;

            // Handles of the message rows currently on screen
            std::vector<std::string> rendered;
            // The whole conversation as the daemon sent it, and the index of the
            // first message actually drawn: only the newest page is built up front.
            nlohmann::json all_messages = nlohmann::json::array();
            size_t window_start = 0;
            bool loading_older = false;
            // Set when the sidebar must redraw even if the thread list is unchanged (a pin).
            bool threads_dirty = false;

            // The last bubble rendered
            int64_t rendered_last_stamp = 0;
            bool rendered_last_outgoing = false;

            guint focus_idle_id = 0;

            // Sidebar scroll offset to put back after the rebuild that dropped it.
            double thread_scroll_value = -1.0;
            guint thread_scroll_idle_id = 0;

            // Folded needle from the search box; empty shows everything.
            std::string search_needle;
            // Whether the daemon says the selected thread can be replied to, and
            // why not when it cannot.
            bool selected_repliable = false;
            std::string selected_block_reason;
            // A conversation opened from outside the list is a reply that is already under way
            bool focus_composer = false;

            // Addressing a message to someone with no conversation yet.
            bool composing = false;
            GtkWidget* compose_bar = nullptr;
            GtkWidget* compose_entry = nullptr;
            std::string compose_requested_key;
            std::string pending_new_thread;

            // Files waiting to go with the next send, shown as chips above the
            // composer. MAP carries text only, so they are uploaded and sent as links.
            std::vector<std::string> attachments;
            // Pasted images written to disk by us, removed once sent or dropped.
            std::set<std::string> pasted;
            GtkWidget* attach_bar = nullptr;
            GtkWidget* attach_chips = nullptr;
            bool uploading = false;
        };

        MessagesState g_messages;

        constexpr int SEND_TIMEOUT_SECONDS = 60;
        constexpr int64_t GROUP_WINDOW_SECONDS = 300;
        constexpr double AT_BOTTOM_SLACK = 48.0;

        void update_composer_sensitivity();
        void update_placeholder();
        void apply_row_selection(GtkWidget* row);
        void clear_selection();
        void switch_thread(const std::string& key);
        void hide_send_error();
        // `offer_permissions` shows the button that re-solicits the iPhone's
        // Bluetooth toggles, which only helps when that is the actual problem.
        void set_banner(const std::string& text, bool offer_permissions = false);
        std::string composer_text();
        void leave_compose();
        void enter_compose();
        void on_recipient_changed(GtkEditable*, gpointer);
        void clear_attachments();

        gboolean focus_composer_idle(gpointer) {
            g_messages.focus_idle_id = 0;
            if (g_messages.composer && gtk_widget_is_sensitive(g_messages.composer))
                gtk_widget_grab_focus(g_messages.composer);
            return G_SOURCE_REMOVE;
        }

        void focus_composer_soon() {
            if (g_messages.focus_idle_id == 0)
                g_messages.focus_idle_id = g_idle_add(focus_composer_idle, nullptr);
        }

        void set_composer_text(const std::string& text) {
            if (!g_messages.composer)
                return;
            GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_messages.composer));
            gtk_text_buffer_set_text(buffer, text.c_str(), -1);
            GtkTextIter end;
            gtk_text_buffer_get_end_iter(buffer, &end);
            gtk_text_buffer_place_cursor(buffer, &end);
        }

        void stash_draft() {
            if (g_messages.selected_thread.empty() || !g_messages.composer)
                return;
            const std::string text = composer_text();
            if (text.empty())
                g_messages.drafts.erase(g_messages.selected_thread);
            else
                g_messages.drafts[g_messages.selected_thread] = text;
        }

        void end_jump();

        void switch_thread(const std::string& key) {
            if (key == g_messages.selected_thread)
                return;
            stash_draft();
            g_messages.selected_thread = key;
            g_messages.rendered.clear();
            g_messages.all_messages = nlohmann::json::array();
            g_messages.window_start = 0;
            g_messages.loading_older = false;
            end_jump();
            g_messages.rendered_last_stamp = 0;
            g_messages.rendered_last_outgoing = false;
            if (g_messages.conversation)
                clear_list_box(g_messages.conversation);
            g_messages.reaction_slots.clear();
            g_messages.reply_counters.clear();
            hide_send_error();
            // Files picked for one person must not go out to the next conversation.
            clear_attachments();

            const auto draft = g_messages.drafts.find(key);
            set_composer_text(draft == g_messages.drafts.end() ? "" : draft->second);
        }

        std::string format_timestamp(int64_t epoch) {
            if (epoch <= 0)
                return "";
            std::time_t t = static_cast<std::time_t>(epoch);
            std::tm tm{};
            localtime_r(&t, &tm);

            char buffer[64];
            // xgettext:no-c-format
            std::strftime(buffer, sizeof(buffer), _("%H:%M"), &tm);
            return buffer;
        }

        void request_threads() {
            nlohmann::json j;
            j["command"] = "bt_list_threads";
            daemon_send(j);
        }

        void request_messages(const std::string& thread_key) {
            if (thread_key.empty())
                return;
            nlohmann::json j;
            j["command"] = "bt_list_messages";
            j["thread"] = thread_key;
            daemon_send(j);
        }

        // Where a conversation sits among the pins, or -1. Matches by number, so a
        // pin made as "+1647…" still applies to a thread keyed "647…".
        int thread_pin_index(const std::string& thread) {
            const auto& list = prefs().value("pinned", nlohmann::json::array());
            const std::string bucket = bluetooth::thread_bucket(thread);
            for (size_t i = 0; i < list.size(); ++i)
                if (list[i].is_string() && bluetooth::thread_bucket(list[i].get<std::string>()) == bucket)
                    return static_cast<int>(i);
            return -1;
        }

        GtkWidget* build_thread_row(const nlohmann::json& thread) {
            const std::string key = thread.value("thread", "");
            const std::string address = thread.value("address", "");
            const std::string name = thread.value("name", address);
            std::string preview = thread.value("preview", "");
            // Previews arrive flattened to one line, so a reply reads as its quote; show the reply.
            for (const char* mark : {"> \u201c", "\u21aa \u201c"}) {
                if (preview.rfind(mark, 0) == 0) {
                    const size_t close = preview.find("\u201d ");
                    if (close != std::string::npos)
                        preview = preview.substr(close + strlen("\u201d "));
                }
            }
            const int unread = thread.value("unread", 0);
            const int64_t stamp = thread.value("timestamp", static_cast<int64_t>(0));

            GtkWidget* row = gtk_list_box_row_new();
            g_object_set_data_full(G_OBJECT(row), "thread", g_strdup(key.c_str()), g_free);
            g_object_set_data_full(G_OBJECT(row), "name", g_strdup(name.c_str()), g_free);
            g_object_set_data_full(G_OBJECT(row), "photo", g_strdup(thread.value("photo", "").c_str()), g_free);
            const int pin = thread_pin_index(key);
            g_object_set_data(G_OBJECT(row), "pin", GINT_TO_POINTER(pin + 1));
            g_object_set_data_full(
                G_OBJECT(row), "search", g_strdup(fold(name + " " + address + " " + preview).c_str()), g_free);
            // The daemon owns the decision about whether a group can be replied
            // to; the UI must not re-derive it from the key shape.
            g_object_set_data(G_OBJECT(row), "repliable", GINT_TO_POINTER(thread.value("repliable", true) ? 1 : 0));
            g_object_set_data_full(
                G_OBJECT(row), "reply_reason", g_strdup(thread.value("reply_reason", "").c_str()), g_free);
            g_object_set_data(G_OBJECT(row), "group", GINT_TO_POINTER(thread.value("group", false) ? 1 : 0));

            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            gtk_container_set_border_width(GTK_CONTAINER(box), 10);

            gtk_box_pack_start(GTK_BOX(box), avatar_new(thread.value("photo", ""), name, 40), FALSE, FALSE, 0);

            GtkWidget* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

            GtkWidget* title = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(title), ("<b>" + escape_markup(name) + "</b>").c_str());
            gtk_label_set_xalign(GTK_LABEL(title), 0.0);
            gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
            gtk_box_pack_start(GTK_BOX(labels), title, FALSE, FALSE, 0);

            GtkWidget* subtitle = gtk_label_new(preview.c_str());
            gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0);
            gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
            gtk_label_set_single_line_mode(GTK_LABEL(subtitle), TRUE);
            gtk_style_context_add_class(gtk_widget_get_style_context(subtitle), "muted");
            gtk_box_pack_start(GTK_BOX(labels), subtitle, FALSE, FALSE, 0);

            if (unread > 0)
                gtk_style_context_add_class(gtk_widget_get_style_context(subtitle), "tether-thread-unread");

            gtk_box_pack_start(GTK_BOX(box), labels, TRUE, TRUE, 0);

            GtkWidget* meta = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
            if (pin >= 0) {
                GtkWidget* pin_icon = gtk_image_new_from_icon_name("view-pin-symbolic", GTK_ICON_SIZE_MENU);
                gtk_widget_set_tooltip_text(pin_icon, _("Pinned"));
                gtk_widget_set_halign(pin_icon, GTK_ALIGN_END);
                gtk_style_context_add_class(gtk_widget_get_style_context(pin_icon), "muted");
                gtk_box_pack_end(GTK_BOX(meta), pin_icon, FALSE, FALSE, 0);
            }
            gtk_widget_set_valign(meta, GTK_ALIGN_START);

            const std::string when = format_thread_time(stamp, std::time(nullptr));
            if (!when.empty()) {
                GtkWidget* time_label = gtk_label_new(when.c_str());
                gtk_label_set_xalign(GTK_LABEL(time_label), 1.0);
                gtk_style_context_add_class(gtk_widget_get_style_context(time_label), "muted");
                gtk_box_pack_start(GTK_BOX(meta), time_label, FALSE, FALSE, 0);
            }

            if (unread > 0) {
                GtkWidget* badge = gtk_label_new(nullptr);
                gtk_label_set_markup(GTK_LABEL(badge), ("<b>" + std::to_string(unread) + "</b>").c_str());
                set_accessible_name(badge,
                                    tether::tr_format(P_("{} unread message", "{} unread messages", unread), unread));
                gtk_widget_set_halign(badge, GTK_ALIGN_END);
                gtk_style_context_add_class(gtk_widget_get_style_context(badge), "tether-badge");
                gtk_box_pack_start(GTK_BOX(meta), badge, FALSE, FALSE, 0);
            }

            gtk_box_pack_start(GTK_BOX(box), meta, FALSE, FALSE, 0);

            gtk_container_add(GTK_CONTAINER(row), box);
            return row;
        }

        gboolean on_bubble_link(GtkWidget* label, gchar* uri, gpointer) {
            GtkWidget* toplevel = gtk_widget_get_toplevel(label);
            gtk_show_uri_on_window(
                GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : nullptr, uri, GDK_CURRENT_TIME, nullptr);
            return TRUE;
        }

        void set_header_avatar(const std::string& photo, const std::string& name) {
            if (!g_messages.header_avatar)
                return;
            clear_list_box(g_messages.header_avatar);
            gtk_box_pack_start(GTK_BOX(g_messages.header_avatar), avatar_new(photo, name, 44), FALSE, FALSE, 0);
            gtk_widget_show_all(g_messages.header_avatar);
        }

        // iOS's own words for tapbacks, as they reach a phone that cannot show
        // them natively. MAP carries reactions exactly this way, so it is also how
        // they are sent from here.
        struct Tapback {
            const char* verb;
            const char* emoji;
        };
        constexpr Tapback TAPBACKS[] = {{"Loved", "❤️"},
                                        {"Liked", "\U0001F44D"},
                                        {"Disliked", "\U0001F44E"},
                                        {"Laughed at", "\U0001F602"},
                                        {"Emphasized", "‼️"},
                                        {"Questioned", "❓"}};
        constexpr int64_t STAMP_GAP_SECONDS = 15 * 60;
        constexpr const char* OPEN_QUOTE = "“";
        constexpr const char* CLOSE_QUOTE = "”";
        constexpr const char* REPLY_MARK = "> ";
        // What replies were marked with before the caret.
        constexpr const char* OLD_REPLY_MARK = "\u21aa ";

        // `“text”` with curly or straight quotes -> text.
        bool unquote(const std::string& quoted, std::string& out) {
            for (const auto& [open, close] : {std::pair<std::string, std::string>{OPEN_QUOTE, CLOSE_QUOTE},
                                              std::pair<std::string, std::string>{"\"", "\""}}) {
                if (quoted.size() >= open.size() + close.size() && quoted.rfind(open, 0) == 0 &&
                    quoted.compare(quoted.size() - close.size(), close.size(), close) == 0) {
                    out = quoted.substr(open.size(), quoted.size() - open.size() - close.size());
                    return true;
                }
            }
            return false;
        }

        // "Loved “hi”" or "Reacted 🔥 to “hi”" -> the emoji and "hi".
        bool parse_tapback(const std::string& body, std::string& emoji, std::string& target) {
            for (const auto& tapback : TAPBACKS) {
                const std::string prefix = std::string(tapback.verb) + " ";
                if (body.rfind(prefix, 0) == 0 && unquote(body.substr(prefix.size()), target)) {
                    emoji = tapback.emoji;
                    return true;
                }
            }
            if (body.rfind("Reacted ", 0) == 0) {
                const size_t to = body.find(" to ", 8);
                if (to != std::string::npos && unquote(body.substr(to + 4), target)) {
                    emoji = body.substr(8, to - 8);
                    return !emoji.empty();
                }
            }
            return false;
        }

        // "↪ “quoted”\nreply" -> quote and reply. Plain messages pass through.
        void parse_reply(const std::string& body, std::string& quote, std::string& text) {
            text = body;
            quote.clear();
            const char* mark = body.rfind(REPLY_MARK, 0) == 0       ? REPLY_MARK
                               : body.rfind(OLD_REPLY_MARK, 0) == 0 ? OLD_REPLY_MARK
                                                                    : nullptr;
            if (!mark)
                return;
            const size_t nl = body.find('\n');
            if (nl == std::string::npos)
                return;
            std::string inner;
            if (!unquote(body.substr(strlen(mark), nl - strlen(mark)), inner))
                return;
            quote = inner;
            text = body.substr(nl + 1);
        }

        // One to three emoji and nothing else, which iMessage shows large without a
        // bubble. Joiners and variation selectors ride along; punctuation does not count.
        bool is_emoji_only(const std::string& text) {
            if (text.empty() || !g_utf8_validate(text.c_str(), -1, nullptr))
                return false;
            int emoji = 0;
            for (const char* p = text.c_str(); *p; p = g_utf8_next_char(p)) {
                const gunichar c = g_utf8_get_char(p);
                if (c == 0x200D || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0x1F3FB && c <= 0x1F3FF) || c == 0x20E3)
                    continue;
                const bool pictograph = (c >= 0x1F000 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF) ||
                                        (c >= 0x2B00 && c <= 0x2BFF) || c == 0x203C || c == 0x2049 ||
                                        (c >= 0x1F1E6 && c <= 0x1F1FF);
                if (!pictograph)
                    return false;
                ++emoji;
            }
            return emoji >= 1 && emoji <= 6;
        }

        std::string snippet(const std::string& text, size_t max_chars) {
            gchar* flat = g_strdup(text.c_str());
            for (gchar* c = flat; *c; ++c)
                if (*c == '\n')
                    *c = ' ';
            std::string out = flat;
            g_free(flat);
            if (g_utf8_strlen(out.c_str(), -1) > static_cast<glong>(max_chars)) {
                gchar* cut = g_utf8_substring(out.c_str(), 0, max_chars);
                out = std::string(cut) + "…";
                g_free(cut);
            }
            return out;
        }

        void deliver(const std::string& thread, const std::string& body);

        void send_side_message(const std::string& body) {
            if (!g_messages.selected_thread.empty())
                deliver(g_messages.selected_thread, body);
        }

        void add_reaction(const std::string& target, const std::string& emoji) {
            const auto slot = g_messages.reaction_slots.find(target);
            if (slot == g_messages.reaction_slots.end())
                return;
            GtkWidget* badge = gtk_label_new(emoji.c_str());
            gtk_style_context_add_class(gtk_widget_get_style_context(badge), "tether-reaction");
            gtk_box_pack_start(GTK_BOX(slot->second), badge, FALSE, FALSE, 0);
            gtk_widget_show(badge);
            gtk_widget_show(slot->second);
        }

        // A quote is the original cut to 60 characters, so a cut one matches by prefix.
        GtkWidget* find_reply_counter(const std::string& quote) {
            if (auto it = g_messages.reply_counters.find(quote); it != g_messages.reply_counters.end())
                return it->second;
            const std::string ellipsis = "\u2026";
            if (quote.size() > ellipsis.size() && quote.compare(quote.size() - ellipsis.size(), ellipsis.size(), ellipsis) == 0) {
                const std::string prefix = quote.substr(0, quote.size() - ellipsis.size());
                for (auto it = g_messages.reply_counters.rbegin(); it != g_messages.reply_counters.rend(); ++it)
                    if (snippet(it->first, 60) == quote || it->first.rfind(prefix, 0) == 0)
                        return it->second;
            }
            return nullptr;
        }

        void count_reply(const std::string& quote) {
            GtkWidget* counter = find_reply_counter(quote);
            if (!counter)
                return;
            const int replies = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(counter), "replies")) + 1;
            g_object_set_data(G_OBJECT(counter), "replies", GINT_TO_POINTER(replies));
            gtk_button_set_label(GTK_BUTTON(counter),
                                 tether::tr_format(P_("{} reply", "{} replies", replies), replies).c_str());
            gtk_widget_show(counter);
        }

        void show_reply_bar(const std::string& body) {
            g_messages.reply_to = body;
            set_text(g_messages.reply_label, tether::tr_format(_("Replying to {}"), OPEN_QUOTE + snippet(body, 60) + CLOSE_QUOTE));
            gtk_widget_show(g_messages.reply_bar);
            focus_composer_soon();
        }

        void hide_reply_bar() {
            g_messages.reply_to.clear();
            if (g_messages.reply_bar)
                gtk_widget_hide(g_messages.reply_bar);
        }

        const char* message_body(GtkWidget* widget) {
            return static_cast<const char*>(g_object_get_data(G_OBJECT(widget), "body"));
        }

        void on_reply_clicked(GtkButton* button, gpointer) {
            if (const char* body = message_body(GTK_WIDGET(button)))
                show_reply_bar(body);
        }

        void on_tapback_clicked(GtkButton* button, gpointer verb) {
            const char* body = message_body(GTK_WIDGET(button));
            if (!body)
                return;
            send_side_message(std::string(static_cast<const char*>(verb)) + " " + OPEN_QUOTE + body + CLOSE_QUOTE);
            if (GtkWidget* popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER))
                gtk_popover_popdown(GTK_POPOVER(popover));
        }

        void on_react_clicked(GtkButton* button, gpointer) {
            const char* body = message_body(GTK_WIDGET(button));
            if (!body)
                return;
            GtkWidget* popover = gtk_popover_new(GTK_WIDGET(button));
            gtk_style_context_add_class(gtk_widget_get_style_context(popover), "tether-tapback-picker");
            GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
            gtk_container_set_border_width(GTK_CONTAINER(row), 4);
            for (const auto& tapback : TAPBACKS) {
                GtkWidget* choice = gtk_button_new_with_label(tapback.emoji);
                gtk_button_set_relief(GTK_BUTTON(choice), GTK_RELIEF_NONE);
                gtk_widget_set_tooltip_text(choice, tapback.verb);
                g_object_set_data_full(G_OBJECT(choice), "body", g_strdup(body), g_free);
                g_signal_connect(choice, "clicked", G_CALLBACK(on_tapback_clicked), const_cast<char*>(tapback.verb));
                gtk_box_pack_start(GTK_BOX(row), choice, FALSE, FALSE, 0);
            }
            gtk_container_add(GTK_CONTAINER(popover), row);
            gtk_widget_show_all(row);
            g_signal_connect(popover, "closed", G_CALLBACK(+[](GtkPopover* p, gpointer) { gtk_widget_destroy(GTK_WIDGET(p)); }), nullptr);
            gtk_popover_popup(GTK_POPOVER(popover));
        }

        GtkWidget* message_action(const char* icon, const char* tip, GCallback handler, const std::string& body) {
            GtkWidget* button = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
            gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
            gtk_widget_set_tooltip_text(button, tip);
            gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
            gtk_style_context_add_class(gtk_widget_get_style_context(button), "tether-message-action");
            g_object_set_data_full(G_OBJECT(button), "body", g_strdup(body.c_str()), g_free);
            g_signal_connect(button, "clicked", handler, nullptr);
            return button;
        }

        // The react and reply buttons fade in beside a bubble under the pointer.
        gboolean on_message_hover(GtkWidget*, GdkEventCrossing* event, gpointer actions) {
            if (event->type == GDK_LEAVE_NOTIFY && event->detail == GDK_NOTIFY_INFERIOR)
                return FALSE;
            gtk_widget_set_opacity(GTK_WIDGET(actions), event->type == GDK_ENTER_NOTIFY ? 1.0 : 0.0);
            return FALSE;
        }

        GtkWidget* build_day_row(int64_t stamp) {
            GtkWidget* row = gtk_list_box_row_new();
            gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);

            GtkWidget* label = gtk_label_new(format_day_heading(stamp, std::time(nullptr)).c_str());
            gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
            gtk_widget_set_margin_top(label, 12);
            gtk_widget_set_margin_bottom(label, 4);
            gtk_style_context_add_class(gtk_widget_get_style_context(label), "muted");

            gtk_container_add(GTK_CONTAINER(row), label);
            return row;
        }

        // Right-click on a message: Copy, and Reply for theirs.
        gboolean on_message_button(GtkWidget* widget, GdkEventButton* event, gpointer) {
            if (event->type != GDK_BUTTON_PRESS || event->button != GDK_BUTTON_SECONDARY)
                return FALSE;
            const char* body = message_body(widget);
            if (!body)
                return FALSE;
            GtkWidget* menu = gtk_menu_new();
            GtkWidget* copy = gtk_menu_item_new_with_label(_("Copy"));
            g_object_set_data_full(G_OBJECT(copy), "body", g_strdup(body), g_free);
            g_signal_connect(copy,
                             "activate",
                             G_CALLBACK(+[](GtkMenuItem* item, gpointer) {
                                 gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),
                                                        (const char*)g_object_get_data(G_OBJECT(item), "body"), -1);
                                 set_status_main(_("Copied to the clipboard."));
                             }),
                             nullptr);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), copy);
            if (!g_object_get_data(G_OBJECT(widget), "outgoing")) {
                GtkWidget* reply = gtk_menu_item_new_with_label(_("Reply"));
                g_object_set_data_full(G_OBJECT(reply), "body", g_strdup(body), g_free);
                g_signal_connect(reply,
                                 "activate",
                                 G_CALLBACK(+[](GtkMenuItem* item, gpointer) {
                                     show_reply_bar((const char*)g_object_get_data(G_OBJECT(item), "body"));
                                 }),
                                 nullptr);
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), reply);
            }
            gtk_widget_show_all(menu);
            g_signal_connect(menu, "deactivate", G_CALLBACK(+[](GtkMenuShell* m, gpointer) {
                                 g_idle_add(+[](gpointer w) -> gboolean { gtk_widget_destroy(GTK_WIDGET(w)); return G_SOURCE_REMOVE; }, m);
                             }), nullptr);
            gtk_menu_popup_at_pointer(GTK_MENU(menu), reinterpret_cast<GdkEvent*>(event));
            return TRUE;
        }

        // ---- pinned conversations ----

        // Pinned first in pin order, then everything else in the daemon's order (newest first).
        gint thread_sort(GtkListBoxRow* a, GtkListBoxRow* b, gpointer) {
            const int pa = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(a), "pin")) - 1;
            const int pb = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "pin")) - 1;
            if ((pa >= 0) != (pb >= 0))
                return pa >= 0 ? -1 : 1;
            if (pa >= 0)
                return pa - pb;
            return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(a), "order")) -
                   GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "order"));
        }

        void thread_header(GtkListBoxRow* row, GtkListBoxRow* before, gpointer) {
            const bool pinned = g_object_get_data(G_OBJECT(row), "pin") != nullptr;
            const bool before_pinned = before && g_object_get_data(G_OBJECT(before), "pin") != nullptr;
            const char* title = nullptr;
            if (pinned && !before)
                title = _("Pinned");
            else if (!pinned && before_pinned)
                title = _("Messages");
            if (!title) {
                gtk_list_box_row_set_header(row, nullptr);
                return;
            }
            GtkWidget* label = gtk_label_new(title);
            gtk_label_set_xalign(GTK_LABEL(label), 0.0);
            gtk_widget_set_margin_start(label, 16);
            gtk_widget_set_margin_top(label, 8);
            gtk_widget_set_margin_bottom(label, 2);
            gtk_style_context_add_class(gtk_widget_get_style_context(label), "tether-section-label");
            gtk_widget_show(label);
            gtk_list_box_row_set_header(row, label);
        }

        void request_threads();

        gboolean on_thread_list_button(GtkWidget* list, GdkEventButton* event, gpointer) {
            if (event->type != GDK_BUTTON_PRESS || event->button != GDK_BUTTON_SECONDARY)
                return FALSE;
            GtkListBoxRow* row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(list), static_cast<int>(event->y));
            const char* thread = row ? (const char*)g_object_get_data(G_OBJECT(row), "thread") : nullptr;
            if (!thread || !*thread)
                return FALSE;
            const bool pinned = thread_pin_index(thread) >= 0;
            GtkWidget* menu = gtk_menu_new();
            GtkWidget* item = gtk_menu_item_new_with_label(pinned ? _("Unpin") : _("Pin"));
            g_object_set_data_full(G_OBJECT(item), "thread", g_strdup(thread), g_free);
            g_signal_connect(item,
                             "activate",
                             G_CALLBACK(+[](GtkMenuItem* i, gpointer) {
                                 const std::string key = (const char*)g_object_get_data(G_OBJECT(i), "thread");
                                 // Unpin by whichever spelling was pinned.
                                 const int at = thread_pin_index(key);
                                 if (at >= 0)
                                     set_pinned(prefs()["pinned"][at].get<std::string>(), false);
                                 else
                                     set_pinned(key, true);
                             }),
                             nullptr);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
            gtk_widget_show_all(menu);
            g_signal_connect(menu, "deactivate", G_CALLBACK(+[](GtkMenuShell* m, gpointer) {
                                 g_idle_add(+[](gpointer w) -> gboolean { gtk_widget_destroy(GTK_WIDGET(w)); return G_SOURCE_REMOVE; }, m);
                             }), nullptr);
            gtk_menu_popup_at_pointer(GTK_MENU(menu), reinterpret_cast<GdkEvent*>(event));
            return TRUE;
        }

        GtkWidget* build_message_row(const nlohmann::json& message, bool show_stamp, bool new_group) {
            const bool outgoing = message.value("outgoing", false);
            const std::string body = message.value("body", "");
            const std::string stamp =
                show_stamp ? format_timestamp(message.value("timestamp", static_cast<int64_t>(0))) : "";
            std::string quote, text;
            parse_reply(body, quote, text);

            GtkWidget* row = gtk_list_box_row_new();
            gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
            gtk_style_context_add_class(gtk_widget_get_style_context(row), "tether-message-row");
            g_object_set_data_full(G_OBJECT(row), "handle", g_strdup(message.value("handle", "").c_str()), g_free);
            g_object_set_data_full(G_OBJECT(row), "search-body", g_strdup(fold(body).c_str()), g_free);

            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            gtk_widget_set_margin_start(box, 14);
            gtk_widget_set_margin_end(box, 14);
            gtk_widget_set_margin_top(box, show_stamp ? 12 : (new_group ? 8 : 1));
            gtk_widget_set_margin_bottom(box, 1);

            // Centred between groups, as iMessage does, rather than pinned to a side.
            if (!stamp.empty()) {
                GtkWidget* time_label = gtk_label_new(stamp.c_str());
                gtk_widget_set_halign(time_label, GTK_ALIGN_CENTER);
                gtk_widget_set_margin_bottom(time_label, 4);
                gtk_style_context_add_class(gtk_widget_get_style_context(time_label), "tether-stamp");
                gtk_box_pack_start(GTK_BOX(box), time_label, FALSE, FALSE, 0);
            }

            const GtkAlign side = outgoing ? GTK_ALIGN_END : GTK_ALIGN_START;
            GtkWidget* column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            gtk_widget_set_halign(column, side);

            // Tapbacks sit on the bubble's outer top corner.
            GtkWidget* reactions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
            gtk_widget_set_halign(reactions, outgoing ? GTK_ALIGN_START : GTK_ALIGN_END);
            gtk_widget_set_no_show_all(reactions, TRUE);
            gtk_box_pack_start(GTK_BOX(column), reactions, FALSE, FALSE, 0);
            g_messages.reaction_slots[body] = reactions;
            if (text != body)
                g_messages.reaction_slots[text] = reactions;

            if (!quote.empty()) {
                GtkWidget* quoted = gtk_label_new(snippet(quote, 80).c_str());
                gtk_label_set_line_wrap(GTK_LABEL(quoted), TRUE);
                gtk_label_set_max_width_chars(GTK_LABEL(quoted), 40);
                gtk_label_set_xalign(GTK_LABEL(quoted), 0.0);
                gtk_widget_set_halign(quoted, side);
                gtk_style_context_add_class(gtk_widget_get_style_context(quoted), "tether-reply-quote");
                gtk_box_pack_start(GTK_BOX(column), quoted, FALSE, FALSE, 0);
            }

            // Picture and GIF links (GIPHY, uploads) show as the picture; other links
            // get a preview card. A line that is nothing but a picture link is dropped
            // from the text, since the picture says it.
            std::string words;
            std::vector<std::string> media;
            std::vector<std::string> cards;
            {
                size_t start = 0;
                while (start <= text.size()) {
                    const size_t nl = text.find('\n', start);
                    const std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
                    if (media_url_for(line).empty())
                        words += (words.empty() ? "" : "\n") + line;
                    if (nl == std::string::npos)
                        break;
                    start = nl + 1;
                }
                for (const auto& url : extract_urls(text)) {
                    const std::string picture = media_url_for(url);
                    auto& list = picture.empty() ? cards : media;
                    const std::string& item = picture.empty() ? url : picture;
                    if (std::find(list.begin(), list.end(), item) == list.end())
                        list.push_back(item);
                }
                if (cards.size() > 2)
                    cards.resize(2);
            }
            for (const auto& url : media) {
                GtkWidget* picture = media_bubble_new(url);
                gtk_widget_set_halign(picture, side);
                gtk_box_pack_start(GTK_BOX(column), picture, FALSE, FALSE, 0);
            }

            GtkWidget* bubble = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(bubble), linkify_markup(words).c_str());
            gtk_label_set_track_visited_links(GTK_LABEL(bubble), FALSE);
            g_signal_connect(bubble, "activate-link", G_CALLBACK(on_bubble_link), nullptr);
            gtk_label_set_line_wrap(GTK_LABEL(bubble), TRUE);
            gtk_label_set_line_wrap_mode(GTK_LABEL(bubble), PANGO_WRAP_WORD_CHAR);
            gtk_label_set_max_width_chars(GTK_LABEL(bubble), 44);
            gtk_label_set_xalign(GTK_LABEL(bubble), 0.0);
            // Not selectable: a text cursor in someone else's message reads as editable.
            // Right-click offers Copy instead.
            // Without this the label stretches to whatever else is in the row and
            // the bubble reads as a full-width bar rather than wrapping the text.
            gtk_widget_set_halign(bubble, side);
            GtkStyleContext* bubble_style = gtk_widget_get_style_context(bubble);
            gtk_style_context_add_class(bubble_style, "tether-bubble");
            gtk_style_context_add_class(bubble_style, outgoing ? "tether-bubble-out" : "tether-bubble-in");
            if (is_emoji_only(words))
                gtk_style_context_add_class(bubble_style, "tether-bubble-emoji");
            // Direction is otherwise only bubble side and color.
            set_accessible_name(bubble,
                                // TRANSLATORS: Read aloud before a message you sent, {} is the message.
                                outgoing ? tether::tr_format(_("Sent: {}"), body)
                                         // TRANSLATORS: Read aloud before a message you received, {} is the message.
                                         : tether::tr_format(_("Received: {}"), body));
            if (!words.empty() || media.empty())
                gtk_box_pack_start(GTK_BOX(column), bubble, FALSE, FALSE, 0);
            else
                g_object_ref_sink(bubble), g_object_unref(bubble);
            for (const auto& url : cards) {
                GtkWidget* card = link_preview_new(url);
                gtk_widget_set_halign(card, side);
                gtk_box_pack_start(GTK_BOX(column), card, FALSE, FALSE, 0);
            }

            // iMessage's "2 Replies" under an original; clicking it carries on the thread.
            GtkWidget* counter = gtk_button_new_with_label("");
            gtk_button_set_relief(GTK_BUTTON(counter), GTK_RELIEF_NONE);
            gtk_widget_set_halign(counter, side);
            gtk_style_context_add_class(gtk_widget_get_style_context(counter), "tether-reply-count");
            g_object_set_data_full(G_OBJECT(counter), "body", g_strdup(text.c_str()), g_free);
            g_signal_connect(counter, "clicked", G_CALLBACK(on_reply_clicked), nullptr);
            gtk_widget_set_no_show_all(counter, TRUE);
            gtk_box_pack_start(GTK_BOX(column), counter, FALSE, FALSE, 0);
            g_messages.reply_counters[text] = counter;
            if (!quote.empty())
                count_reply(quote);

            GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_widget_set_valign(actions, GTK_ALIGN_CENTER);
            gtk_box_pack_start(GTK_BOX(actions),
                               message_action("face-smile-symbolic", _("React"), G_CALLBACK(on_react_clicked), text),
                               FALSE, FALSE, 0);
            // Replies are for what they said; replying to yourself is not offered.
            if (!outgoing)
                gtk_box_pack_start(GTK_BOX(actions),
                                   message_action("mail-reply-sender-symbolic", _("Reply"), G_CALLBACK(on_reply_clicked), text),
                                   FALSE, FALSE, 0);
            gtk_widget_set_opacity(actions, 0.0);

            GtkWidget* line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
            gtk_widget_set_halign(line, side);
            if (outgoing) {
                gtk_box_pack_start(GTK_BOX(line), actions, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(line), column, FALSE, FALSE, 0);
            } else {
                gtk_box_pack_start(GTK_BOX(line), column, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(line), actions, FALSE, FALSE, 0);
            }

            GtkWidget* hover = gtk_event_box_new();
            gtk_widget_add_events(hover, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
            g_signal_connect(hover, "enter-notify-event", G_CALLBACK(on_message_hover), actions);
            g_signal_connect(hover, "leave-notify-event", G_CALLBACK(on_message_hover), actions);
            g_object_set_data_full(G_OBJECT(hover), "body", g_strdup(text.c_str()), g_free);
            g_object_set_data(G_OBJECT(hover), "outgoing", GINT_TO_POINTER(outgoing ? 1 : 0));
            gtk_widget_add_events(hover, GDK_BUTTON_PRESS_MASK);
            g_signal_connect(hover, "button-press-event", G_CALLBACK(on_message_button), nullptr);
            gtk_container_add(GTK_CONTAINER(hover), line);
            gtk_box_pack_start(GTK_BOX(box), hover, FALSE, FALSE, 0);

            // Only the newest outgoing message carries "Sent". MAP says the phone
            // took it; nothing says whether it was delivered or read.
            if (outgoing) {
                if (g_messages.last_status)
                    gtk_widget_destroy(g_messages.last_status);
                g_messages.last_status = gtk_label_new(_("Sent"));
                gtk_widget_set_halign(g_messages.last_status, GTK_ALIGN_END);
                gtk_style_context_add_class(gtk_widget_get_style_context(g_messages.last_status), "tether-stamp");
                g_signal_connect(g_messages.last_status,
                                 "destroy",
                                 G_CALLBACK(+[](GtkWidget* w, gpointer) {
                                     if (g_messages.last_status == w)
                                         g_messages.last_status = nullptr;
                                 }),
                                 nullptr);
                gtk_box_pack_start(GTK_BOX(box), g_messages.last_status, FALSE, FALSE, 0);
            }

            gtk_container_add(GTK_CONTAINER(row), box);
            return row;
        }

        GtkAdjustment* conversation_adjustment() {
            if (!g_messages.conversation_scroll)
                return nullptr;
            return gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(g_messages.conversation_scroll));
        }

        // upper minus the visible page is the bottom edge
        double conversation_bottom(GtkAdjustment* adjustment) {
            return gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment);
        }

        bool conversation_at_bottom() {
            GtkAdjustment* adjustment = conversation_adjustment();
            if (!adjustment)
                return true;
            return gtk_adjustment_get_value(adjustment) >= conversation_bottom(adjustment) - AT_BOTTOM_SLACK;
        }

        bool aim_at_jump_row();
        void end_jump();
        gboolean load_older_page(gpointer);

        // GTK only updates the adjustment once it has laid the new rows out, and
        // its layout runs on the frame clock, after any idle this could post.
        void on_conversation_changed(GtkAdjustment* adjustment, gpointer) {
            const double bottom = conversation_bottom(adjustment);
            g_messages.scroll_last_bottom = bottom;
            if (g_messages.jump_row) {
                aim_at_jump_row();
                return;
            }
            if (g_messages.scroll_pin) {
                gtk_adjustment_set_value(adjustment, bottom);
            } else if (g_messages.scroll_restore) {
                g_messages.scroll_restore = false;
                gtk_adjustment_set_value(adjustment, bottom - g_messages.scroll_from_bottom);
            }
        }

        // A value that moves while the content height is unchanged is the user.
        void on_conversation_value_changed(GtkAdjustment* adjustment, gpointer) {
            const double bottom = conversation_bottom(adjustment);
            if (bottom == g_messages.scroll_last_bottom) {
                if (g_messages.jump_row && g_messages.jump_timer) {
                    // Our own aim lands here too; only a move away from the row is the user.
                    GtkAllocation a;
                    gtk_widget_get_allocation(g_messages.jump_row, &a);
                    const double value = gtk_adjustment_get_value(adjustment);
                    const double page = gtk_adjustment_get_page_size(adjustment);
                    if (a.y < value - 10 || a.y > value + page + 10)
                        end_jump();
                }
                g_messages.scroll_pin = conversation_at_bottom();
                if (gtk_adjustment_get_value(adjustment) < 400 && g_messages.window_start > 0 &&
                    !g_messages.rendered.empty() && !g_messages.loading_older && !g_messages.jump_row) {
                    g_messages.loading_older = true;
                    g_idle_add(load_older_page, nullptr);
                }
                g_messages.scroll_restore = false;
            }
            g_messages.scroll_last_bottom = bottom;
        }

        void restore_conversation_scroll(bool pin) {
            g_messages.scroll_pin = pin;
            g_messages.scroll_restore = !pin;
        }

        gboolean thread_scroll_idle(gpointer) {
            g_messages.thread_scroll_idle_id = 0;
            if (g_messages.thread_scroll_value < 0.0 || !g_messages.thread_scroll)
                return G_SOURCE_REMOVE;
            if (GtkAdjustment* adjustment =
                    gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(g_messages.thread_scroll)))
                gtk_adjustment_set_value(adjustment, g_messages.thread_scroll_value);
            g_messages.thread_scroll_value = -1.0;
            return G_SOURCE_REMOVE;
        }

        void restore_thread_scroll() {
            if (g_messages.thread_scroll_value >= 0.0 && g_messages.thread_scroll_idle_id == 0)
                g_messages.thread_scroll_idle_id =
                    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, thread_scroll_idle, nullptr, nullptr);
        }

        gboolean thread_visible(GtkListBoxRow* row, gpointer) {
            if (g_messages.search_needle.empty())
                return TRUE;
            const char* haystack = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "search"));
            return haystack && std::strstr(haystack, g_messages.search_needle.c_str()) != nullptr;
        }

        void request_history_search();

        void on_search_changed(GtkSearchEntry* entry, gpointer) {
            g_messages.search_needle = fold(gtk_entry_get_text(GTK_ENTRY(entry)));
            gtk_list_box_invalidate_filter(GTK_LIST_BOX(g_messages.thread_list));
            request_history_search();
        }

        // One spelled several ways ("+15551234567", "5551234567"), daemon puts in one bucket.
        // The UI has to agree.
        bool same_thread(const std::string& a, const std::string& b) {
            if (a.empty() || b.empty())
                return a == b;
            return bluetooth::thread_bucket(a) == bluetooth::thread_bucket(b);
        }

        GtkWidget* find_thread_row(const std::string& thread_key) {
            for (int index = 0;; ++index) {
                GtkListBoxRow* row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(g_messages.thread_list), index);
                if (!row)
                    return nullptr;
                const char* key = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "thread"));
                if (key && same_thread(key, thread_key))
                    return GTK_WIDGET(row);
            }
        }

        void render_threads(const nlohmann::json& event);

        // The daemon sends the thread list after every message, read and refresh,
        // often several in a row. Identical lists are skipped and a burst is drawn
        // once, shortly after it settles.
        void show_threads(const nlohmann::json& event) {
            if (!event.contains("threads") || !event["threads"].is_array())
                return;
            static std::string last_drawn;
            static nlohmann::json pending;
            static guint timer = 0;
            const std::string dump = event["threads"].dump();
            if (dump == last_drawn && !timer && !g_messages.threads_dirty)
                return;
            g_messages.threads_dirty = false;
            pending = event;
            if (timer)
                return;
            const bool first = last_drawn.empty();
            last_drawn = dump;
            timer = g_timeout_add(first ? 0 : 80,
                                  +[](gpointer) -> gboolean {
                                      timer = 0;
                                      last_drawn = pending["threads"].dump();
                                      render_threads(pending);
                                      return G_SOURCE_REMOVE;
                                  },
                                  nullptr);
        }

        void render_threads(const nlohmann::json& event) {

            // Clearing the list destroys the selected row, which fires
            // row-selected(nullptr) and then again for the row that replaces it.
            // Unblocked, that reads as the user opening a conversation: it
            // re-requests the messages, which re-sends their mark-read requests,
            // which produce another refresh — a loop that never settles.
            if (g_messages.thread_selected_handler)
                g_signal_handler_block(g_messages.thread_list, g_messages.thread_selected_handler);

            if (GtkAdjustment* adjustment =
                    gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(g_messages.thread_scroll)))
                g_messages.thread_scroll_value = gtk_adjustment_get_value(adjustment);

            clear_list_box(g_messages.thread_list);

            g_messages.threads_known = true;
            g_messages.thread_count = event["threads"].size();
            update_placeholder();

            GtkWidget* reselect = nullptr;
            int order = 0;
            for (const auto& thread : event["threads"]) {
                GtkWidget* row = build_thread_row(thread);
                g_object_set_data(G_OBJECT(row), "order", GINT_TO_POINTER(++order));
                gtk_list_box_insert(GTK_LIST_BOX(g_messages.thread_list), row, -1);
                if (same_thread(thread.value("thread", ""), g_messages.selected_thread))
                    reselect = row;
            }
            gtk_widget_show_all(g_messages.thread_list);

            if (reselect) {
                g_messages.pending_new_thread.clear();
                gtk_list_box_select_row(GTK_LIST_BOX(g_messages.thread_list), GTK_LIST_BOX_ROW(reselect));
                if (!g_messages.composing)
                    apply_row_selection(reselect);
            } else if (!g_messages.selected_thread.empty() && !g_messages.composing) {
                bluetooth::Recipient recipient;
                std::string err;
                const bool wanted = same_thread(g_messages.pending_new_thread, g_messages.selected_thread) &&
                                    bluetooth::recipient_from_thread_key(g_messages.selected_thread, recipient, err);
                g_messages.pending_new_thread.clear();
                if (wanted) {
                    enter_compose();
                    gtk_entry_set_text(GTK_ENTRY(g_messages.compose_entry), recipient.address.c_str());
                } else {
                    clear_selection();
                }
            }

            if (g_messages.thread_selected_handler)
                g_signal_handler_unblock(g_messages.thread_list, g_messages.thread_selected_handler);

            restore_thread_scroll();

            if (g_messages.focus_composer) {
                g_messages.focus_composer = false;
                focus_composer_soon();
            }
        }

        // Opening a conversation marks it read here and on the phone, which is
        // what makes the unread badge agree with what the user has seen.
        void mark_conversation_read(const nlohmann::json& messages) {
            std::vector<std::string> pending;
            for (const auto& message : messages) {
                if (message.value("read", true) || message.value("outgoing", false))
                    continue;
                const std::string handle = message.value("handle", "");
                if (!handle.empty() && !g_messages.marked_read.count(handle))
                    pending.push_back(handle);
            }
            if (pending.empty())
                return;

            nlohmann::json j;
            j["command"] = "bt_mark_read";
            j["handles"] = pending;
            j["read"] = true;

            if (daemon_send(j))
                g_messages.marked_read.insert(pending.begin(), pending.end());
        }

        void drop_pending_row(const std::string& body);

        void append_message_rows(const nlohmann::json& messages, size_t from, int insert_at = -1) {
            // -1 appends; a position prepends an older page in order at the top.
            int position = insert_at;
            auto insert = [&](GtkWidget* row) {
                gtk_list_box_insert(GTK_LIST_BOX(g_messages.conversation), row, position);
                if (position >= 0)
                    ++position;
            };
            for (size_t i = from; i < messages.size(); ++i) {
                const auto& message = messages[i];
                const int64_t stamp = message.value("timestamp", static_cast<int64_t>(0));
                const bool outgoing = message.value("outgoing", false);
                const bool first = from == 0 && i == 0;

                if (stamp > 0 && (first || !same_local_day(g_messages.rendered_last_stamp, stamp))) {
                    GtkWidget* day = build_day_row(stamp);
                    g_object_set_data(G_OBJECT(day), "day-stamp", GINT_TO_POINTER(1));
                    g_object_set_data_full(G_OBJECT(day), "stamp", g_strdup(std::to_string(stamp).c_str()), g_free);
                    insert(day);
                }

                const bool grouped = !first && outgoing == g_messages.rendered_last_outgoing &&
                                     stamp - g_messages.rendered_last_stamp < GROUP_WINDOW_SECONDS &&
                                     same_local_day(g_messages.rendered_last_stamp, stamp);

                std::string emoji, target;
                if (parse_tapback(message.value("body", ""), emoji, target) &&
                    g_messages.reaction_slots.count(target)) {
                    add_reaction(target, emoji);
                    continue;
                }

                // iMessage only stamps after a pause, not every time the sender changes.
                const bool pause = first || stamp - g_messages.rendered_last_stamp >= STAMP_GAP_SECONDS ||
                                   !same_local_day(g_messages.rendered_last_stamp, stamp);
                if (outgoing)
                    drop_pending_row(message.value("body", ""));
                insert(build_message_row(message, pause, !grouped));
                g_messages.rendered_last_stamp = stamp;
                g_messages.rendered_last_outgoing = outgoing;
            }
        }

        // Centres `row` in the conversation and flashes it. Layout may not have run
        // yet for freshly rendered rows, so it retries briefly until it has.
        // Puts the jump target in the middle of the view. Returns false until the
        // row has been laid out.
        bool aim_at_jump_row() {
            GtkAdjustment* adjustment = conversation_adjustment();
            GtkWidget* row = g_messages.jump_row;
            if (!adjustment || !row)
                return false;
            GtkAllocation a;
            gtk_widget_get_allocation(row, &a);
            if (a.height <= 1)
                return false;
            const double page = gtk_adjustment_get_page_size(adjustment);
            const double target = std::clamp(a.y - (page - a.height) / 2.0, 0.0, conversation_bottom(adjustment));
            gtk_adjustment_set_value(adjustment, target);
            // The settle window starts from the first real aim, not from the click:
            // laying out a long conversation can take longer than the window itself.
            if (!g_messages.jump_timer)
                g_messages.jump_timer = g_timeout_add(5000,
                                                      +[](gpointer) -> gboolean {
                                                          g_messages.jump_timer = 0;
                                                          end_jump();
                                                          return G_SOURCE_REMOVE;
                                                      },
                                                      nullptr);
            return true;
        }

        void end_jump() {
            if (g_messages.jump_row)
                g_object_unref(g_messages.jump_row);
            g_messages.jump_row = nullptr;
            if (g_messages.jump_timer)
                g_source_remove(g_messages.jump_timer);
            g_messages.jump_timer = 0;
        }

        // Centres `row` and flashes it. A long conversation keeps laying out for a
        // moment after it renders, so the view re-aims at the row on every layout
        // change for two seconds (see on_conversation_changed).
        void scroll_to_row(GtkWidget* row) {
            end_jump();
            // Jumping back means the view must stop following new messages.
            g_messages.scroll_pin = false;
            g_messages.scroll_restore = false;
            g_messages.jump_row = GTK_WIDGET(g_object_ref(row));
            aim_at_jump_row();

            GtkStyleContext* style = gtk_widget_get_style_context(row);
            gtk_style_context_add_class(style, "tether-search-hit");
            g_object_ref(row);
            g_timeout_add(3500,
                          +[](gpointer r) -> gboolean {
                              gtk_style_context_remove_class(gtk_widget_get_style_context(GTK_WIDGET(r)), "tether-search-hit");
                              g_object_unref(r);
                              return G_SOURCE_REMOVE;
                          },
                          row);
        }

        size_t message_index(const std::string& handle);
        void render_window(size_t start);

        bool jump_to_handle(const std::string& handle) {
            GList* rows = gtk_container_get_children(GTK_CONTAINER(g_messages.conversation));
            GtkWidget* found = nullptr;
            for (GList* it = rows; it && !found; it = it->next) {
                const char* h = (const char*)g_object_get_data(G_OBJECT(it->data), "handle");
                if (h && handle == h)
                    found = GTK_WIDGET(it->data);
            }
            g_list_free(rows);
            if (!found) {
                const size_t at = message_index(handle);
                if (at != std::string::npos && at < g_messages.window_start) {
                    render_window(at > 20 ? at - 20 : 0);
                    return jump_to_handle(handle);
                }
            }
            if (found)
                scroll_to_row(found);
            return found != nullptr;
        }

        // Messages built per page. A row costs about a millisecond to build, so
        // drawing a 1500-message conversation at once froze the window for seconds.
        constexpr size_t MESSAGE_PAGE = 150;

        nlohmann::json message_slice(size_t start) {
            nlohmann::json view = nlohmann::json::array();
            for (size_t i = start; i < g_messages.all_messages.size(); ++i)
                view.push_back(g_messages.all_messages[i]);
            return view;
        }

        // Rebuilds the conversation from message `start` to the newest.
        void render_window(size_t start) {
            end_jump();
            clear_list_box(g_messages.conversation);
            g_messages.reaction_slots.clear();
            g_messages.reply_counters.clear();
            g_messages.rendered_last_stamp = 0;
            g_messages.rendered_last_outgoing = false;
            g_messages.window_start = std::min(start, g_messages.all_messages.size());
            append_message_rows(message_slice(g_messages.window_start), 0);
            gtk_widget_show_all(g_messages.conversation);
        }

        size_t message_index(const std::string& handle) {
            for (size_t i = 0; i < g_messages.all_messages.size(); ++i)
                if (g_messages.all_messages[i].value("handle", "") == handle)
                    return i;
            return std::string::npos;
        }

        // Scrolled near the top: draw the previous page above, keeping the view where it was.
        gboolean load_older_page(gpointer) {
            if (g_messages.window_start > 0 && !g_messages.rendered.empty()) {
                if (GtkAdjustment* adjustment = conversation_adjustment())
                    g_messages.scroll_from_bottom =
                        conversation_bottom(adjustment) - gtk_adjustment_get_value(adjustment);
                const size_t old_start = g_messages.window_start;
                const size_t start = old_start > MESSAGE_PAGE ? old_start - MESSAGE_PAGE : 0;
                nlohmann::json page = nlohmann::json::array();
                for (size_t i = start; i < old_start; ++i)
                    page.push_back(g_messages.all_messages[i]);

                // Built on its own grouping state, then the newer state is put back.
                const int64_t saved_stamp = g_messages.rendered_last_stamp;
                const bool saved_outgoing = g_messages.rendered_last_outgoing;
                g_messages.rendered_last_stamp = 0;
                g_messages.rendered_last_outgoing = false;

                // The old top row's day heading repeats if the new page ends on the same day.
                GtkListBoxRow* old_top = gtk_list_box_get_row_at_index(GTK_LIST_BOX(g_messages.conversation), 0);
                append_message_rows(page, 0, 0);
                if (old_top && g_object_get_data(G_OBJECT(old_top), "day-stamp")) {
                    const char* top_stamp = (const char*)g_object_get_data(G_OBJECT(old_top), "stamp");
                    if (top_stamp && same_local_day(g_messages.rendered_last_stamp, std::atoll(top_stamp)))
                        gtk_widget_destroy(GTK_WIDGET(old_top));
                }
                g_messages.rendered_last_stamp = saved_stamp;
                g_messages.rendered_last_outgoing = saved_outgoing;
                g_messages.window_start = start;
                gtk_widget_show_all(g_messages.conversation);
                restore_conversation_scroll(false);
            }
            g_messages.loading_older = false;
            return G_SOURCE_REMOVE;
        }

        void show_messages(const nlohmann::json& event) {
            if (!same_thread(event.value("thread", ""), g_messages.selected_thread))
                return;

            static const nlohmann::json none = nlohmann::json::array();
            const nlohmann::json& messages =
                (event.contains("messages") && event["messages"].is_array()) ? event["messages"] : none;

            mark_conversation_read(messages);

            std::vector<std::string> handles;
            handles.reserve(messages.size());
            for (const auto& message : messages)
                handles.push_back(message.value("handle", ""));

            if (handles == g_messages.rendered)
                return;

            const bool opening = g_messages.rendered.empty();
            const bool pinned = g_messages.pin_next || opening || conversation_at_bottom();
            g_messages.pin_next = false;

            const bool appended = !opening && handles.size() > g_messages.rendered.size() &&
                                  std::equal(g_messages.rendered.begin(), g_messages.rendered.end(), handles.begin());
            const size_t previous = g_messages.rendered.size();
            g_messages.all_messages = messages;

            if (appended) {
                // New messages at the end: build just those.
                append_message_rows(message_slice(g_messages.window_start), previous - g_messages.window_start);
                gtk_widget_show_all(g_messages.conversation);
            } else {
                if (GtkAdjustment* adjustment = conversation_adjustment())
                    g_messages.scroll_from_bottom =
                        conversation_bottom(adjustment) - gtk_adjustment_get_value(adjustment);
                const size_t n = messages.size();
                size_t start = n > MESSAGE_PAGE ? n - MESSAGE_PAGE : 0;
                // A refresh keeps the older pages already scrolled into.
                if (!opening)
                    start = std::min(start, g_messages.window_start);
                // A search jump into older history draws from a little before it.
                if (!g_messages.pending_jump.empty()) {
                    const size_t at = message_index(g_messages.pending_jump);
                    if (at != std::string::npos && at < start)
                        start = at > 20 ? at - 20 : 0;
                }
                render_window(start);
            }
            g_messages.rendered = handles;

            if (pinned || !appended)
                restore_conversation_scroll(pinned);

            if (!g_messages.pending_jump.empty()) {
                const std::string handle = std::move(g_messages.pending_jump);
                g_messages.pending_jump.clear();
                if (!jump_to_handle(handle))
                    set_status_main(_("That message is older than what is loaded for this conversation."));
            }
        }

        // ---- history search ----

        std::string lower_ascii(std::string text) {
            for (char& c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        // A window of the message around the first match, the match in bold.
        std::string search_snippet_markup(const std::string& body, const std::string& query) {
            std::string flat = body;
            for (char& c : flat)
                if (c == '\n')
                    c = ' ';
            const size_t at = lower_ascii(flat).find(lower_ascii(query));
            if (at == std::string::npos)
                return escape_markup(snippet(flat, 90));
            size_t start = at > 40 ? at - 40 : 0;
            while (start > 0 && (static_cast<unsigned char>(flat[start]) & 0xC0) == 0x80)
                --start;
            std::string before = flat.substr(start, at - start);
            std::string match = flat.substr(at, query.size());
            std::string after = snippet(flat.substr(at + query.size()), 60);
            return (start > 0 ? "…" : "") + escape_markup(before) + "<b>" + escape_markup(match) + "</b>" +
                   escape_markup(after);
        }

        void on_result_activated(GtkListBox*, GtkListBoxRow* row, gpointer) {
            const char* thread = (const char*)g_object_get_data(G_OBJECT(row), "thread");
            const char* handle = (const char*)g_object_get_data(G_OBJECT(row), "handle");
            if (!thread || !handle)
                return;
            if (same_thread(thread, g_messages.selected_thread) && jump_to_handle(handle))
                return;
            g_messages.pending_jump = handle;
            messages_view_open_thread(thread);
        }

        void show_search_results(const nlohmann::json& event) {
            if (!g_messages.results_list)
                return;
            const std::string query = event.value("query", "");
            // A slower answer for an older query must not replace a newer one.
            if (fold(query) != g_messages.search_needle)
                return;
            clear_list_box(g_messages.results_list);
            const auto& results = event.contains("results") ? event["results"] : nlohmann::json::array();
            const int n = static_cast<int>(results.size());
            gtk_label_set_text(GTK_LABEL(g_messages.results_header),
                               n == 0 ? _("No messages match")
                                      : tether::tr_format(P_("{} message", "{} messages", n), n).c_str());
            gtk_widget_show(g_messages.results_header);
            for (const auto& result : results) {
                const std::string thread = result.value("thread", "");
                std::string name = result.value("name", "");
                if (name.empty())
                    name = result.value("address", "");
                if (name.empty())
                    name = thread.substr(thread.find(':') + 1);
                GtkWidget* row = gtk_list_box_row_new();
                g_object_set_data_full(G_OBJECT(row), "thread", g_strdup(thread.c_str()), g_free);
                g_object_set_data_full(G_OBJECT(row), "handle", g_strdup(result.value("handle", "").c_str()), g_free);
                GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
                gtk_container_set_border_width(GTK_CONTAINER(box), 8);
                gtk_box_pack_start(GTK_BOX(box), avatar_new(result.value("photo", ""), name, 32), FALSE, FALSE, 0);
                GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
                GtkWidget* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
                GtkWidget* who = gtk_label_new(nullptr);
                const std::string author = result.value("outgoing", false) ? std::string(_("You")) + " → " + name : name;
                gtk_label_set_markup(GTK_LABEL(who), ("<b>" + escape_markup(author) + "</b>").c_str());
                gtk_label_set_ellipsize(GTK_LABEL(who), PANGO_ELLIPSIZE_END);
                gtk_label_set_xalign(GTK_LABEL(who), 0.0);
                gtk_box_pack_start(GTK_BOX(top), who, TRUE, TRUE, 0);
                GtkWidget* when = gtk_label_new(
                    format_thread_time(result.value("timestamp", static_cast<int64_t>(0)), std::time(nullptr)).c_str());
                gtk_style_context_add_class(gtk_widget_get_style_context(when), "muted");
                gtk_box_pack_start(GTK_BOX(top), when, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(text), top, FALSE, FALSE, 0);
                GtkWidget* line = gtk_label_new(nullptr);
                gtk_label_set_markup(GTK_LABEL(line), search_snippet_markup(result.value("body", ""), query).c_str());
                gtk_label_set_xalign(GTK_LABEL(line), 0.0);
                gtk_label_set_line_wrap(GTK_LABEL(line), TRUE);
                gtk_label_set_lines(GTK_LABEL(line), 2);
                gtk_label_set_ellipsize(GTK_LABEL(line), PANGO_ELLIPSIZE_END);
                gtk_label_set_max_width_chars(GTK_LABEL(line), 30);
                gtk_style_context_add_class(gtk_widget_get_style_context(line), "muted");
                gtk_box_pack_start(GTK_BOX(text), line, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(box), text, TRUE, TRUE, 0);
                gtk_container_add(GTK_CONTAINER(row), box);
                gtk_list_box_insert(GTK_LIST_BOX(g_messages.results_list), row, -1);
            }
            gtk_widget_show_all(g_messages.results_list);
        }

        void request_history_search() {
            if (g_messages.search_timer) {
                g_source_remove(g_messages.search_timer);
                g_messages.search_timer = 0;
            }
            const std::string query = g_messages.search_entry ? gtk_entry_get_text(GTK_ENTRY(g_messages.search_entry)) : "";
            if (g_utf8_strlen(query.c_str(), -1) < 2) {
                if (g_messages.results_list) {
                    clear_list_box(g_messages.results_list);
                    gtk_widget_hide(g_messages.results_header);
                }
                return;
            }
            // Waits for a pause in typing, so it is one search per word, not per key.
            g_messages.search_timer = g_timeout_add(250,
                                                    +[](gpointer) -> gboolean {
                                                        g_messages.search_timer = 0;
                                                        nlohmann::json j;
                                                        j["command"] = "bt_search_messages";
                                                        j["query"] = gtk_entry_get_text(GTK_ENTRY(g_messages.search_entry));
                                                        daemon_send(j);
                                                        return G_SOURCE_REMOVE;
                                                    },
                                                    nullptr);
        }

        // ---- find in the open conversation ----

        void find_select(int index) {
            if (g_messages.find_hits.empty()) {
                gtk_label_set_text(GTK_LABEL(g_messages.find_count), _("No matches"));
                return;
            }
            const int n = static_cast<int>(g_messages.find_hits.size());
            g_messages.find_index = (index % n + n) % n;
            gtk_label_set_text(GTK_LABEL(g_messages.find_count),
                               tether::tr_format(_("{} of {}"), n - g_messages.find_index, n).c_str());
            scroll_to_row(g_messages.find_hits[g_messages.find_index]);
        }

        void find_refresh() {
            g_messages.find_hits.clear();
            // Covers the whole loaded conversation: draw back only as far as the oldest match.
            {
                const std::string wanted = fold(gtk_entry_get_text(GTK_ENTRY(g_messages.find_entry)));
                if (wanted.size() >= 2) {
                    for (size_t i = 0; i < g_messages.window_start; ++i) {
                        if (fold(g_messages.all_messages[i].value("body", "")).find(wanted) != std::string::npos) {
                            render_window(i);
                            break;
                        }
                    }
                }
            }
            const std::string needle = fold(gtk_entry_get_text(GTK_ENTRY(g_messages.find_entry)));
            if (needle.size() < 2) {
                gtk_label_set_text(GTK_LABEL(g_messages.find_count), "");
                return;
            }
            GList* rows = gtk_container_get_children(GTK_CONTAINER(g_messages.conversation));
            for (GList* it = rows; it; it = it->next) {
                const char* body = (const char*)g_object_get_data(G_OBJECT(it->data), "search-body");
                if (body && std::strstr(body, needle.c_str()))
                    g_messages.find_hits.push_back(GTK_WIDGET(it->data));
            }
            g_list_free(rows);
            // Newest match first, as iMessage does; Up walks back in time.
            find_select(static_cast<int>(g_messages.find_hits.size()) - 1);
        }

        void open_find_bar() {
            if (!g_messages.find_bar || g_messages.selected_thread.empty())
                return;
            gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(g_messages.find_bar), TRUE);
            gtk_widget_grab_focus(g_messages.find_entry);
        }

        void set_banner(const std::string& text, bool offer_permissions) {
            if (!g_messages.banner)
                return;
            if (text.empty()) {
                gtk_widget_hide(g_messages.banner);
                return;
            }
            set_text(g_messages.banner_label, text);
            gtk_widget_show_all(g_messages.banner);
            // show_all revealed it; hide it again unless re-soliciting is the
            // thing that would actually help here.
            gtk_widget_set_visible(g_messages.banner_action, offer_permissions);
        }

        // The advertisement that makes iOS reveal its Messages and Contacts
        // permission toggles expires a few minutes after pairing. Without this the
        // only way back to those toggles is to remove the bond and pair again.
        void on_solicit_clicked(GtkWidget*, gpointer) {
            if (!daemon_send({{"command", "bt_solicit"}})) {
                set_status_main(_("Could not reach the Tether daemon."));
                return;
            }
            set_status_main(_("Asking the iPhone to show its Bluetooth permissions…"));
        }

        void update_connection(const nlohmann::json& event) {
            const bool map_open = event.value("map_open", false);
            g_messages.map_open = map_open;
            update_placeholder();
            update_composer_sensitivity();
            if (map_open) {
                set_banner("");
                if (g_messages.visible)
                    request_threads();
                return;
            }
            // The daemon's reason strings name the actual next step, including
            // which toggle to flip on the phone, so they are shown verbatim
            // rather than replaced with something vaguer.
            std::string reason = event.value("profile_reason", "");
            if (reason.empty())
                reason = event.value("link_reason", "");
            if (reason.empty())
                reason = _("Messages are not connected.");

            // Re-soliciting only helps when the phone is withholding the profile,
            // not when the link itself is down.
            const std::string map_error = event.value("map_error", "none");
            set_banner(reason, map_error == "forbidden" || map_error == "no_record");
        }

        void update_placeholder() {
            if (!g_messages.placeholder_icon || !g_messages.placeholder_label)
                return;

            const bool bluetooth_needed =
                g_messages.threads_known && g_messages.thread_count == 0 && !g_messages.map_open;
            gtk_image_set_from_icon_name(GTK_IMAGE(g_messages.placeholder_icon),
                                         bluetooth_needed ? "bluetooth-disabled-symbolic" : "mail-unread-symbolic",
                                         GTK_ICON_SIZE_DIALOG);
            set_text(g_messages.placeholder_label,
                     bluetooth_needed ? _("Bluetooth connection needed to sync messages.")
                                      : _("Select a conversation"));
        }

        // Replying needs an open conversation and a live MAP session. A group
        // thread has no reply routing yet, and the daemon refuses one anyway, so
        // the composer stays shut rather than accepting text that will bounce.
        void update_composer_sensitivity() {
            const bool can_send =
                g_messages.map_open && !g_messages.selected_thread.empty() && g_messages.selected_repliable;
            if (!g_messages.composer)
                return;

            // The box stays typable whenever the conversation can take a reply;
            // only the button waits for there to be something in it.
            // A send in flight never locks the box: messages queue behind it.
            const bool composer_live = can_send && !g_messages.uploading;
            gtk_widget_set_sensitive(g_messages.composer, composer_live);
            const bool has_content = !composer_text().empty() || !g_messages.attachments.empty();
            gtk_widget_set_sensitive(g_messages.send_button, composer_live && has_content);
            // Discord's composer: the send plane appears once there is something to send.
            gtk_widget_set_visible(g_messages.send_button, has_content);

            const char* reason = nullptr;
            if (g_messages.uploading)
                reason = _("Uploading…");
            else if (g_messages.sending)
                reason = _("Sending…");
            else if (!g_messages.map_open)
                reason = _("Messages are not connected.");
            else if (g_messages.selected_thread.empty())
                reason = g_messages.composing
                             ? (g_messages.selected_block_reason.empty() ? _("Enter a recipient.")
                                                                         : g_messages.selected_block_reason.c_str())
                             : _("Select a conversation first.");
            else if (!g_messages.selected_block_reason.empty())
                reason = g_messages.selected_block_reason.c_str();
            else if (!can_send)
                reason = _("Replying to this conversation is not available.");
            gtk_widget_set_tooltip_text(g_messages.send_button, reason);

            if (g_messages.composer_notice) {
                // Why the box is shut.
                const char* notice = nullptr;
                if (reason && !composer_live && !g_messages.uploading)
                    notice = reason;
                if (notice)
                    set_text(g_messages.composer_notice, notice);
                gtk_widget_set_visible(g_messages.composer_notice, notice != nullptr);
            }
        }

        std::string composer_text() {
            GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_messages.composer));
            GtkTextIter start, end;
            gtk_text_buffer_get_bounds(buffer, &start, &end);
            gchar* text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
            std::string out = text ? text : "";
            g_free(text);
            return out;
        }

        void hide_send_error() {
            if (g_messages.send_error)
                gtk_widget_hide(g_messages.send_error);
        }

        void show_send_error(const std::string& text) {
            if (!g_messages.send_error)
                return;
            set_text(g_messages.send_error_label, text);
            gtk_widget_show(g_messages.send_error);
        }

        void clear_sending() {
            if (g_messages.send_watchdog_id != 0) {
                g_source_remove(g_messages.send_watchdog_id);
                g_messages.send_watchdog_id = 0;
            }
            g_messages.sending = false;
        }

        void finish_front_send(bool ok, const std::string& reason);
        void pump_outbox();

        gboolean on_send_timeout(gpointer) {
            g_messages.send_watchdog_id = 0;
            if (g_messages.sending) {
                // Probably sent; keep its bubble rather than offering a duplicate.
                finish_front_send(true, "");
                set_status_main(_("No answer about that message; it may still have been sent."));
            }
            return G_SOURCE_REMOVE;
        }

        // catbox.moe: no account, unlisted random URLs, kept until deleted.
        // Tried in order. Some networks refuse catbox.moe outright, so a second
        // public host keeps attachments working there. Both return a bare URL.
        struct UploadHost {
            const char* url;
            const char* field;
            const char* extra;  // an additional -F field, or null
        };
        constexpr UploadHost UPLOAD_HOSTS[] = {
            {"https://catbox.moe/user/api.php", "fileToUpload", "reqtype=fileupload"},
            {"https://0x0.st", "file", nullptr},
        };
        constexpr goffset UPLOAD_MAX_BYTES = 200LL * 1024 * 1024;

        void forget_pasted(const std::string& path) {
            if (g_messages.pasted.erase(path))
                g_remove(path.c_str());
        }

        void rebuild_attach_bar() {
            if (!g_messages.attach_bar)
                return;
            GList* children = gtk_container_get_children(GTK_CONTAINER(g_messages.attach_chips));
            for (GList* it = children; it; it = it->next)
                gtk_widget_destroy(GTK_WIDGET(it->data));
            g_list_free(children);

            for (const auto& path : g_messages.attachments) {
                GtkWidget* chip = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
                gtk_style_context_add_class(gtk_widget_get_style_context(chip), "tether-attachment");
                GtkWidget* overlay = gtk_overlay_new();
                GdkPixbuf* thumb = gdk_pixbuf_new_from_file_at_scale(path.c_str(), 72, 72, TRUE, nullptr);
                GtkWidget* picture = thumb ? gtk_image_new_from_pixbuf(thumb)
                                           : gtk_image_new_from_icon_name("text-x-generic", GTK_ICON_SIZE_DIALOG);
                if (thumb)
                    g_object_unref(thumb);
                gtk_widget_set_size_request(picture, 72, 72);
                gtk_container_add(GTK_CONTAINER(overlay), picture);

                GtkWidget* remove = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
                gtk_widget_set_tooltip_text(remove, _("Remove"));
                gtk_widget_set_halign(remove, GTK_ALIGN_END);
                gtk_widget_set_valign(remove, GTK_ALIGN_START);
                gtk_style_context_add_class(gtk_widget_get_style_context(remove), "tether-attachment-remove");
                g_object_set_data_full(G_OBJECT(remove), "path", g_strdup(path.c_str()), g_free);
                g_signal_connect(remove,
                                 "clicked",
                                 G_CALLBACK(+[](GtkButton* button, gpointer) {
                                     if (g_messages.uploading)
                                         return;
                                     const std::string path = (const char*)g_object_get_data(G_OBJECT(button), "path");
                                     auto& list = g_messages.attachments;
                                     list.erase(std::remove(list.begin(), list.end(), path), list.end());
                                     forget_pasted(path);
                                     rebuild_attach_bar();
                                     update_composer_sensitivity();
                                 }),
                                 nullptr);
                gtk_overlay_add_overlay(GTK_OVERLAY(overlay), remove);
                gtk_box_pack_start(GTK_BOX(chip), overlay, FALSE, FALSE, 0);

                gchar* base = g_path_get_basename(path.c_str());
                GtkWidget* name = gtk_label_new(g_messages.pasted.count(path) ? _("Pasted image") : base);
                g_free(base);
                gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
                gtk_label_set_max_width_chars(GTK_LABEL(name), 10);
                gtk_style_context_add_class(gtk_widget_get_style_context(name), "muted");
                gtk_box_pack_start(GTK_BOX(chip), name, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(g_messages.attach_chips), chip, FALSE, FALSE, 0);
            }
            gtk_widget_show_all(g_messages.attach_chips);
            gtk_widget_set_visible(g_messages.attach_bar, !g_messages.attachments.empty());
        }

        void clear_attachments() {
            // An upload in flight still reads these files; it clears them itself.
            if (g_messages.uploading || g_messages.attachments.empty())
                return;
            for (const auto& path : g_messages.attachments)
                forget_pasted(path);
            g_messages.attachments.clear();
            rebuild_attach_bar();
        }

        void add_attachment(const std::string& path) {
            if (path.empty() || g_messages.uploading)
                return;
            auto& list = g_messages.attachments;
            if (std::find(list.begin(), list.end(), path) != list.end())
                return;
            GStatBuf st;
            if (g_stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
                return;
            if (st.st_size > UPLOAD_MAX_BYTES) {
                gchar* base = g_path_get_basename(path.c_str());
                set_status_main(tether::tr_format(_("{} is larger than the 200 MB upload limit."), base));
                g_free(base);
                return;
            }
            list.push_back(path);
            rebuild_attach_bar();
            update_composer_sensitivity();
            focus_composer_soon();
        }

        void add_attachment_uris(gchar** uris) {
            for (gchar** uri = uris; uri && *uri; ++uri) {
                gchar* path = g_filename_from_uri(*uri, nullptr, nullptr);
                if (path)
                    add_attachment(path);
                g_free(path);
            }
        }

        // A screenshot or copied image goes in as a PNG under the runtime dir.
        bool attach_pixbuf(GdkPixbuf* pixbuf) {
            const std::string dir = std::string(g_get_user_runtime_dir()) + "/tether-paste";
            g_mkdir_with_parents(dir.c_str(), 0700);
            const std::string path = dir + "/pasted-" + std::to_string(g_get_real_time()) + ".png";
            if (!gdk_pixbuf_save(pixbuf, path.c_str(), "png", nullptr, nullptr)) {
                set_status_main(_("Could not save the pasted image."));
                return false;
            }
            g_messages.pasted.insert(path);
            add_attachment(path);
            g_message("paste: saved %s, %zu attachment(s) waiting", path.c_str(), g_messages.attachments.size());
            return true;
        }

        // Ctrl+V: an image or copied files become attachments; text pastes as usual.
        void on_composer_paste(GtkTextView* view, gpointer) {
            GtkClipboard* clipboard = gtk_widget_get_clipboard(GTK_WIDGET(view), GDK_SELECTION_CLIPBOARD);
            if (gtk_clipboard_wait_is_uris_available(clipboard)) {
                gchar** uris = gtk_clipboard_wait_for_uris(clipboard);
                bool any_file = false;
                for (gchar** uri = uris; uri && *uri; ++uri)
                    any_file = any_file || g_str_has_prefix(*uri, "file://");
                if (any_file) {
                    g_signal_stop_emission_by_name(view, "paste-clipboard");
                    add_attachment_uris(uris);
                    g_strfreev(uris);
                    return;
                }
                g_strfreev(uris);
            }
            if (gtk_clipboard_wait_is_image_available(clipboard)) {
                g_signal_stop_emission_by_name(view, "paste-clipboard");
                GdkPixbuf* pixbuf = gtk_clipboard_wait_for_image(clipboard);
                if (pixbuf && gdk_pixbuf_get_width(pixbuf) > 0) {
                    g_message("paste: image %dx%d", gdk_pixbuf_get_width(pixbuf), gdk_pixbuf_get_height(pixbuf));
                    attach_pixbuf(pixbuf);
                } else {
                    // Spectacle can quit before the clipboard manager has copied the
                    // picture, leaving an image that is advertised but empty.
                    g_message("paste: clipboard offered an image but it was empty");
                    const char* reason = _("The copied image was empty. Take the screenshot again, or save it and attach the file.");
                    set_status_main(reason);
                    show_send_error(reason);
                }
                if (pixbuf)
                    g_object_unref(pixbuf);
                return;
            }
            g_message("paste: no image or files on the clipboard, pasting as text");
        }

        void on_drop_received(GtkWidget* widget,
                              GdkDragContext* context,
                              gint,
                              gint,
                              GtkSelectionData* data,
                              guint,
                              guint time,
                              gpointer) {
            gchar** uris = gtk_selection_data_get_uris(data);
            if (uris && *uris) {
                // Not the text view's own handler, which would type the URI in.
                if (GTK_IS_TEXT_VIEW(widget))
                    g_signal_stop_emission_by_name(widget, "drag-data-received");
                add_attachment_uris(uris);
                gtk_drag_finish(context, TRUE, FALSE, time);
            } else if (!GTK_IS_TEXT_VIEW(widget)) {
                if (GdkPixbuf* pixbuf = gtk_selection_data_get_pixbuf(data)) {
                    attach_pixbuf(pixbuf);
                    g_object_unref(pixbuf);
                    gtk_drag_finish(context, TRUE, FALSE, time);
                } else {
                    gtk_drag_finish(context, FALSE, FALSE, time);
                }
            }
            g_strfreev(uris);
        }

        void on_attach_clicked(GtkWidget* button, gpointer) {
            GtkWidget* toplevel = gtk_widget_get_toplevel(button);
            GtkFileChooserNative* chooser = gtk_file_chooser_native_new(_("Choose files to send"),
                                                                        GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel)
                                                                                                : nullptr,
                                                                        GTK_FILE_CHOOSER_ACTION_OPEN,
                                                                        nullptr,
                                                                        nullptr);
            gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(chooser), TRUE);
            GtkFileFilter* media = gtk_file_filter_new();
            gtk_file_filter_set_name(media, _("Images and videos"));
            gtk_file_filter_add_mime_type(media, "image/*");
            gtk_file_filter_add_mime_type(media, "video/*");
            gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), media);
            GtkFileFilter* all = gtk_file_filter_new();
            gtk_file_filter_set_name(all, _("All files"));
            gtk_file_filter_add_pattern(all, "*");
            gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), all);
            if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
                GSList* files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(chooser));
                for (GSList* it = files; it; it = it->next) {
                    add_attachment(static_cast<const char*>(it->data));
                    g_free(it->data);
                }
                g_slist_free(files);
            }
            g_object_unref(chooser);
        }

        // One send in progress: files still to upload, links so far, and where it goes.
        struct UploadJob {
            std::vector<std::string> pending;
            std::vector<std::string> sent_files;
            std::vector<std::string> links;
            // What was typed, and the same with the reply quote that goes out.
            std::string typed;
            std::string text;
            std::string thread;
            size_t total = 0;
            // Index into UPLOAD_HOSTS for the file at the front of `pending`.
            size_t host = 0;
        };

        void deliver(const std::string& thread, const std::string& body);
        void upload_next(UploadJob* job);

        void upload_failed(UploadJob* job, const std::string& reason) {
            g_messages.uploading = false;
            // The text was cleared at Send; give it back with the files still attached.
            if (g_messages.selected_thread == job->thread && composer_text().empty())
                set_composer_text(job->typed);
            delete job;
            update_composer_sensitivity();
            set_status_main(reason);
            show_send_error(reason);
        }

        void on_upload_done(GObject* source, GAsyncResult* result, gpointer user_data) {
            auto* job = static_cast<UploadJob*>(user_data);
            GSubprocess* proc = G_SUBPROCESS(source);
            gchar* out = nullptr;
            gchar* err = nullptr;
            GError* error = nullptr;
            const bool ran = g_subprocess_communicate_utf8_finish(proc, result, &out, &err, &error);
            std::string link = out ? g_strstrip(out) : "";
            std::string detail = error ? error->message : (err && *err ? g_strstrip(err) : link);
            const bool ok = ran && g_subprocess_get_successful(proc) && g_str_has_prefix(link.c_str(), "https://");
            g_message("upload: %s -> %s (%s)", job->pending.front().c_str(), ok ? link.c_str() : "failed", detail.c_str());
            g_free(out);
            g_free(err);
            g_clear_error(&error);
            g_object_unref(proc);

            gchar* base = g_path_get_basename(job->pending.front().c_str());
            const std::string name = base;
            g_free(base);
            if (!ok && job->host + 1 < G_N_ELEMENTS(UPLOAD_HOSTS)) {
                // That host is unreachable or refused the file; try the next one.
                ++job->host;
                upload_next(job);
                return;
            }
            if (!ok) {
                if (detail.size() > 160)
                    detail = detail.substr(0, 160) + "…";
                upload_failed(job, tether::tr_format(_("Could not upload {}: {}"), name, detail));
                return;
            }
            job->links.push_back(link);
            job->sent_files.push_back(job->pending.front());
            job->pending.erase(job->pending.begin());
            job->host = 0;
            upload_next(job);
        }

        void upload_next(UploadJob* job) {
            if (job->pending.empty()) {
                // Links on their own lines after the text, so the phone can preview each one.
                std::string body = job->text;
                std::string typed = job->typed;
                for (const auto& link : job->links) {
                    body += (body.empty() ? "" : "\n") + link;
                    typed += (typed.empty() ? "" : "\n") + link;
                }
                const std::string thread = job->thread;
                // The links take the files' place in the box, so a failed send
                // is retried without uploading again.
                for (const auto& file : job->sent_files) {
                    auto& list = g_messages.attachments;
                    list.erase(std::remove(list.begin(), list.end(), file), list.end());
                    forget_pasted(file);
                }
                delete job;
                g_messages.uploading = false;
                rebuild_attach_bar();
                deliver(thread, body);
                return;
            }
            const std::string& path = job->pending.front();
            set_status_main(tether::tr_format(_("Uploading {} of {}…"), job->total - job->pending.size() + 1, job->total));
            // curl's -F reads ; and , as field options, so the name is quoted.
            std::string quoted;
            for (char c : path) {
                if (c == '"' || c == '\\')
                    quoted += '\\';
                quoted += c;
            }
            const UploadHost& host = UPLOAD_HOSTS[job->host];
            const std::string field = std::string(host.field) + "=@\"" + quoted + "\"";
            std::vector<const gchar*> argv = {"curl", "-sS", "--fail-with-body", "--max-time", "300", "-F", field.c_str()};
            if (host.extra) {
                argv.push_back("-F");
                argv.push_back(host.extra);
            }
            argv.push_back(host.url);
            argv.push_back(nullptr);
            GError* error = nullptr;
            GSubprocess* proc = g_subprocess_newv(
                argv.data(), GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE), &error);
            if (!proc) {
                const std::string reason = tether::tr_format(_("Could not upload {}: {}"), path, error->message);
                g_clear_error(&error);
                upload_failed(job, reason);
                return;
            }
            g_subprocess_communicate_utf8_async(proc, nullptr, nullptr, on_upload_done, job);
        }

        void on_send_clicked(GtkWidget*, gpointer) {
            const std::string typed = composer_text();
            std::string body = typed;
            if ((body.empty() && g_messages.attachments.empty()) || g_messages.selected_thread.empty() ||
                g_messages.uploading)
                return;
            // MAP has no threaded replies, so the quote travels in the text,
            // and parse_reply turns it back into a quote on this side.
            if (!g_messages.reply_to.empty())
                body = REPLY_MARK + std::string(OPEN_QUOTE) + snippet(g_messages.reply_to, 60) + CLOSE_QUOTE + "\n" + body;

            hide_send_error();
            g_message("send: %zu chars, %zu attachment(s)", typed.size(), g_messages.attachments.size());
            // Cleared now, not when the phone answers; a failure puts the text back.
            g_messages.drafts.erase(g_messages.selected_thread);
            set_composer_text("");
            if (!g_messages.attachments.empty()) {
                auto* job = new UploadJob{g_messages.attachments, {}, {}, typed, body, g_messages.selected_thread, 0};
                job->total = job->pending.size();
                g_messages.uploading = true;
                update_composer_sensitivity();
                upload_next(job);
                return;
            }
            deliver(g_messages.selected_thread, body);
        }

        // The bubble goes up the moment Send is pressed, faded with "Sending…",
        // and is swapped for the real message when the phone's copy arrives.
        GtkWidget* add_pending_row(const std::string& body) {
            nlohmann::json message{{"outgoing", true},
                                   {"body", body},
                                   {"timestamp", static_cast<int64_t>(std::time(nullptr))}};
            const bool grouped = g_messages.rendered_last_outgoing &&
                                 std::time(nullptr) - g_messages.rendered_last_stamp < GROUP_WINDOW_SECONDS;
            GtkWidget* row = build_message_row(message, false, !grouped);
            gtk_style_context_add_class(gtk_widget_get_style_context(row), "tether-pending");
            if (g_messages.last_status)
                gtk_label_set_text(GTK_LABEL(g_messages.last_status), _("Sending…"));
            g_object_set_data_full(G_OBJECT(row), "pending-body", g_strdup(body.c_str()), g_free);
            gtk_list_box_insert(GTK_LIST_BOX(g_messages.conversation), row, -1);
            gtk_widget_show_all(row);
            g_signal_connect(row,
                             "destroy",
                             G_CALLBACK(+[](GtkWidget* w, gpointer) {
                                 for (auto& out : g_messages.outbox)
                                     if (out.row == w)
                                         out.row = nullptr;
                             }),
                             nullptr);
            g_messages.pin_next = true;
            restore_conversation_scroll(true);
            return row;
        }

        // The phone's copy of a sent message replaces its optimistic bubble.
        void drop_pending_row(const std::string& body) {
            GList* rows = gtk_container_get_children(GTK_CONTAINER(g_messages.conversation));
            for (GList* it = rows; it; it = it->next) {
                const char* pending = (const char*)g_object_get_data(G_OBJECT(it->data), "pending-body");
                if (pending && body == pending) {
                    gtk_widget_destroy(GTK_WIDGET(it->data));
                    break;
                }
            }
            g_list_free(rows);
        }

        void pump_outbox() {
            if (g_messages.sending || g_messages.outbox.empty())
                return;
            const auto& next = g_messages.outbox.front();
            nlohmann::json j;
            j["command"] = "bt_send_message";
            j["thread"] = next.thread;
            j["body"] = next.body;
            if (!daemon_send(j)) {
                const char* reason = _("Could not reach the Tether daemon; the message was not sent.");
                set_status_main(reason);
                show_send_error(reason);
                if (next.row)
                    gtk_widget_destroy(next.row);
                g_messages.outbox.pop_front();
                return;
            }
            // The watchdog stops a result that never arrives from stalling the queue.
            g_messages.sending = true;
            g_messages.send_watchdog_id = g_timeout_add_seconds(SEND_TIMEOUT_SECONDS, on_send_timeout, nullptr);
            set_status_main(_("Sending…"));
        }

        void deliver(const std::string& thread, const std::string& body) {
            GtkWidget* row = thread == g_messages.selected_thread ? add_pending_row(body) : nullptr;
            g_messages.outbox.push_back({thread, body, row});
            pump_outbox();
        }

        // Every messaging app has trained people that Enter sends and Shift+Enter
        // starts a new line. A multi-line box that only sends on a button click is
        // the single thing that makes a composer feel unfinished.
        gboolean on_composer_key(GtkWidget*, GdkEventKey* event, gpointer) {
            if (event->keyval == GDK_KEY_Escape) {
                gtk_widget_grab_focus(g_messages.thread_list);
                return TRUE;
            }
            if ((event->state & GDK_CONTROL_MASK) && (event->keyval == GDK_KEY_f || event->keyval == GDK_KEY_F)) {
                open_find_bar();
                return TRUE;
            }
            if (event->keyval != GDK_KEY_Return && event->keyval != GDK_KEY_KP_Enter)
                return FALSE;
            if (event->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK))
                return FALSE;
            // Mid-composition input methods deliver Return as part of a candidate
            // selection, which must not be read as a send.
            if (event->state & GDK_MOD1_MASK)
                return FALSE;

            if (gtk_widget_is_sensitive(g_messages.send_button))
                on_send_clicked(nullptr, nullptr);
            return TRUE;
        }

        void finish_front_send(bool ok, const std::string& reason) {
            clear_sending();
            if (g_messages.outbox.empty())
                return;
            auto sent = g_messages.outbox.front();
            g_messages.outbox.pop_front();
            if (ok) {
                hide_send_error();
                if (sent.row && g_messages.outbox.empty() && g_messages.last_status)
                    gtk_label_set_text(GTK_LABEL(g_messages.last_status), _("Sent"));
                if (sent.row)
                    gtk_style_context_remove_class(gtk_widget_get_style_context(sent.row), "tether-pending");
                // The conversation now exists under this key, so the thread list
                // refresh that follows will select it like any other.
                leave_compose();
                set_status_main(_("Sent"));
            } else {
                if (sent.row)
                    gtk_widget_destroy(sent.row);
                // Back in the box, so Retry (or Enter) sends it again.
                if (sent.thread == g_messages.selected_thread && composer_text().empty())
                    set_composer_text(sent.body);
                set_status_main(reason);
                show_send_error(reason);
            }
            update_composer_sensitivity();
            pump_outbox();
        }

        void on_send_result(const nlohmann::json& event) {
            const bool ok = event.value("success", false);
            finish_front_send(ok, event.value("message", _("The message was not sent.")));
        }

        void clear_selection() {
            switch_thread("");
            g_messages.selected_name.clear();
            g_messages.selected_repliable = false;
            g_messages.selected_block_reason.clear();
            update_composer_sensitivity();
            gtk_stack_set_visible_child_name(GTK_STACK(g_messages.placeholder_stack), "placeholder");
        }

        // Reads the thread identity and reply eligibility the daemon attached to
        // this row. The daemon owns that decision; the UI must not re-derive it.
        void apply_row_selection(GtkWidget* row) {
            const char* thread = (const char*)g_object_get_data(G_OBJECT(row), "thread");
            const char* name = (const char*)g_object_get_data(G_OBJECT(row), "name");
            const char* block_reason = (const char*)g_object_get_data(G_OBJECT(row), "reply_reason");

            switch_thread(thread ? thread : "");

            g_messages.selected_name = name ? name : "";
            g_messages.selected_repliable = g_object_get_data(G_OBJECT(row), "repliable") != nullptr;
            g_messages.selected_block_reason = block_reason ? block_reason : "";
            // Which conversation is open is otherwise only legible from the
            // selection highlight in the list beside it.
            const std::string shown =
                g_messages.selected_name.empty() ? g_messages.selected_thread : g_messages.selected_name;
            set_markup(g_messages.conversation_header, "<small>" + escape_markup(shown) + "</small>");
            const char* photo = (const char*)g_object_get_data(G_OBJECT(row), "photo");
            set_header_avatar(photo ? photo : "", shown);
            if (g_messages.find_bar)
                gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(g_messages.find_bar), FALSE);
            set_text(g_messages.composer_placeholder, tether::tr_format(_("Message @{}"), shown));
            hide_reply_bar();
            update_composer_sensitivity();
        }

        void on_recipient_changed(GtkEditable*, gpointer) {
            if (!g_messages.composing)
                return;

            const std::string text = gtk_entry_get_text(GTK_ENTRY(g_messages.compose_entry));
            g_messages.selected_thread.clear();
            g_messages.selected_name.clear();
            g_messages.selected_repliable = false;
            g_messages.selected_block_reason.clear();

            bluetooth::Recipient recipient;
            std::string err;
            if (text.empty()) {
                // Nothing typed yet is not an error worth shouting about.
            } else if (bluetooth::recipient_from_input(text, recipient, err)) {
                g_messages.selected_thread = bluetooth::thread_key_for(recipient);
                g_messages.selected_repliable = !g_messages.selected_thread.empty();
                g_messages.selected_name = contact_name_for(g_messages.selected_thread);
            } else {
                g_messages.selected_block_reason = err;
            }

            const bool bad = !g_messages.selected_block_reason.empty();
            gtk_entry_set_icon_from_icon_name(
                GTK_ENTRY(g_messages.compose_entry), GTK_ENTRY_ICON_SECONDARY, bad ? "dialog-error-symbolic" : nullptr);
            gtk_entry_set_icon_tooltip_text(GTK_ENTRY(g_messages.compose_entry),
                                            GTK_ENTRY_ICON_SECONDARY,
                                            bad ? g_messages.selected_block_reason.c_str() : nullptr);

            std::string header = _("New Message");
            if (!g_messages.selected_name.empty())
                header = g_messages.selected_name;
            else if (!g_messages.selected_thread.empty())
                header = recipient.address;
            set_markup(g_messages.conversation_header, "<b>" + escape_markup(header) + "</b>");

            update_composer_sensitivity();

            // Whatever has already been said to this person belongs above the
            // composer. Only on a change, so this is not a request per keystroke;
            // the daemon serves it from memory either way.
            if (g_messages.selected_thread != g_messages.compose_requested_key) {
                g_messages.compose_requested_key = g_messages.selected_thread;
                end_jump();
                clear_list_box(g_messages.conversation);
                g_messages.reaction_slots.clear();
                g_messages.reply_counters.clear();
                g_messages.rendered.clear();
                g_messages.rendered_last_stamp = 0;
                g_messages.rendered_last_outgoing = false;
                gtk_widget_show_all(g_messages.conversation);
                request_messages(g_messages.selected_thread);
            }
        }

        void enter_compose() {
            // Dropping the selection fires row-selected(nullptr), which would run
            // clear_selection() over the state set just below.
            if (g_messages.thread_selected_handler)
                g_signal_handler_block(g_messages.thread_list, g_messages.thread_selected_handler);
            gtk_list_box_unselect_all(GTK_LIST_BOX(g_messages.thread_list));
            if (g_messages.thread_selected_handler)
                g_signal_handler_unblock(g_messages.thread_list, g_messages.thread_selected_handler);

            stash_draft();
            set_composer_text("");
            hide_send_error();

            g_messages.composing = true;
            g_messages.compose_requested_key.clear();
            contact_completion_request();

            clear_list_box(g_messages.conversation);
                g_messages.reaction_slots.clear();
                g_messages.reply_counters.clear();
            g_messages.rendered.clear();
            g_messages.rendered_last_stamp = 0;
            g_messages.rendered_last_outgoing = false;
            gtk_widget_show_all(g_messages.conversation);
            gtk_widget_show(g_messages.compose_bar);
            gtk_stack_set_visible_child_name(GTK_STACK(g_messages.placeholder_stack), "conversation");

            gtk_entry_set_text(GTK_ENTRY(g_messages.compose_entry), "");
            // set_text only emits "changed" when the text actually differs, so the
            // reset state is applied here rather than relied on.
            on_recipient_changed(nullptr, nullptr);
            gtk_widget_grab_focus(g_messages.compose_entry);
        }

        void leave_compose() {
            if (!g_messages.composing)
                return;
            g_messages.composing = false;
            g_messages.compose_requested_key.clear();
            gtk_widget_hide(g_messages.compose_bar);
            gtk_entry_set_icon_from_icon_name(GTK_ENTRY(g_messages.compose_entry), GTK_ENTRY_ICON_SECONDARY, nullptr);
        }

        void on_compose_cancel(GtkWidget*, gpointer) {
            leave_compose();
            clear_selection();
        }

        gboolean on_compose_entry_key(GtkWidget*, GdkEventKey* event, gpointer) {
            if (event->keyval != GDK_KEY_Escape)
                return FALSE;
            on_compose_cancel(nullptr, nullptr);
            return TRUE;
        }

        void on_thread_selected(GtkListBox*, GtkListBoxRow* row, gpointer) {
            leave_compose();
            if (!row) {
                clear_selection();
                return;
            }
            apply_row_selection(GTK_WIDGET(row));
            gtk_stack_set_visible_child_name(GTK_STACK(g_messages.placeholder_stack), "conversation");
            request_messages(g_messages.selected_thread);
        }

        void on_thread_activated(GtkListBox*, GtkListBoxRow*, gpointer) { focus_composer_soon(); }

    } // namespace

    void messages_view_handle_disconnect() {
        clear_sending();
        g_messages.map_open = false;
        // The next session re-lists messages under fresh object paths, so nothing
        // is owed from the one that just ended.
        g_messages.marked_read.clear();
        update_composer_sensitivity();
        set_banner(_("The Tether daemon is not running."));
    }

    void messages_view_open_thread(const std::string& thread_key) {
        if (thread_key.empty())
            return;

        leave_compose();
        g_messages.pending_new_thread = thread_key;
        switch_thread(thread_key);
        g_messages.selected_name.clear();
        g_messages.selected_repliable = false;
        g_messages.selected_block_reason.clear();
        g_messages.focus_composer = true;
        gtk_stack_set_visible_child_name(GTK_STACK(g_messages.placeholder_stack), "conversation");
        update_composer_sensitivity();
        request_threads();
        request_messages(thread_key);
    }

    void messages_view_new_message() { enter_compose(); }

    void messages_view_focus_search() {
        if (g_messages.search_entry)
            gtk_widget_grab_focus(g_messages.search_entry);
    }

    void messages_view_store_prefs() {
        if (g_messages.paned)
            prefs()["sidebar_width"] = gtk_paned_get_position(GTK_PANED(g_messages.paned));
    }

    void messages_view_set_visible(bool visible) {
        g_messages.visible = visible;
        if (!visible)
            return;
        request_threads();
        request_messages(g_messages.selected_thread);
    }

    bool messages_view_handle_event(const nlohmann::json& event) {
        const std::string command = event.value("command", "");
        if (command == "bt_search_results") {
            show_search_results(event);
            return true;
        }
        if (command == "bt_threads") {
            int unread = 0;
            if (event.contains("threads") && event["threads"].is_array())
                for (const auto& thread : event["threads"])
                    unread += thread.value("unread", 0);
            tray_set_unread(unread);
            show_threads(event);
            return true;
        }
        if (command == "bt_messages") {
            show_messages(event);
            return true;
        }
        if (command == "bt_contacts") {
            // The shared completion model is fed from the dispatcher.
            return true;
        }
        if (command == "bt_connection_changed") {
            update_connection(event);
            return false;
        }
        if (command == "bt_message") {
            // thread list is re-read even while hidden for tray unread count
            request_threads();
            if (!g_messages.visible)
                return true;
            if (same_thread(event.value("thread", ""), g_messages.selected_thread))
                request_messages(g_messages.selected_thread);
            return true;
        }
        if (command == "bt_send_result") {
            on_send_result(event);
            return true;
        }
        if (command == "bt_message_read") {
            if (g_messages.visible)
                request_threads();
            return true;
        }
        if (command == "bt_solicit_result") {
            set_status_main(event.value("message", ""));
            return true;
        }
        return false;
    }

    GtkWidget* messages_view_new() {
        GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

        g_messages.banner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(g_messages.banner), 8);
        GtkWidget* banner_icon = gtk_image_new_from_icon_name("dialog-information-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_box_pack_start(GTK_BOX(g_messages.banner), banner_icon, FALSE, FALSE, 0);
        g_messages.banner_label = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(g_messages.banner_label), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(g_messages.banner_label), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(g_messages.banner_label), PANGO_WRAP_WORD_CHAR);
        gtk_box_pack_start(GTK_BOX(g_messages.banner), g_messages.banner_label, TRUE, TRUE, 0);

        g_messages.banner_action = gtk_button_new_with_label(_("Show iPhone Permissions"));
        gtk_widget_set_valign(g_messages.banner_action, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(g_messages.banner_action,
                                    _("Re-advertise so the iPhone shows its Show Message Notifications and "
                                      "Sync Contacts toggles under Settings > Bluetooth > (i)."));
        g_signal_connect(g_messages.banner_action, "clicked", G_CALLBACK(on_solicit_clicked), nullptr);
        gtk_box_pack_start(GTK_BOX(g_messages.banner), g_messages.banner_action, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(root), g_messages.banner, FALSE, FALSE, 0);

        GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
        g_messages.paned = paned;
        gtk_paned_set_position(GTK_PANED(paned), prefs().value("sidebar_width", 260));
        gtk_box_pack_start(GTK_BOX(root), paned, TRUE, TRUE, 0);

        GtkWidget* thread_scroll = gtk_scrolled_window_new(nullptr, nullptr);
        g_messages.thread_scroll = thread_scroll;
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(thread_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_size_request(thread_scroll, 240, -1);
        g_messages.thread_list = gtk_list_box_new();
        gtk_style_context_add_class(gtk_widget_get_style_context(g_messages.thread_list), "tether-thread-list");
        g_messages.thread_selected_handler =
            g_signal_connect(g_messages.thread_list, "row-selected", G_CALLBACK(on_thread_selected), nullptr);
        g_signal_connect(g_messages.thread_list, "row-activated", G_CALLBACK(on_thread_activated), nullptr);
        gtk_list_box_set_filter_func(GTK_LIST_BOX(g_messages.thread_list), thread_visible, nullptr, nullptr);
        gtk_list_box_set_sort_func(GTK_LIST_BOX(g_messages.thread_list), thread_sort, nullptr, nullptr);
        gtk_list_box_set_header_func(GTK_LIST_BOX(g_messages.thread_list), thread_header, nullptr, nullptr);
        g_signal_connect(g_messages.thread_list, "button-press-event", G_CALLBACK(on_thread_list_button), nullptr);
        on_pins_changed([] {
            g_messages.threads_dirty = true;
            request_threads();
        });
        GtkWidget* side_lists = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_box_pack_start(GTK_BOX(side_lists), g_messages.thread_list, FALSE, FALSE, 0);
        g_messages.results_header = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(g_messages.results_header), 0.0);
        gtk_widget_set_margin_start(g_messages.results_header, 16);
        gtk_widget_set_margin_top(g_messages.results_header, 12);
        gtk_widget_set_margin_bottom(g_messages.results_header, 4);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_messages.results_header), "tether-section-label");
        gtk_widget_set_no_show_all(g_messages.results_header, TRUE);
        gtk_box_pack_start(GTK_BOX(side_lists), g_messages.results_header, FALSE, FALSE, 0);
        g_messages.results_list = gtk_list_box_new();
        gtk_style_context_add_class(gtk_widget_get_style_context(g_messages.results_list), "tether-thread-list");
        gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(g_messages.results_list), TRUE);
        g_signal_connect(g_messages.results_list, "row-activated", G_CALLBACK(on_result_activated), nullptr);
        gtk_box_pack_start(GTK_BOX(side_lists), g_messages.results_list, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(thread_scroll), side_lists);

        GtkWidget* thread_side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

        g_messages.search_entry = gtk_search_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(g_messages.search_entry), _("Search conversations"));
        set_accessible_name(g_messages.search_entry, _("Search conversations"));
        gtk_entry_set_width_chars(GTK_ENTRY(g_messages.search_entry), 8);
        gtk_widget_set_margin_top(g_messages.search_entry, 8);
        gtk_widget_set_margin_start(g_messages.search_entry, 8);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_messages.search_entry), "tether-search");
        gtk_widget_set_margin_end(g_messages.search_entry, 8);
        g_signal_connect(g_messages.search_entry, "search-changed", G_CALLBACK(on_search_changed), nullptr);
        // GtkSearchEntry reports Escape but does not act on it outside a
        // GtkSearchBar, so an abandoned search would otherwise stay applied.
        g_signal_connect(g_messages.search_entry,
                         "stop-search",
                         G_CALLBACK(+[](GtkSearchEntry* entry, gpointer) {
                             gtk_entry_set_text(GTK_ENTRY(entry), "");
                             gtk_widget_grab_focus(g_messages.thread_list);
                         }),
                         nullptr);
        GtkWidget* search_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        gtk_box_pack_start(GTK_BOX(search_row), g_messages.search_entry, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(thread_side), search_row, FALSE, FALSE, 0);

        GtkWidget* new_message = gtk_button_new_from_icon_name("document-edit-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_button_set_relief(GTK_BUTTON(new_message), GTK_RELIEF_NONE);
        gtk_widget_set_tooltip_text(new_message, _("New Message"));
        set_accessible_name(new_message, _("New Message"));
        gtk_widget_set_valign(new_message, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_end(new_message, 6);
        g_signal_connect(new_message, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer) { enter_compose(); }), nullptr);
        gtk_box_pack_start(GTK_BOX(search_row), new_message, FALSE, FALSE, 0);
        gtk_widget_set_margin_bottom(search_row, 6);
        gtk_box_pack_start(GTK_BOX(thread_side), thread_scroll, TRUE, TRUE, 0);
        gtk_paned_pack1(GTK_PANED(paned), thread_side, FALSE, FALSE);

        g_messages.placeholder_stack = gtk_stack_new();

        GtkWidget* placeholder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_widget_set_valign(placeholder, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(placeholder, GTK_ALIGN_CENTER);
        g_messages.placeholder_icon = gtk_image_new_from_icon_name("mail-unread-symbolic", GTK_ICON_SIZE_DIALOG);
        gtk_box_pack_start(GTK_BOX(placeholder), g_messages.placeholder_icon, FALSE, FALSE, 0);
        g_messages.placeholder_label = gtk_label_new(_("Select a conversation"));
        gtk_style_context_add_class(gtk_widget_get_style_context(g_messages.placeholder_label), "muted");
        gtk_box_pack_start(GTK_BOX(placeholder), g_messages.placeholder_label, FALSE, FALSE, 0);
        gtk_stack_add_named(GTK_STACK(g_messages.placeholder_stack), placeholder, "placeholder");

        GtkWidget* conversation_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

        g_messages.compose_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(g_messages.compose_bar), 8);
        GtkWidget* to_label = gtk_label_new(_("To:"));
        gtk_box_pack_start(GTK_BOX(g_messages.compose_bar), to_label, FALSE, FALSE, 0);

        g_messages.compose_entry = gtk_entry_new();
        gtk_label_set_mnemonic_widget(GTK_LABEL(to_label), g_messages.compose_entry);
        gtk_entry_set_placeholder_text(GTK_ENTRY(g_messages.compose_entry), _("Phone number or email"));
        // Lets the entry shrink with the pane. At its natural minimum the Cancel
        // button beside it is pushed off the edge of a narrow window.
        gtk_entry_set_width_chars(GTK_ENTRY(g_messages.compose_entry), 12);
        gtk_box_pack_start(GTK_BOX(g_messages.compose_bar), g_messages.compose_entry, TRUE, TRUE, 0);

        attach_contact_completion(g_messages.compose_entry, ContactKind::Any);

        GtkWidget* compose_cancel = gtk_button_new_with_label(_("Cancel"));
        g_signal_connect(compose_cancel, "clicked", G_CALLBACK(on_compose_cancel), nullptr);
        gtk_box_pack_start(GTK_BOX(g_messages.compose_bar), compose_cancel, FALSE, FALSE, 0);

        g_signal_connect(g_messages.compose_entry, "changed", G_CALLBACK(on_recipient_changed), nullptr);
        // Enter in the To: field means "done addressing"; the body is empty and
        // sending from here would be an accident every time. Connected after the
        // completion, so a popup selection consumes the key first.
        g_signal_connect(g_messages.compose_entry,
                         "activate",
                         G_CALLBACK(+[](GtkEntry*, gpointer) { focus_composer_soon(); }),
                         nullptr);
        g_signal_connect_after(g_messages.compose_entry, "key-press-event", G_CALLBACK(on_compose_entry_key), nullptr);

        // The children are shown once here, then the bar itself is toggled. A
        // no-show-all container is skipped by gtk_widget_show_all outright, so
        // showing it later has to be gtk_widget_show on an already-built tree.
        gtk_box_pack_start(GTK_BOX(conversation_box), g_messages.compose_bar, FALSE, FALSE, 0);
        gtk_widget_show_all(g_messages.compose_bar);
        gtk_widget_hide(g_messages.compose_bar);
        gtk_widget_set_no_show_all(g_messages.compose_bar, TRUE);

        // iMessage's header: the person's picture over their name, centred.
        GtkWidget* conversation_header_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_container_set_border_width(GTK_CONTAINER(conversation_header_box), 8);
        g_messages.header_avatar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_halign(g_messages.header_avatar, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(conversation_header_box), g_messages.header_avatar, FALSE, FALSE, 0);
        g_messages.conversation_header = gtk_label_new(nullptr);
        gtk_widget_set_halign(g_messages.conversation_header, GTK_ALIGN_CENTER);
        gtk_label_set_ellipsize(GTK_LABEL(g_messages.conversation_header), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(conversation_header_box), g_messages.conversation_header, FALSE, FALSE, 0);
        // The header with a search button at its right edge.
        GtkWidget* header_overlay = gtk_overlay_new();
        gtk_container_add(GTK_CONTAINER(header_overlay), conversation_header_box);
        GtkWidget* find_button = gtk_button_new_from_icon_name("edit-find-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_button_set_relief(GTK_BUTTON(find_button), GTK_RELIEF_NONE);
        gtk_widget_set_tooltip_text(find_button, _("Search this conversation (Ctrl+F)"));
        gtk_widget_set_halign(find_button, GTK_ALIGN_END);
        gtk_widget_set_valign(find_button, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_end(find_button, 12);
        g_signal_connect(find_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { open_find_bar(); }), nullptr);
        gtk_overlay_add_overlay(GTK_OVERLAY(header_overlay), find_button);
        gtk_box_pack_start(GTK_BOX(conversation_box), header_overlay, FALSE, FALSE, 0);

        g_messages.find_bar = gtk_search_bar_new();
        GtkWidget* find_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        g_messages.find_entry = gtk_search_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(g_messages.find_entry), _("Search messages"));
        gtk_entry_set_width_chars(GTK_ENTRY(g_messages.find_entry), 28);
        gtk_box_pack_start(GTK_BOX(find_row), g_messages.find_entry, TRUE, TRUE, 0);
        g_messages.find_count = gtk_label_new("");
        gtk_style_context_add_class(gtk_widget_get_style_context(g_messages.find_count), "muted");
        gtk_box_pack_start(GTK_BOX(find_row), g_messages.find_count, FALSE, FALSE, 0);
        GtkWidget* older = gtk_button_new_from_icon_name("go-up-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_widget_set_tooltip_text(older, _("Older match"));
        g_signal_connect(older, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { find_select(g_messages.find_index - 1); }), nullptr);
        gtk_box_pack_start(GTK_BOX(find_row), older, FALSE, FALSE, 0);
        GtkWidget* newer = gtk_button_new_from_icon_name("go-down-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_widget_set_tooltip_text(newer, _("Newer match"));
        g_signal_connect(newer, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { find_select(g_messages.find_index + 1); }), nullptr);
        gtk_box_pack_start(GTK_BOX(find_row), newer, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(g_messages.find_bar), find_row);
        gtk_search_bar_connect_entry(GTK_SEARCH_BAR(g_messages.find_bar), GTK_ENTRY(g_messages.find_entry));
        gtk_search_bar_set_show_close_button(GTK_SEARCH_BAR(g_messages.find_bar), TRUE);
        g_signal_connect(g_messages.find_entry, "search-changed", G_CALLBACK(+[](GtkSearchEntry*, gpointer) { find_refresh(); }), nullptr);
        // Enter steps to the next older match, Shift+Enter back towards newer.
        g_signal_connect(g_messages.find_entry,
                         "activate",
                         G_CALLBACK(+[](GtkEntry*, gpointer) { find_select(g_messages.find_index - 1); }),
                         nullptr);
        gtk_box_pack_start(GTK_BOX(conversation_box), g_messages.find_bar, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(conversation_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

        g_messages.conversation_scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(
            GTK_SCROLLED_WINDOW(g_messages.conversation_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        g_messages.conversation = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_messages.conversation), GTK_SELECTION_NONE);
        gtk_container_add(GTK_CONTAINER(g_messages.conversation_scroll), g_messages.conversation);
        if (GtkAdjustment* adjustment = conversation_adjustment()) {
            g_signal_connect(adjustment, "changed", G_CALLBACK(on_conversation_changed), nullptr);
            g_signal_connect(adjustment, "value-changed", G_CALLBACK(on_conversation_value_changed), nullptr);
        }
        gtk_box_pack_start(GTK_BOX(conversation_box), g_messages.conversation_scroll, TRUE, TRUE, 0);

        // Replying-to strip above the composer, hidden until a Reply button is used.
        g_messages.reply_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_margin_start(g_messages.reply_bar, 16);
        gtk_widget_set_margin_end(g_messages.reply_bar, 12);
        gtk_widget_set_margin_top(g_messages.reply_bar, 4);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_messages.reply_bar), "tether-reply-bar");
        gtk_box_pack_start(GTK_BOX(g_messages.reply_bar),
                           gtk_image_new_from_icon_name("mail-reply-sender-symbolic", GTK_ICON_SIZE_MENU),
                           FALSE, FALSE, 0);
        g_messages.reply_label = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(g_messages.reply_label), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(g_messages.reply_label), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(g_messages.reply_bar), g_messages.reply_label, TRUE, TRUE, 0);
        GtkWidget* reply_cancel = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
        gtk_button_set_relief(GTK_BUTTON(reply_cancel), GTK_RELIEF_NONE);
        gtk_widget_set_tooltip_text(reply_cancel, _("Cancel reply"));
        g_signal_connect(reply_cancel, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { hide_reply_bar(); }), nullptr);
        gtk_box_pack_start(GTK_BOX(g_messages.reply_bar), reply_cancel, FALSE, FALSE, 0);
        gtk_widget_show_all(g_messages.reply_bar);
        gtk_widget_hide(g_messages.reply_bar);
        gtk_widget_set_no_show_all(g_messages.reply_bar, TRUE);

        GtkWidget* composer_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(composer_box), 10);
        g_messages.composer = gtk_text_view_new();
        set_accessible_name(g_messages.composer, _("Message"));
        // Tab moves focus on to Send instead of typing a tab.
        gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(g_messages.composer), FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(g_messages.composer), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(g_messages.composer), 4);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(g_messages.composer), 4);
        gtk_text_view_set_top_margin(GTK_TEXT_VIEW(g_messages.composer), 5);
        gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(g_messages.composer), 5);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_messages.composer), "tether-composer-text");
        GtkWidget* composer_frame = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(composer_frame), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(composer_frame), TRUE);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(composer_frame), 140);
        gtk_container_add(GTK_CONTAINER(composer_frame), g_messages.composer);

        // GtkTextView has no placeholder, so a label sits over it while it is empty.
        GtkWidget* composer_overlay = gtk_overlay_new();
        gtk_container_add(GTK_CONTAINER(composer_overlay), composer_frame);
        g_messages.composer_placeholder = gtk_label_new(_("Message"));
        gtk_widget_set_halign(g_messages.composer_placeholder, GTK_ALIGN_START);
        gtk_widget_set_valign(g_messages.composer_placeholder, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_start(g_messages.composer_placeholder, 6);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_messages.composer_placeholder), "tether-placeholder");
        gtk_overlay_add_overlay(GTK_OVERLAY(composer_overlay), g_messages.composer_placeholder);
        gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(composer_overlay), g_messages.composer_placeholder, TRUE);

        // The pill, as in Discord: attach, text, emoji, then send, inside one rounded field.
        GtkWidget* pill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        gtk_style_context_add_class(gtk_widget_get_style_context(pill), "tether-composer");

        GtkWidget* attach = gtk_button_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_button_set_relief(GTK_BUTTON(attach), GTK_RELIEF_NONE);
        gtk_widget_set_valign(attach, GTK_ALIGN_END);
        gtk_widget_set_tooltip_text(attach, _("Attach images or files"));
        set_accessible_name(attach, _("Attach images or files"));
        gtk_style_context_add_class(gtk_widget_get_style_context(attach), "tether-composer-button");
        g_signal_connect(attach, "clicked", G_CALLBACK(on_attach_clicked), nullptr);
        gtk_box_pack_start(GTK_BOX(pill), attach, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(pill), composer_overlay, TRUE, TRUE, 0);

        GtkWidget* gif = gtk_button_new_with_label("GIF");
        gtk_button_set_relief(GTK_BUTTON(gif), GTK_RELIEF_NONE);
        gtk_widget_set_valign(gif, GTK_ALIGN_END);
        gtk_widget_set_tooltip_text(gif, _("Send a GIF"));
        gtk_style_context_add_class(gtk_widget_get_style_context(gif), "tether-composer-button");
        gtk_style_context_add_class(gtk_widget_get_style_context(gif), "tether-gif-button");
        // Sent as a GIPHY link, which iMessage plays inline on the other phone too.
        g_signal_connect(gif,
                         "clicked",
                         G_CALLBACK(+[](GtkButton* button, gpointer) {
                             if (g_messages.selected_thread.empty())
                                 return;
                             gif_picker_open(GTK_WIDGET(button), [](const std::string& url) { send_side_message(url); });
                         }),
                         nullptr);
        gtk_box_pack_start(GTK_BOX(pill), gif, FALSE, FALSE, 0);

        GtkWidget* emoji = gtk_button_new_from_icon_name("face-smile-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_button_set_relief(GTK_BUTTON(emoji), GTK_RELIEF_NONE);
        gtk_widget_set_valign(emoji, GTK_ALIGN_END);
        gtk_widget_set_tooltip_text(emoji, _("Emoji (Ctrl+.)"));
        gtk_style_context_add_class(gtk_widget_get_style_context(emoji), "tether-composer-button");
        // GTK's own emoji chooser, the same one Ctrl+. opens in the box.
        g_signal_connect(emoji,
                         "clicked",
                         G_CALLBACK(+[](GtkButton*, gpointer) {
                             gtk_widget_grab_focus(g_messages.composer);
                             g_signal_emit_by_name(g_messages.composer, "insert-emoji");
                         }),
                         nullptr);
        gtk_box_pack_start(GTK_BOX(pill), emoji, FALSE, FALSE, 0);

        g_messages.send_button = gtk_button_new_from_icon_name("document-send-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_widget_set_no_show_all(g_messages.send_button, TRUE);
        gtk_widget_set_tooltip_text(g_messages.send_button, _("Send"));
        set_accessible_name(g_messages.send_button, _("Send"));
        gtk_widget_set_valign(g_messages.send_button, GTK_ALIGN_END);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_messages.send_button), "tether-send");
        gtk_box_pack_start(GTK_BOX(pill), g_messages.send_button, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(composer_box), pill, TRUE, TRUE, 0);

        g_signal_connect(g_messages.send_button, "clicked", G_CALLBACK(on_send_clicked), nullptr);
        g_signal_connect(g_messages.composer, "key-press-event", G_CALLBACK(on_composer_key), nullptr);
        g_signal_connect(g_messages.composer, "paste-clipboard", G_CALLBACK(on_composer_paste), nullptr);
        // Files dropped anywhere on the conversation, or on the box itself, are attached.
        gtk_target_list_add_uri_targets(gtk_drag_dest_get_target_list(g_messages.composer), 0);
        g_signal_connect(g_messages.composer, "drag-data-received", G_CALLBACK(on_drop_received), nullptr);
        gtk_drag_dest_set(conversation_box, GTK_DEST_DEFAULT_ALL, nullptr, 0, GDK_ACTION_COPY);
        gtk_drag_dest_add_uri_targets(conversation_box);
        gtk_drag_dest_add_image_targets(conversation_box);
        g_signal_connect(conversation_box, "drag-data-received", G_CALLBACK(on_drop_received), nullptr);
        // The button follows what is actually in the box, so "Send" is never
        // offered for an empty message. Editing also retires a failure notice
        // that no longer describes what is in the box.
        g_signal_connect(gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_messages.composer)),
                         "changed",
                         G_CALLBACK(+[](GtkTextBuffer* buffer, gpointer) {
                             hide_send_error();
                             update_composer_sensitivity();
                             const bool empty = gtk_text_buffer_get_char_count(buffer) == 0;
                             gtk_widget_set_visible(g_messages.composer_placeholder, empty);
                         }),
                         nullptr);
        // Enabled only once MAP is up and a conversation is open.
        update_composer_sensitivity();

        g_messages.send_error = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(g_messages.send_error), 8);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_messages.send_error), "tether-send-error");
        gtk_box_pack_start(GTK_BOX(g_messages.send_error),
                           gtk_image_new_from_icon_name("dialog-warning-symbolic", GTK_ICON_SIZE_BUTTON),
                           FALSE,
                           FALSE,
                           0);
        g_messages.send_error_label = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(g_messages.send_error_label), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(g_messages.send_error_label), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(g_messages.send_error_label), PANGO_WRAP_WORD_CHAR);
        gtk_box_pack_start(GTK_BOX(g_messages.send_error), g_messages.send_error_label, TRUE, TRUE, 0);
        GtkWidget* retry = gtk_button_new_with_label(_("Retry"));
        gtk_widget_set_valign(retry, GTK_ALIGN_CENTER);
        g_signal_connect(retry, "clicked", G_CALLBACK(on_send_clicked), nullptr);
        gtk_box_pack_start(GTK_BOX(g_messages.send_error), retry, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(conversation_box), g_messages.send_error, FALSE, FALSE, 0);
        gtk_widget_show_all(g_messages.send_error);
        gtk_widget_hide(g_messages.send_error);
        gtk_widget_set_no_show_all(g_messages.send_error, TRUE);

        // A dead composer with the explanation hidden in the send button's tooltip
        // reads as the app being broken. The button is insensitive, so the tooltip
        // never appears at all.
        g_messages.composer_notice = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(g_messages.composer_notice), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(g_messages.composer_notice), TRUE);
        gtk_widget_set_no_show_all(g_messages.composer_notice, TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_messages.composer_notice), "muted");
        gtk_widget_set_margin_start(g_messages.composer_notice, 8);
        gtk_widget_set_margin_end(g_messages.composer_notice, 8);
        gtk_box_pack_start(GTK_BOX(conversation_box), g_messages.composer_notice, FALSE, FALSE, 0);

        // Files waiting to be sent, above the composer, with where they will go.
        g_messages.attach_bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_margin_start(g_messages.attach_bar, 14);
        gtk_widget_set_margin_end(g_messages.attach_bar, 12);
        gtk_widget_set_margin_top(g_messages.attach_bar, 6);
        GtkWidget* chips_scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(chips_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(chips_scroll), TRUE);
        g_messages.attach_chips = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_container_add(GTK_CONTAINER(chips_scroll), g_messages.attach_chips);
        gtk_box_pack_start(GTK_BOX(g_messages.attach_bar), chips_scroll, FALSE, FALSE, 0);
        GtkWidget* attach_note =
            gtk_label_new(_("Uploaded to a public host (catbox.moe, or 0x0.st if that is blocked) and sent as a link: anyone with the link can open the file."));
        gtk_label_set_xalign(GTK_LABEL(attach_note), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(attach_note), TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(attach_note), "muted");
        gtk_style_context_add_class(gtk_widget_get_style_context(attach_note), "tether-reply-bar");
        gtk_box_pack_start(GTK_BOX(g_messages.attach_bar), attach_note, FALSE, FALSE, 0);
        gtk_widget_show_all(g_messages.attach_bar);
        gtk_widget_hide(g_messages.attach_bar);
        gtk_widget_set_no_show_all(g_messages.attach_bar, TRUE);

        gtk_box_pack_start(GTK_BOX(conversation_box), g_messages.reply_bar, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(conversation_box), g_messages.attach_bar, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(conversation_box), composer_box, FALSE, FALSE, 0);
        gtk_stack_add_named(GTK_STACK(g_messages.placeholder_stack), conversation_box, "conversation");

        gtk_paned_pack2(GTK_PANED(paned), g_messages.placeholder_stack, TRUE, FALSE);
        gtk_stack_set_visible_child_name(GTK_STACK(g_messages.placeholder_stack), "placeholder");

        gtk_widget_set_no_show_all(g_messages.banner, TRUE);
        return root;
    }

} // namespace tether::ui
