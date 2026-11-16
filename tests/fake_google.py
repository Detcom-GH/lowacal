#!/usr/bin/env python3
"""A stand-in for Google, for end-to-end tests: OAuth 2.0 for installed
apps (authorization endpoint, token endpoint, PKCE) plus a CalDAV endpoint
that - like the real one - accepts only "Authorization: Bearer <token>".

It checks what the real Google checks, so a client that would be rejected
there is rejected here:
  - redirect_uri must be a loopback address (http://127.0.0.1:<port>)
  - the code exchange must present the PKCE verifier matching the challenge
  - CalDAV with Basic auth, no auth or an unknown token -> 401
Test hooks (never part of real Google): POST /test/mode?m=allow|deny|no_calendar,
POST /test/revoke, GET /test/stats.
"""
import base64
import hashlib
import json
import re
import secrets
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, quote, urlencode, urlparse

CLIENT_ID = "test-client-id.apps.googleusercontent.com"
EMAIL = "artem@gmail.test"
CAL_SCOPE = "https://www.googleapis.com/auth/calendar"
BASE = "/caldav/v2/"
PRINCIPAL = BASE + quote(EMAIL, safe="") + "/user"
HOME = BASE + quote(EMAIL, safe="") + "/"
CAL = HOME + "events/"

S = {"mode": "allow", "codes": {}, "access": set(), "revoked": False,
     "stats": {"pkce_ok": 0, "pkce_bad": 0, "code_grants": 0, "refresh_grants": 0, "bearer_ok": 0, "auth_rejected": 0}}

# 14:00 Kyiv time on 20 July = 11:00 UTC (summer time). Google sends local
# time + TZID, exactly the shape the app used to misread as UTC.
EVENT = """BEGIN:VCALENDAR
VERSION:2.0
BEGIN:VEVENT
UID:google-event-1@google.com
DTSTART;TZID=Europe/Kyiv:20260720T140000
DTEND;TZID=Europe/Kyiv:20260720T150000
SUMMARY:Подія з Google
END:VEVENT
END:VCALENDAR
"""


def b64url(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def multistatus(inner):
    return ('<?xml version="1.0" encoding="utf-8"?><D:multistatus xmlns:D="DAV:" '
            'xmlns:caldav="urn:ietf:params:xml:ns:caldav" xmlns:cs="http://calendarserver.org/ns/" '
            'xmlns:ical="http://apple.com/ns/ical/">' + inner + '</D:multistatus>').encode()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stderr.write("  [fake-google] %s %s\n" % (self.command, self.path.split("?")[0]))

    def _send(self, code, body=b"", headers=None, ctype=None):
        self.send_response(code)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        if ctype:
            self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _json(self, code, obj):
        self._send(code, json.dumps(obj).encode(), ctype="application/json")

    def _body(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        return self.rfile.read(n).decode() if n else ""

    def _bearer_ok(self):
        auth = self.headers.get("Authorization", "")
        ok = auth.startswith("Bearer ") and auth[7:] in S["access"]
        S["stats"]["bearer_ok" if ok else "auth_rejected"] += 1
        return ok

    # ---------------- OAuth ----------------
    def do_GET(self):
        url = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        if url.path == "/test/stats":
            return self._json(200, S["stats"])
        if url.path != "/auth":
            return self._send(404)
        redirect = q.get("redirect_uri", "")
        problems = []
        if q.get("client_id") != CLIENT_ID:
            problems.append("unknown client_id")
        if not re.fullmatch(r"http://127\.0\.0\.1:\d+", redirect):
            problems.append("redirect_uri is not a loopback address")
        if q.get("response_type") != "code":
            problems.append("response_type must be code")
        if q.get("code_challenge_method") != "S256" or not q.get("code_challenge"):
            problems.append("PKCE S256 challenge missing")
        if CAL_SCOPE not in q.get("scope", "").split():
            problems.append("calendar scope not requested")
        if not q.get("state"):
            problems.append("state missing")
        if problems:
            return self._send(400, ("; ".join(problems)).encode(), ctype="text/plain")
        if S["mode"] == "deny":  # the user pressed "Cancel" on the consent page
            return self._send(302, headers={"Location": redirect + "/?" + urlencode({"error": "access_denied", "state": q["state"]})})
        code = secrets.token_urlsafe(16)
        scope = q["scope"] if S["mode"] != "no_calendar" else "openid email"  # user unticked the calendar box
        S["codes"][code] = {"challenge": q["code_challenge"], "redirect": redirect, "scope": scope}
        self._send(302, headers={"Location": redirect + "/?" + urlencode({"code": code, "state": q["state"]})})

    def do_POST(self):
        url = urlparse(self.path)
        if url.path == "/test/mode":
            S["mode"] = parse_qs(url.query)["m"][0]
            return self._send(204)
        if url.path == "/test/revoke":  # user removed the app's access in their Google account
            S["revoked"] = True
            S["access"].clear()
            return self._send(204)
        if url.path != "/token":
            return self._send(404)
        f = {k: v[0] for k, v in parse_qs(self._body()).items()}
        if f.get("client_id") != CLIENT_ID:
            return self._json(401, {"error": "invalid_client"})
        if f.get("grant_type") == "authorization_code":
            entry = S["codes"].pop(f.get("code", ""), None)
            if not entry or entry["redirect"] != f.get("redirect_uri"):
                return self._json(400, {"error": "invalid_grant"})
            verifier = f.get("code_verifier", "")
            if b64url(hashlib.sha256(verifier.encode()).digest()) != entry["challenge"]:
                S["stats"]["pkce_bad"] += 1
                return self._json(400, {"error": "invalid_grant", "error_description": "PKCE verification failed"})
            S["stats"]["pkce_ok"] += 1
            S["stats"]["code_grants"] += 1
            S["revoked"] = False
            token = "at-" + secrets.token_urlsafe(8)
            S["access"].add(token)
            id_token = ".".join([b64url(b'{"alg":"none"}'), b64url(json.dumps({"email": EMAIL}).encode()), "sig"])
            return self._json(200, {"access_token": token, "expires_in": 3599, "refresh_token": "rt-1",
                                    "scope": entry["scope"], "token_type": "Bearer", "id_token": id_token})
        if f.get("grant_type") == "refresh_token":
            if S["revoked"] or f.get("refresh_token") != "rt-1":
                return self._json(400, {"error": "invalid_grant", "error_description": "Token has been expired or revoked."})
            S["stats"]["refresh_grants"] += 1
            token = "at-" + secrets.token_urlsafe(8)
            S["access"].add(token)
            return self._json(200, {"access_token": token, "expires_in": 3599, "scope": CAL_SCOPE, "token_type": "Bearer"})
        self._json(400, {"error": "unsupported_grant_type"})

    # ---------------- CalDAV ----------------
    def do_PROPFIND(self):
        self._body()
        if not self._bearer_ok():
            return self._send(401, headers={"WWW-Authenticate": 'Bearer realm="fake-google"'})
        ok = "<D:status>HTTP/1.1 200 OK</D:status>"
        if self.path == BASE:
            inner = (f"<D:response><D:href>{BASE}</D:href><D:propstat><D:prop><D:current-user-principal>"
                     f"<D:href>{PRINCIPAL}</D:href></D:current-user-principal></D:prop>{ok}</D:propstat></D:response>")
        elif self.path == PRINCIPAL:
            inner = (f"<D:response><D:href>{PRINCIPAL}</D:href><D:propstat><D:prop><caldav:calendar-home-set>"
                     f"<D:href>{HOME}</D:href></caldav:calendar-home-set></D:prop>{ok}</D:propstat></D:response>")
        elif self.path == HOME:
            inner = (f"<D:response><D:href>{HOME}</D:href><D:propstat><D:prop><D:resourcetype><D:collection/>"
                     f"</D:resourcetype></D:prop>{ok}</D:propstat></D:response>"
                     f"<D:response><D:href>{CAL}</D:href><D:propstat><D:prop><D:resourcetype><D:collection/>"
                     f"<caldav:calendar/></D:resourcetype><D:displayname>{EMAIL}</D:displayname>"
                     f"<cs:getctag>g-1</cs:getctag><ical:calendar-color>#0B8043</ical:calendar-color>"
                     f"</D:prop>{ok}</D:propstat></D:response>")
        else:
            return self._send(404)
        self._send(207, multistatus(inner), ctype="application/xml; charset=utf-8")

    def do_REPORT(self):
        self._body()
        if not self._bearer_ok():
            return self._send(401)
        if self.path != CAL:
            return self._send(404)
        inner = (f"<D:response><D:href>{CAL}google-event-1.ics</D:href><D:propstat><D:prop><D:getetag>\"1\"</D:getetag>"
                 f"<caldav:calendar-data>{EVENT}</caldav:calendar-data></D:prop>"
                 "<D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>")
        self._send(207, multistatus(inner), ctype="application/xml; charset=utf-8")


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8766
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
