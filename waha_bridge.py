from __future__ import annotations

import os
import re
import time
from collections import deque
from threading import Lock

import requests
from flask import Flask, jsonify, request
from dotenv import load_dotenv

load_dotenv()

app = Flask(__name__)

WAHA_BASE_URL = os.getenv("WAHA_BASE_URL", "http://127.0.0.1:3000").rstrip("/")
WAHA_API_KEY = os.getenv("WAHA_API_KEY", "")
WAHA_SESSION = os.getenv("WAHA_SESSION", "")
WAHA_WEBHOOK_SECRET = os.getenv("WAHA_WEBHOOK_SECRET", "")
ESP32_BASE_URL = os.getenv("ESP32_BASE_URL", "http://yourip").rstrip("/")
ESP32_API_KEY = os.getenv("ESP32_API_KEY", "")
ALLOWED_SENDERS = {
    re.sub(r"\D", "", item)
    for item in os.getenv("ALLOWED_SENDERS", "phone number").split(",")
    if re.sub(r"\D", "", item)
}

ALLOWED_LIDS = {
    item.strip().lower()
    for item in os.getenv("ALLOWED_LIDS", "").split(",")
    if item.strip()
}


# Per-sender state preserves the original /setfeed1 -> HH:MM workflow.
pending_schedule: dict[str, int] = {}
_seen_ids: set[str] = set()
_seen_order: deque[str] = deque()
_seen_lock = Lock()


def normalize_number(jid: str) -> str:
    """Extract digits from ordinary WhatsApp user JIDs such as 62812...@c.us."""
    if not jid or "@g.us" in jid:
        return ""
    return re.sub(r"\D", "", jid.split("@", 1)[0])


def send_whatsapp(chat_id: str, text: str) -> None:
    if not WAHA_API_KEY:
        raise RuntimeError("WAHA_API_KEY belum diisi pada file .env")
    response = requests.post(
        WAHA_BASE_URL + "/api/sendText",
        headers={
            "X-Api-Key": WAHA_API_KEY,
            "Accept": "application/json",
            "Content-Type": "application/json",
        },
        json={"chatId": chat_id, "text": text, "session": WAHA_SESSION},
        timeout=15,
    )
    if not response.ok:
        raise RuntimeError(f"WAHA sendText gagal: HTTP {response.status_code}: {response.text[:300]}")


def esp32_request(path: str, method: str = "GET", params: dict | None = None) -> str:
    if not ESP32_API_KEY:
        raise RuntimeError("ESP32_API_KEY belum diisi pada file .env")
    url = ESP32_BASE_URL + path
    headers = {"X-API-Key": ESP32_API_KEY, "Accept": "text/plain"}
    if method == "POST":
        response = requests.post(url, params=params or {}, headers=headers, timeout=8)
    else:
        response = requests.get(url, params=params or {}, headers=headers, timeout=8)
    if not response.ok:
        raise RuntimeError(f"ESP32 merespons HTTP {response.status_code}: {response.text[:300]}")
    return response.text.strip()


def remember_message(message_id: str) -> bool:
    """Return False for duplicate webhook deliveries, preventing double /feed."""
    if not message_id:
        return True
    with _seen_lock:
        if message_id in _seen_ids:
            return False
        _seen_ids.add(message_id)
        _seen_order.append(message_id)
        while len(_seen_order) > 500:
            old = _seen_order.popleft()
            _seen_ids.discard(old)
    return True


def valid_hhmm(text: str) -> bool:
    if not re.fullmatch(r"\d{2}:\d{2}", text):
        return False
    hour, minute = map(int, text.split(":"))
    return 0 <= hour <= 23 and 0 <= minute <= 59


def process_command(sender_jid: str, sender_number: str, text: str) -> None:
    command = text.strip().lower()

    # Preserve the original workflow: after /setfeed1 or /setfeed2,
    # the next message is interpreted as HH:MM until a valid time is sent.
    if sender_number in pending_schedule:
        if not valid_hhmm(text.strip()):
            send_whatsapp(sender_jid, "❌ Format salah. Gunakan HH:MM (contoh: 07:30).")
            return
        feed_number = pending_schedule[sender_number]
        endpoint = f"/api/setfeed{feed_number}"
        result = esp32_request(endpoint, method="POST", params={"time": text.strip()})
        pending_schedule.pop(sender_number, None)
        send_whatsapp(sender_jid, f"✅ Jadwal Feed {feed_number} telah diatur ke {text.strip()}.\n{result}")
        return

    if command in ("/start", "/help"):
        welcome = (
            "🐟 *FishFeeder / Smart Aquarium*\n\n"
            "/feed - Beri makan ikan sekarang\n"
            "/setfeed1 - Atur jadwal feed pertama\n"
            "/setfeed2 - Atur jadwal feed kedua\n"
            "/info - Lihat status perangkat"
        )
        send_whatsapp(sender_jid, welcome)
        return

    if command == "/feed":
        result = esp32_request("/api/feed", method="POST")
        send_whatsapp(sender_jid, "✅ Ikan telah diberi makan.\n" + result)
        return

    if command == "/setfeed1":
        pending_schedule[sender_number] = 1
        send_whatsapp(sender_jid, "Silakan masukkan waktu untuk Feed 1 dengan format HH:MM (contoh: 07:30).")
        return

    if command == "/setfeed2":
        pending_schedule[sender_number] = 2
        send_whatsapp(sender_jid, "Silakan masukkan waktu untuk Feed 2 dengan format HH:MM (contoh: 18:00).")
        return

    if command == "/info":
        info = esp32_request("/api/info")
        send_whatsapp(sender_jid, info)
        return

    # Unknown messages are not passed to ESP32. Respond with available commands.
    send_whatsapp(sender_jid, "Perintah tidak dikenal. Kirim /start untuk melihat daftar perintah.")


@app.get("/health")
def health():
    return jsonify({"ok": True, "service": "Smart Aquarium WAHA bridge"})


@app.post("/webhook")
def webhook():
    # WAHA adds this custom header via WHATSAPP_HOOK_CUSTOM_HEADERS.
    supplied_secret = request.headers.get("X-WAHA-Hook-Secret", "")
    if not WAHA_WEBHOOK_SECRET or supplied_secret != WAHA_WEBHOOK_SECRET:
        return jsonify({"ok": False, "error": "invalid webhook secret"}), 403

    event_data = request.get_json(silent=True) or {}
    if event_data.get("event") != "message":
        return jsonify({"ok": True, "ignored": "event"})

    payload = event_data.get("payload") or {}
    # Ignore messages generated by this linked WhatsApp account itself.
    if payload.get("fromMe") is True:
        return jsonify({"ok": True, "ignored": "fromMe"})

    sender_jid = str(payload.get("from") or "")
    sender_number = normalize_number(sender_jid)
    if not sender_jid or "@g.us" in sender_jid:
        return jsonify({"ok": True, "ignored": "not a direct chat"})

    
    sender_jid = sender_jid.strip().lower()

    if ( 
        sender_number not in ALLOWED_SENDERS
        and sender_jid not in ALLOWED_LIDS
    ):
        print(f"[DENIED] WhatsApp sender not allowed: {sender_jid}", flush=True)
        return jsonify({"ok": True, "ignored": "sender not allowed"})


    message_id = str(payload.get("id") or "")
    if not remember_message(message_id):
        return jsonify({"ok": True, "ignored": "duplicate"})

    text = str(payload.get("body") or "").strip()
    if not text:
        return jsonify({"ok": True, "ignored": "empty message"})

    print(f"[MESSAGE] {sender_jid}: {text}", flush=True)
    try:
        process_command(sender_jid, sender_number, text)
    except requests.RequestException as exc:
        print(f"[NETWORK ERROR] {exc}", flush=True)
        try:
            send_whatsapp(sender_jid, "❌ Tidak bisa menghubungi ESP32/WAHA. Periksa koneksi lokal dan status layanan.")
        except Exception as reply_exc:
            print(f"[REPLY ERROR] {reply_exc}", flush=True)
    except Exception as exc:
        print(f"[COMMAND ERROR] {exc}", flush=True)
        try:
            send_whatsapp(sender_jid, f"❌ Perintah gagal: {exc}")
        except Exception as reply_exc:
            print(f"[REPLY ERROR] {reply_exc}", flush=True)

    return jsonify({"ok": True})


if __name__ == "__main__":
    print("Smart Aquarium WAHA bridge starting...", flush=True)
    print("Webhook: http://127.0.0.1:5000/webhook", flush=True)
    print("WAHA:    " + WAHA_BASE_URL, flush=True)
    print("ESP32:   " + ESP32_BASE_URL, flush=True)
    if not WAHA_API_KEY or not WAHA_WEBHOOK_SECRET or not ESP32_API_KEY:
        print("WARNING: lengkapi WAHA_API_KEY, WAHA_WEBHOOK_SECRET, dan ESP32_API_KEY di .env", flush=True)
    if not ALLOWED_SENDERS:
        print("WARNING: ALLOWED_SENDERS kosong; semua pengirim akan ditolak.", flush=True)
    app.run(host="0.0.0.0", port=5000, debug=False)
