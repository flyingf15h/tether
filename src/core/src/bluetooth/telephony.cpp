#include "tether/bluetooth/telephony.hpp"
#include "tether/bluetooth/monitor.hpp"
#include "tether/log.hpp"

#include <tether/i18n.hpp>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstdlib>
#include <cctype>

namespace tether::bluetooth {

    namespace {

        constexpr const char* BLUEZ_NAME = "org.bluez";
        constexpr const char* IFACE_TELEPHONY = "org.bluez.Telephony1";
        constexpr const char* IFACE_CALL = "org.bluez.Call1";
        constexpr const char* IFACE_PROPS = "org.freedesktop.DBus.Properties";

        constexpr int CALL_TIMEOUT_MS = 10000;

        constexpr size_t MAX_DIAL_DIGITS = 32;
        constexpr size_t MAX_TONES = 32;

        bool iequals(const std::string& a, const std::string& b) {
            return a.size() == b.size() &&
                   std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
                       return std::tolower(x) == std::tolower(y);
                   });
        }

    } // namespace

    std::string normalize_dial_string(const std::string& input) {
        std::string out;
        bool seen_digit = false;
        for (const char raw : input) {
            const unsigned char c = static_cast<unsigned char>(raw);
            if (c == '+') {
                // Only as the country-code prefix
                if (!out.empty())
                    return {};
                out.push_back('+');
                continue;
            }
            if (std::isdigit(c)) {
                out.push_back(static_cast<char>(c));
                seen_digit = true;
                continue;
            }
            if (c == ' ' || c == '-' || c == '(' || c == ')' || c == '.' || c == '\t')
                continue;
            return {};
        }
        if (!seen_digit)
            return {};
        const size_t digits = out.size() - (out[0] == '+' ? 1 : 0);
        return digits > MAX_DIAL_DIGITS ? std::string{} : out;
    }

    nlohmann::json to_json(const Call& call) {
        nlohmann::json j;
        j["path"] = call.path;
        j["number"] = call.withheld() ? "" : call.number;
        j["withheld"] = call.withheld();
        j["name"] = call.name;
        j["incoming_line"] = call.incoming_line;
        j["state"] = call.state;
        j["multiparty"] = call.multiparty;
        j["ringing"] = call.ringing();
        j["outgoing"] = call.outgoing();
        j["connected"] = call.connected();
        return j;
    }

    nlohmann::json to_json(const std::vector<Call>& calls) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& call : calls)
            out.push_back(to_json(call));
        return out;
    }

    nlohmann::json to_json(const Telephony& gateway) {
        nlohmann::json j;
        j["available"] = gateway.ready();
        j["path"] = gateway.path;
        j["state"] = gateway.state;
        j["operator"] = gateway.operator_name;
        j["signal"] = gateway.signal;
        j["battery"] = gateway.battery;
        j["service"] = gateway.service;
        j["roaming"] = gateway.roaming;
        j["inband_ringtone"] = gateway.inband_ringtone;
        return j;
    }

    namespace {

        class BluezSource : public TelephonySource {
        public:
            BluezSource(BluezMonitor& monitor, std::string address)
                : monitor_(&monitor), address_(std::move(address)) {}

            TelephonyIds ids() const override {
                return {"bluez", BLUEZ_NAME, IFACE_TELEPHONY, nullptr, IFACE_CALL, true, true};
            }

            GDBusConnection* connection() const override { return monitor_->connection(); }

            TelephonySnapshot snapshot() const override {
                const BluezObjects objects = monitor_->snapshot();
                for (const auto& device : objects.devices) {
                    if (!iequals(device.address, address_))
                        continue;
                    if (const Telephony* found = objects.find_telephony(device.path))
                        return {*found, objects.calls_for(found->path), {}};
                }
                return {};
            }

        private:
            BluezMonitor* monitor_;
            std::string address_;
        };

    } // namespace

    std::string dial_argument(const TelephonyIds& ids, const std::string& number) {
        return ids.dial_uri ? "tel:" + number : number;
    }

    std::unique_ptr<TelephonySource> make_bluez_source(BluezMonitor& monitor, std::string address) {
        return std::make_unique<BluezSource>(monitor, std::move(address));
    }

    std::unique_ptr<TelephonyClient> make_telephony_client(BluezMonitor& monitor, std::string address) {
        std::vector<std::unique_ptr<TelephonySource>> sources;
        sources.push_back(make_bluez_source(monitor, address));
        sources.push_back(make_pipewire_source(address));
        return std::make_unique<TelephonyClient>(std::move(sources), std::move(address));
    }

    TelephonyClient::TelephonyClient(std::vector<std::unique_ptr<TelephonySource>> sources, std::string address)
        : sources_(std::move(sources)), address_(std::move(address)) {}

    std::pair<TelephonySource*, TelephonySnapshot> TelephonyClient::pick() const {
        for (const auto& source : sources_) {
            TelephonySnapshot snap = source->snapshot();
            if (!snap.gateway.path.empty())
                return {source.get(), std::move(snap)};
        }
        return {nullptr, {}};
    }

    bool TelephonyClient::available() const { return pick().second.gateway.ready(); }

    bool TelephonyClient::present() const { return pick().first != nullptr; }

    nlohmann::json TelephonyClient::status() const {
        const auto [source, snap] = pick();
        nlohmann::json j = to_json(snap.gateway);
        j["address"] = address_;
        j["calls"] = snap.calls.size();
        j["backend"] = source ? source->ids().name : "";
        j["indicators"] = !source || source->ids().indicators;
        j["audio"] = snap.audio_state;
        if (!source)
            j["reason"] = _("The iPhone has not connected Hands-Free to this computer.");
        else if (!snap.gateway.ready())
            j["reason"] = _("Connecting to the iPhone's Hands-Free service.");
        else if (source->ids().transport_iface)
            j["reason"] = _("PipeWire is handling Hands-Free, so the call audio can play on this computer.");
        else
            j["reason"] = _("Calls are controlled here; the audio plays on the iPhone.");
        return j;
    }

    nlohmann::json TelephonyClient::calls() const {
        // An ended call lingers as "disconnected" until the stack drops it, which
        // otherwise lists the same person twice when they call back.
        std::vector<Call> live = pick().second.calls;
        live.erase(std::remove_if(live.begin(), live.end(), [](const Call& c) { return c.state == "disconnected"; }),
                   live.end());
        nlohmann::json out = to_json(live);
        std::lock_guard<std::mutex> lock(audio_mutex_);
        for (auto& call : out)
            call["muted"] = muted_;
        return out;
    }

    bool TelephonyClient::set_muted(bool muted, std::string& err) {
        // The echo-cancelled call mic when it exists, the default mic otherwise.
        for (const char* source : {"tether_call_mic", "@DEFAULT_SOURCE@"}) {
            const std::string cmd =
                std::string("pactl set-source-mute ") + source + (muted ? " 1" : " 0") + " >/dev/null 2>&1";
            if (std::system(cmd.c_str()) == 0) {
                std::lock_guard<std::mutex> lock(audio_mutex_);
                muted_ = muted;
                return true;
            }
        }
        err = _("Could not reach the audio server to mute the microphone.");
        return false;
    }

    bool TelephonyClient::invoke(TelephonySource& source,
                                 const std::string& path,
                                 const char* iface,
                                 const char* method,
                                 GVariant* args,
                                 std::string& err) {
        GDBusConnection* conn = source.connection();
        if (!conn || path.empty()) {
            // Consumes the floating reference the caller built.
            if (args)
                g_variant_unref(g_variant_ref_sink(args));
            err = _("The iPhone has not connected Hands-Free to this computer.");
            return false;
        }

        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(conn,
                                                      source.ids().bus,
                                                      path.c_str(),
                                                      iface,
                                                      method,
                                                      args,
                                                      nullptr,
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      CALL_TIMEOUT_MS,
                                                      nullptr,
                                                      &error);
        if (!reply) {
            err = error ? error->message : "unknown error";
            g_clear_error(&error);
            debug::log(WARN, "telephony: {} failed: {}", method, err);
            return false;
        }
        g_variant_unref(reply);
        return true;
    }

    bool TelephonyClient::route_audio(TelephonySource& source,
                                      const TelephonySnapshot& snap,
                                      bool to_phone,
                                      std::string& err) {
        const char* transport = source.ids().transport_iface;
        if (!transport) {
            err = _("This computer is not carrying the call audio; it plays on the iPhone.");
            return false;
        }
        if (!invoke(source,
                    snap.gateway.path,
                    IFACE_PROPS,
                    "Set",
                    g_variant_new("(ssv)", transport, "RejectSCO", g_variant_new_boolean(to_phone)),
                    err))
            return false;
        if (to_phone)
            return true;

        std::string activate_err;
        if (!invoke(source, snap.gateway.path, transport, "Activate", nullptr, activate_err))
            debug::log(INFO, "telephony: no call audio to take yet: {}", activate_err);
        return true;
    }

    bool TelephonyClient::dial(const std::string& number, std::string& err) {
        const std::string dialable = normalize_dial_string(number);
        if (dialable.empty()) {
            err = _("Not a dialable number.");
            return false;
        }
        const auto [source, snap] = pick();
        if (!source) {
            err = _("The iPhone has not connected Hands-Free to this computer.");
            return false;
        }
        const std::string argument = dial_argument(source->ids(), dialable);
        return invoke(*source,
                      snap.gateway.path,
                      source->ids().gateway_iface,
                      "Dial",
                      g_variant_new("(s)", argument.c_str()),
                      err);
    }

    namespace {
        std::atomic<bool> g_calls_on_laptop{true};
    }

    void set_calls_on_laptop(bool on) { g_calls_on_laptop = on; }

    bool calls_on_laptop() { return g_calls_on_laptop; }

    bool TelephonyClient::claim_audio(TelephonySource& source, const TelephonySnapshot& snap, std::string& err) {
        if (!route_audio(source, snap, false, err))
            return false;
        std::lock_guard<std::mutex> lock(audio_mutex_);
        audio_claimed_ = true;
        claimed_call_seen_ = !snap.calls.empty();
        claimed_audio_active_ = false;
        claimed_at_ = std::chrono::steady_clock::now();
        for (const auto& call : snap.calls)
            on_phone_.erase(call.path);
        return true;
    }

    bool TelephonyClient::set_reject_sco(TelephonySource& source, const TelephonySnapshot& snap, bool reject) {
        std::string err;
        if (!invoke(source,
                    snap.gateway.path,
                    "org.freedesktop.DBus.Properties",
                    "Set",
                    g_variant_new("(ssv)", source.ids().transport_iface, "RejectSCO", g_variant_new_boolean(reject)),
                    err)) {
            debug::log(INFO, "telephony: could not set RejectSCO: {}", err);
            return false;
        }
        return true;
    }

    void TelephonyClient::settle_audio() {
        const auto [source, snap] = pick();
        std::lock_guard<std::mutex> lock(audio_mutex_);
        if (!source || !source->ids().transport_iface) {
            audio_claimed_ = claimed_call_seen_ = false;
            pulled_.clear();
            on_phone_.clear();
            idle_gateway_.clear();
            return;
        }
        const bool laptop = calls_on_laptop();

        if (snap.calls.empty()) {
            // Claimed with no call object: a dial from here that has not shown up
            // yet, or audio pulled for an app call (FaceTime, WhatsApp) the phone
            // never announces over Hands-Free. Hold the claim while that audio is
            // here; once it drops, or never arrives within a minute, it is over.
            if (audio_claimed_ && !claimed_call_seen_) {
                if (snap.audio_state == "active") {
                    claimed_audio_active_ = true;
                    return;
                }
                const bool expired = std::chrono::steady_clock::now() - claimed_at_ > std::chrono::seconds(60);
                if (!claimed_audio_active_ && !expired)
                    return;
            }
            claimed_audio_active_ = false;
            const bool ended = audio_claimed_ || !pulled_.empty();
            audio_claimed_ = claimed_call_seen_ = false;
            pulled_.clear();
            on_phone_.clear();
            if (muted_) {
                muted_ = false;
                std::system("pactl set-source-mute tether_call_mic 0 >/dev/null 2>&1; "
                            "pactl set-source-mute @DEFAULT_SOURCE@ 0 >/dev/null 2>&1");
            }
            // Between calls the gate follows the preference: open lets the phone
            // bring the next call straight here, shut keeps it on the earpiece.
            // PipeWire resets it for a new gateway, so that counts as unapplied.
            const std::string applied = snap.gateway.path + (laptop ? "#here" : "#phone");
            if (ended || idle_gateway_ != applied) {
                if (set_reject_sco(*source, snap, !laptop))
                    idle_gateway_ = applied;
            }
            return;
        }

        claimed_call_seen_ = true;
        // Each call is pulled here once, when it can carry audio. Once it has been
        // here, moving it to the phone from the phone is respected.
        const bool here = snap.audio_state == "active";
        for (const auto& call : snap.calls) {
            if (!(call.connected() || call.outgoing()) || pulled_.count(call.path) || on_phone_.count(call.path))
                continue;
            if (!laptop && !audio_claimed_)
                continue;
            if (here) {
                pulled_.insert(call.path);
                continue;
            }
            std::string err;
            if (set_reject_sco(*source, snap, false) &&
                invoke(*source, snap.gateway.path, source->ids().transport_iface, "Activate", nullptr, err)) {
                debug::log(INFO, "telephony: call audio brought to this computer");
                pulled_.insert(call.path);
            }
        }
    }

    bool TelephonyClient::call_action(const std::string& path, const std::string& action, std::string& err) {
        const auto [source, snap] = pick();
        if (!source) {
            err = _("The iPhone has not connected Hands-Free to this computer.");
            return false;
        }
        const std::vector<Call>& live = snap.calls;

        if (action == "answer" || action == "answer_here" || action == "hangup") {
            std::string target = path;
            if (target.empty() && live.size() == 1)
                target = live.front().path;
            if (target.empty()) {
                err = _("No call named.");
                return false;
            }
            if (std::none_of(live.begin(), live.end(), [&](const Call& c) { return c.path == target; })) {
                err = _("That call is no longer active.");
                return false;
            }
            // RejectSCO has to drop before the answer for the phone to bring the
            // voice link to this computer.
            if (action == "answer_here" && !claim_audio(*source, snap, err))
                return false;
            if (action == "answer") {
                // Picked "on iPhone": this call stays there even with laptop as the default.
                std::lock_guard<std::mutex> lock(audio_mutex_);
                on_phone_.insert(target);
                if (calls_on_laptop())
                    set_reject_sco(*source, snap, true);
            }
            return invoke(
                *source, target, source->ids().call_iface, action == "hangup" ? "Hangup" : "Answer", nullptr, err);
        }

        if (action == "mute" || action == "unmute")
            return set_muted(action == "mute", err);

        // Moving a call here is for that call: the default comes back once it ends.
        if (action == "audio_here")
            return claim_audio(*source, snap, err);
        if (action == "audio_phone")
            return route_audio(*source, snap, true, err);

        static const std::pair<const char*, const char*> gateway_actions[] = {
            {"hangup_all", "HangupAll"},
            // With no second call, AT+CHLD=2 holds the active call and resumes a held one.
            {"hold", "HoldAndAnswer"},
            {"swap", "SwapCalls"},
            {"hold_and_answer", "HoldAndAnswer"},
            {"release_and_answer", "ReleaseAndAnswer"},
            {"release_and_swap", "ReleaseAndSwap"},
            {"multiparty", "CreateMultiparty"},
        };
        for (const auto& [name, method] : gateway_actions)
            if (action == name)
                return invoke(*source, snap.gateway.path, source->ids().gateway_iface, method, nullptr, err);

        err = _("Unknown call action.");
        return false;
    }

    bool TelephonyClient::send_tones(const std::string& tones, std::string& err) {
        if (tones.empty() || tones.size() > MAX_TONES || !std::all_of(tones.begin(), tones.end(), [](unsigned char c) {
                return std::isdigit(c) || c == '*' || c == '#' || (c >= 'A' && c <= 'D');
            })) {
            err = _("Not a DTMF sequence.");
            return false;
        }
        const auto [source, snap] = pick();
        if (!source) {
            err = _("The iPhone has not connected Hands-Free to this computer.");
            return false;
        }
        return invoke(*source,
                      snap.gateway.path,
                      source->ids().gateway_iface,
                      "SendTones",
                      g_variant_new("(s)", tones.c_str()),
                      err);
    }

} // namespace tether::bluetooth
