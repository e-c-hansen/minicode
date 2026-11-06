# fake_lsp.py — a scripted language server for tests/gui/LanguageSuite.mm.
#
#   python3 fake_lsp.py SCRIPT.json LOG.jsonl
#
# Logs every message it reads to LOG (one JSON object a line) and answers by
# the script, which is read again for every message, so a test can change
# what it does while it runs:
#
#   {"capabilities": {...},                 sent in the initialize reply
#    "responses": {"textDocument/rename": {"result": ..., "delay": 1.5},
#                  "workspace/symbol": "never"},
#    "publish": [{"uri": ..., "diagnostics": [...]}],   after each didOpen/didChange
#    "stopReading": true}                   after initialize: never read again
#
# A request with no entry gets a null result. Only the Python standard
# library, so it runs wherever python3 does.
import json
import sys
import threading
import time

script_path, log_path = sys.argv[1], sys.argv[2]
out_lock = threading.Lock()


def script():
    try:
        with open(script_path) as f:
            return json.load(f)
    except Exception:
        return {}


def log(msg):
    with open(log_path, "a") as f:
        f.write(json.dumps(msg) + "\n")


def send(msg):
    body = json.dumps(msg).encode("utf-8")
    with out_lock:
        sys.stdout.buffer.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        sys.stdout.buffer.flush()


def read_message(stream):
    length = None
    while True:
        line = stream.readline()
        if not line:
            return None
        line = line.strip()
        if not line:
            break
        name, _, value = line.partition(b":")
        if name.strip().lower() == b"content-length":
            length = int(value.strip())
    if length is None:
        return {}
    return json.loads(stream.read(length).decode("utf-8"))


def main():
    stream = sys.stdin.buffer
    while True:
        msg = read_message(stream)
        if msg is None:
            return
        log(msg)
        s = script()
        method = msg.get("method")
        if "id" in msg and method:
            if method == "initialize":
                send({"jsonrpc": "2.0", "id": msg["id"],
                      "result": {"capabilities": s.get("capabilities", {}),
                                 "serverInfo": {"name": "fakelsp"}}})
                continue
            if method == "shutdown":
                send({"jsonrpc": "2.0", "id": msg["id"], "result": None})
                continue
            entry = s.get("responses", {}).get(method)
            if entry == "never":
                continue
            result = entry.get("result") if isinstance(entry, dict) else None
            delay = entry.get("delay", 0) if isinstance(entry, dict) else 0
            reply = {"jsonrpc": "2.0", "id": msg["id"], "result": result}
            if delay:
                threading.Timer(delay, send, [reply]).start()
            else:
                send(reply)
            continue
        if method == "exit":
            return
        if method == "initialized" and s.get("stopReading"):
            while True:
                time.sleep(3600)
        if method in ("textDocument/didOpen", "textDocument/didChange"):
            doc = msg["params"]["textDocument"]
            for p in s.get("publish", []):
                if p.get("uri") == doc["uri"]:
                    send({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics",
                          "params": {"uri": doc["uri"], "version": doc.get("version"),
                                     "diagnostics": p["diagnostics"]}})


main()
