#!/usr/bin/env python3
"""One-time Oura OAuth login for RingBoard.

Reads the client id/secret from src/secrets.h, opens the Oura consent page in
your browser, catches the redirect on http://localhost:8080/callback, swaps
the code for tokens, and writes the refresh token back into src/secrets.h.
After that, flash the board (pio run -t upload) and it takes care of itself:
the firmware refreshes the access token as needed and keeps the rotating
refresh token in NVS.

Usage: python3 tools/oura_auth.py
"""

import re
import secrets as pysecrets
import sys
import threading
import urllib.parse
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

SECRETS_PATH = Path(__file__).resolve().parent.parent / "src" / "secrets.h"
AUTHORIZE_URL = "https://cloud.ouraring.com/oauth/authorize"
TOKEN_URL = "https://api.ouraring.com/oauth/token"
REDIRECT_URI = "http://localhost:8080/callback"
SCOPE = "daily heartrate workout spo2Daily"

result = {}
done = threading.Event()


def read_define(text, name):
    m = re.search(rf'#define\s+{name}\s+"([^"]*)"', text)
    if not m or not m.group(1):
        sys.exit(f"error: {name} not set in {SECRETS_PATH}")
    return m.group(1)


class Callback(BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path != "/callback":
            self.send_error(404)
            return
        params = urllib.parse.parse_qs(parsed.query)
        result["code"] = params.get("code", [None])[0]
        result["state"] = params.get("state", [None])[0]
        result["error"] = params.get("error", [None])[0]
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.end_headers()
        self.wfile.write(
            b"<html><body style='font-family:sans-serif;padding:40px'>"
            b"<h2>RingBoard is authorized.</h2>"
            b"<p>You can close this tab and go back to the terminal.</p>"
            b"</body></html>"
        )
        done.set()

    def log_message(self, *args):
        pass


def main():
    text = SECRETS_PATH.read_text()
    client_id = read_define(text, "OURA_CLIENT_ID")
    client_secret = read_define(text, "OURA_CLIENT_SECRET")

    state = pysecrets.token_urlsafe(16)
    auth_url = AUTHORIZE_URL + "?" + urllib.parse.urlencode(
        {
            "response_type": "code",
            "client_id": client_id,
            "redirect_uri": REDIRECT_URI,
            "scope": SCOPE,
            "state": state,
        }
    )

    server = HTTPServer(("localhost", 8080), Callback)
    threading.Thread(target=server.serve_forever, daemon=True).start()

    print("Opening the Oura consent page in your browser...")
    print(f"If nothing opens, paste this URL yourself:\n\n  {auth_url}\n")
    webbrowser.open(auth_url)

    if not done.wait(timeout=300):
        sys.exit("error: timed out waiting for the redirect (5 minutes)")
    server.shutdown()

    if result.get("error"):
        sys.exit(f"error: Oura returned '{result['error']}'")
    if result.get("state") != state:
        sys.exit("error: state mismatch, try again")
    code = result.get("code")
    if not code:
        sys.exit("error: no authorization code in the redirect")

    body = urllib.parse.urlencode(
        {
            "grant_type": "authorization_code",
            "code": code,
            "redirect_uri": REDIRECT_URI,
            "client_id": client_id,
            "client_secret": client_secret,
        }
    ).encode()
    req = urllib.request.Request(
        TOKEN_URL,
        data=body,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            import json

            tokens = json.load(resp)
    except urllib.error.HTTPError as e:
        sys.exit(f"error: token exchange failed: HTTP {e.code} {e.read().decode()}")

    refresh = tokens.get("refresh_token")
    if not refresh:
        sys.exit(f"error: no refresh_token in response: {tokens}")

    new_text = re.sub(
        r'(#define\s+OURA_REFRESH_TOKEN\s+)"[^"]*"',
        rf'\g<1>"{refresh}"',
        text,
    )
    SECRETS_PATH.write_text(new_text)
    print(f"Refresh token written to {SECRETS_PATH}")
    print("Now flash the board:  pio run -t upload")


if __name__ == "__main__":
    main()
