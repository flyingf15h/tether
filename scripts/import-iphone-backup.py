#!/usr/bin/env python3
"""Import Messages history from an unencrypted iPhone backup into tetherd.

MAP only shows what arrived after this computer was given access, so older
conversations (and your own replies typed on the phone) exist only in the
phone's sms.db. A local backup made with `idevicebackup2 backup --full DIR`
contains it.

    scripts/import-iphone-backup.py ~/iPhoneBackup

Reactions are written the way MAP delivers them ("Loved “…”"), and replies
with the "> “quote”" first line the GTK app shows as a thread, so imported
history renders like live messages. Group chats are skipped: MAP has no
conversation id to tie them to.
"""

import json
import os
import shutil
import socket
import sqlite3
import sys
import tempfile

APPLE_EPOCH = 978307200
# SHA-1 of "HomeDomain-Library/SMS/sms.db": its name in every backup.
SMS_DB_ID = "3d0d7e5fb2ce288813306e4d4636395e047a3d28"
TAPBACKS = {2000: "Loved", 2001: "Liked", 2002: "Disliked", 2003: "Laughed at", 2004: "Emphasized", 2005: "Questioned"}
OBJECT_CHAR = "￼"
THUMBS_UP = "\U0001F44D"


def find_sms_db(root):
    """The backup's sms.db, located through Manifest.db."""
    candidates = [root] + [os.path.join(root, d) for d in sorted(os.listdir(root))]
    for base in candidates:
        manifest = os.path.join(base, "Manifest.db")
        if not os.path.exists(manifest):
            continue
        db = sqlite3.connect(f"file:{manifest}?mode=ro", uri=True)
        row = db.execute(
            "SELECT fileID FROM Files WHERE domain='HomeDomain' AND relativePath='Library/SMS/sms.db'"
        ).fetchone()
        db.close()
        if row:
            path = os.path.join(base, row[0][:2], row[0])
            if os.path.exists(path):
                return path
    # A backup still in progress keeps its files under Snapshot/ and has no manifest yet.
    for base in candidates:
        for sub in ("", "Snapshot"):
            path = os.path.join(base, sub, SMS_DB_ID[:2], SMS_DB_ID)
            if os.path.exists(path):
                return path
    raise SystemExit(f"No sms.db in {root}. Is the backup finished and unencrypted?")


def attributed_text(blob):
    """Plain text from the NSAttributedString iOS 16+ stores instead of `text`."""
    if not blob:
        return None
    start = blob.find(b"NSString")
    if start < 0:
        return None
    plus = blob.find(b"\x2b", start)
    if plus < 0 or plus + 1 >= len(blob):
        return None
    length, offset = blob[plus + 1], plus + 2
    if length == 0x81:
        length, offset = int.from_bytes(blob[plus + 2:plus + 4], "little"), plus + 4
    elif length == 0x82:
        length, offset = int.from_bytes(blob[plus + 2:plus + 5], "little"), plus + 5
    return blob[offset:offset + length].decode("utf-8", "replace")


def timestamp(value):
    if not value:
        return 0
    # Nanoseconds since 2001 from iOS 11 on, seconds before that.
    seconds = value / 1e9 if value > 1e11 else value
    return int(seconds + APPLE_EPOCH)


def snippet(text, limit=60):
    flat = text.replace("\n", " ")
    return flat if len(flat) <= limit else flat[:limit] + "…"


def attachment_label(mime):
    mime = mime or ""
    if mime.startswith("image/"):
        return "\U0001F4F7 Photo"
    if mime.startswith("video/"):
        return "\U0001F3A5 Video"
    if mime.startswith("audio/"):
        return "\U0001F3A4 Audio message"
    return "\U0001F4CE Attachment"


def read_messages(path):
    work = tempfile.mkdtemp()
    copy = os.path.join(work, "sms.db")
    shutil.copy(path, copy)
    db = sqlite3.connect(copy)
    db.row_factory = sqlite3.Row
    columns = {r[1] for r in db.execute("PRAGMA table_info(message)")}
    optional = [c for c in ("attributedBody", "thread_originator_guid", "balloon_bundle_id",
                            "associated_message_emoji") if c in columns]
    rows = db.execute(f"""
        SELECT m.ROWID AS id, m.guid, m.text, m.date, m.is_from_me, m.is_read, m.item_type,
               m.associated_message_type AS assoc_type, m.associated_message_guid AS assoc_guid,
               m.cache_has_attachments AS has_attachments,
               {''.join(f'm.{c}, ' for c in optional)}
               c.chat_identifier AS chat, c.style AS style,
               (SELECT a.mime_type FROM message_attachment_join maj JOIN attachment a ON a.ROWID = maj.attachment_id
                 WHERE maj.message_id = m.ROWID LIMIT 1) AS mime
        FROM message m
        JOIN chat_message_join cmj ON cmj.message_id = m.ROWID
        JOIN chat c ON c.ROWID = cmj.chat_id
        ORDER BY m.date""").fetchall()
    db.close()
    shutil.rmtree(work, ignore_errors=True)
    return rows


def convert(rows):
    by_guid = {}
    for row in rows:
        text = row["text"] or attributed_text(row["attributedBody"] if "attributedBody" in row.keys() else None) or ""
        by_guid[row["guid"]] = text.replace(OBJECT_CHAR, "").strip()

    out, groups, skipped = [], set(), 0
    for row in rows:
        if row["style"] == 43:          # group chat
            groups.add(row["chat"])
            continue
        if row["item_type"] not in (0, None):
            skipped += 1                # renames, joins and other events
            continue
        body = by_guid[row["guid"]]
        keys = row.keys()

        kind = row["assoc_type"] or 0
        if 2000 <= kind <= 2006:
            target_guid = (row["assoc_guid"] or "").split("/")[-1].split(":")[-1]
            target = by_guid.get(target_guid, "")
            if not target:
                skipped += 1
                continue
            if kind == 2006:
                emoji = row["associated_message_emoji"] if "associated_message_emoji" in keys else ""
                body = f"Reacted {emoji or THUMBS_UP} to “{target}”"
            else:
                body = f"{TAPBACKS[kind]} “{target}”"
        elif 3000 <= kind <= 3006:
            skipped += 1                # a reaction taken back
            continue
        else:
            bundle = (row["balloon_bundle_id"] if "balloon_bundle_id" in keys else "") or ""
            if "gamepigeon" in bundle.lower():
                body = "\U0001F3AE GamePigeon game"
            elif not body and row["has_attachments"]:
                body = attachment_label(row["mime"])
            elif row["has_attachments"]:
                body = f"{attachment_label(row['mime'])}\n{body}"
            if not body:
                skipped += 1
                continue
            origin = row["thread_originator_guid"] if "thread_originator_guid" in keys else None
            if origin and by_guid.get(origin):
                body = f"> “{snippet(by_guid[origin])}”\n{body}"

        out.append({
            "handle": f"backup-{row['guid']}",
            "address": row["chat"],
            "body": body,
            "timestamp": timestamp(row["date"]),
            "outgoing": bool(row["is_from_me"]),
            "read": bool(row["is_read"]) or bool(row["is_from_me"]),
        })
    return out, groups, skipped


def ask_daemon(command, reply, timeout):
    """Sends one command to tetherd and returns the first line answering it."""
    sock_path = os.path.join(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}"), "tether", "tetherd.sock")
    s = socket.socket(socket.AF_UNIX)
    try:
        s.connect(sock_path)
    except OSError as e:
        raise SystemExit(f"tetherd is not running ({sock_path}: {e.strerror}).")
    s.settimeout(timeout)
    s.sendall((json.dumps(command) + "\n").encode())
    buf = b""
    while True:
        chunk = s.recv(65536)
        if not chunk:
            raise SystemExit("tetherd closed the connection before answering")
        buf += chunk
        for line in buf.split(b"\n"):
            if reply.encode() in line:
                return json.loads(line)


def check_daemon():
    """Refuses a stock tetherd: it would not take the import, and on its next start it
    ages out anything older than 90 days, which is all of a backup's history."""
    status = ask_daemon({"command": "bt_status"}, '"bt_status"', 10)
    if "calls_on_laptop" not in status:
        raise SystemExit(
            f"The running tetherd (version {status.get('version', '?')}) is not this fork. Install the fork "
            "(the .deb or `sudo cmake --install build/release`), restart it with "
            "`systemctl --user restart tetherd`, then run this again.")


def send_to_daemon(path):
    return ask_daemon({"command": "bt_import_messages", "path": path}, '"bt_import_result"', 600)


def main():
    args = [a for a in sys.argv[1:] if a != "--dry-run"]
    if len(args) != 1:
        raise SystemExit(__doc__)
    rows = read_messages(find_sms_db(os.path.expanduser(args[0])))
    messages, groups, skipped = convert(rows)
    if "--dry-run" in sys.argv:
        people = {m["address"] for m in messages}
        mine = sum(m["outgoing"] for m in messages)
        span = [m["timestamp"] for m in messages if m["timestamp"]]
        import datetime
        print(f"{len(messages)} messages with {len(people)} people ({mine} from you), "
              f"{len(groups)} group chats skipped, {skipped} skipped.")
        if span:
            print("from", datetime.datetime.fromtimestamp(min(span)), "to", datetime.datetime.fromtimestamp(max(span)))
        kinds = {"reactions": sum(m["body"].startswith(tuple(TAPBACKS.values()) + ("Reacted ",)) for m in messages),
                 "replies": sum(m["body"].startswith("> \u201c") for m in messages)}
        print(kinds)
        return
    check_daemon()
    fd, path = tempfile.mkstemp(prefix="tether-import-", suffix=".jsonl", dir=os.environ.get("XDG_RUNTIME_DIR"))
    with os.fdopen(fd, "w") as f:
        for message in messages:
            f.write(json.dumps(message, ensure_ascii=False) + "\n")
    try:
        result = send_to_daemon(path)
    finally:
        os.unlink(path)
    print(f"{len(messages)} messages read, {result['added']} new, "
          f"{len(groups)} group chats skipped, {skipped} events/empty skipped.")


if __name__ == "__main__":
    main()
