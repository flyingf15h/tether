#pragma once

#include "tether/bluetooth/messages.hpp"
#include "tether/secret_store.hpp"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace tether::bluetooth {

    // High enough to hold a whole imported iPhone history.
    inline constexpr size_t JOURNAL_MAX_MESSAGES = 500000;
    inline constexpr int64_t JOURNAL_MAX_AGE_SECONDS = 90LL * 24 * 3600;

    // Encrypted retention keeps messages.ndjson.enc; plaintext keeps the
    // readable messages.ndjson an older tetherd would also write. Nothing ever
    // writes a sealed record to the plaintext name, which is what makes running
    // an older daemon against this store harmless.
    std::string journal_path(Retention mode);
    std::string journal_path();

    // Rewrites the journal from one mode's file into the other's and removes the
    // source. No-op unless the source exists and the destination does not, so it
    // is safe to call on every open. False on any refusal, including a source
    // that could not be read.
    bool migrate_journal(Retention from, Retention to);

    // Append-only newline-delimited JSON, one record per line, mode 0600. Each
    // record is sealed under the active retention mode.
    class MessageJournal {
    public:
        // Creates the directory and opens the file for appending, mode 0600.
        // False when retention is off, or when it is encrypted and the wallet
        // has no key to offer yet: the caller then runs live-only and retries.
        bool open();
        bool is_open() const { return out_.is_open(); }
        void close();

        void append(const Message& message);

        // Malformed lines are skipped rather than aborting the load
        std::vector<Message> load(int64_t now) const;

        // Rewrites the file to exactly `messages`, atomically. Refuses when the
        // last load could not read the whole file, so a wrong key or a truncated
        // record cannot be compacted away.
        bool compact(const std::vector<Message>& messages);

    private:
        std::ofstream out_;
        std::string path_;
        mutable size_t skipped_ = 0;
    };

    std::string serialize_message_line(const Message& message);
    bool deserialize_message_line(const std::string& line, Message& out);

    // Drops anything older than max_age, then keeps the newest max_messages.
    std::vector<Message> apply_retention(std::vector<Message> messages,
                                         int64_t now,
                                         size_t max_messages = JOURNAL_MAX_MESSAGES,
                                         int64_t max_age = JOURNAL_MAX_AGE_SECONDS);

} // namespace tether::bluetooth
