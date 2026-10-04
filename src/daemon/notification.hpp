#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tether {

    // A mirrored notification as the desktop should present it.
    struct NotificationSpec {
        // The originating iPhone app. Shown as the popup's application name
        std::string app_name;
        std::string summary;
        std::string body;
        // Freedesktop icon names, best first.
        std::vector<std::string> icons;
        bool quiet = false;
        std::string reply_thread;
        // OTP found in this notification, or empty. Non-empty adds a copy action.
        std::string otp_code;
        // Originating iPhone bundle id.
        std::string app_id;
        // MAP handle to mark read when the code is copied. Empty for
        // notifications with no message behind them.
        std::string read_handle;
    };

    class DesktopNotifier {
    public:
        DesktopNotifier();
        ~DesktopNotifier();

        DesktopNotifier(const DesktopNotifier&) = delete;
        DesktopNotifier& operator=(const DesktopNotifier&) = delete;

        bool init();

        // Where the "Copy Code" action sends the code, along with the spec's
        // read_handle. Called on the notifier's own thread, so the handler is
        // responsible for any thread hop it needs.
        void set_copy_handler(std::function<void(const std::string& code, const std::string& handle)> handler);

        void notify_file_arrived(const std::filesystem::path& path);

        // Where the incoming call popup's buttons go: action is answer_here,
        // answer or hangup, path the ringing call. Called on the notifier's thread.
        void set_call_action_handler(std::function<void(const std::string& action, const std::string& path)> handler);

        // One popup per ringing call; showing another replaces it.
        void show_incoming_call(const std::string& path, const std::string& caller, const std::string& detail);
        void dismiss_incoming_call();

        void notify(const NotificationSpec& spec);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace tether
