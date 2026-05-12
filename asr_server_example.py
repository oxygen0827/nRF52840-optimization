"""
Minimal local ASR proxy for web/index.html.

Run from the voice-keyboard project root if you want to reuse its STTClient:

    set GLM_API_KEY=...
    python C:\\Users\\19051\\Desktop\\ai_deploy\\nRF52840-optimization\\asr_server_example.py

Endpoint:
    POST http://localhost:8787/asr

Request:
    multipart/form-data file=audio.wav
    or raw audio/wav body

Response:
    {"text": "..."}
"""

from __future__ import annotations

import json
import os
import sys
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


HOST = "127.0.0.1"
PORT = 8787
VOICE_KEYBOARD_ROOT = Path(r"C:\Users\19051\Desktop\ai_deploy\voice-keyboard-mac")


def _load_stt_client():
    sys.path.insert(0, str(VOICE_KEYBOARD_ROOT))
    from agent.stt import STTClient

    provider = os.getenv("ASR_PROVIDER", "glm_asr_2512")
    cfg = {
        "provider": provider,
        "api_key": os.getenv("GLM_API_KEY", ""),
        "model": os.getenv("ASR_MODEL", "glm-asr-2512"),
    }

    if provider == "xunfei":
        cfg = {
            "provider": "xunfei",
            "app_id": os.getenv("XUNFEI_APPID", ""),
            "api_key": os.getenv("XUNFEI_APIKEY", ""),
            "api_secret": os.getenv("XUNFEI_APISECRET", ""),
            "language": os.getenv("ASR_LANGUAGE", "zh_cn"),
        }

    return STTClient(cfg)


STT = _load_stt_client()


def _wav_to_pcm(wav_bytes: bytes) -> bytes:
    import io

    with wave.open(io.BytesIO(wav_bytes), "rb") as w:
        channels = w.getnchannels()
        rate = w.getframerate()
        width = w.getsampwidth()
        pcm = w.readframes(w.getnframes())

    if channels != 1 or rate != 16000 or width != 2:
        raise ValueError(f"expected 16kHz mono int16 WAV, got {rate}Hz {channels}ch {width * 8}bit")
    return pcm


def _extract_multipart_file(body: bytes, content_type: str) -> bytes:
    marker = "boundary="
    if marker not in content_type:
        raise ValueError("multipart request missing boundary")

    boundary = content_type.split(marker, 1)[1].split(";", 1)[0].strip().strip('"')
    delimiter = ("--" + boundary).encode("utf-8")

    for part in body.split(delimiter):
        part = part.strip()
        if not part or part == b"--":
            continue
        if part.endswith(b"--"):
            part = part[:-2].rstrip()

        header_end = part.find(b"\r\n\r\n")
        if header_end < 0:
            continue
        headers = part[:header_end].decode("utf-8", errors="replace")
        payload = part[header_end + 4:]
        if 'name="file"' in headers:
            return payload.rstrip(b"\r\n")

    raise ValueError('multipart request missing field "file"')


class Handler(BaseHTTPRequestHandler):
    def do_OPTIONS(self):
        self._cors(204)

    def do_POST(self):
        if self.path != "/asr":
            self.send_error(404)
            return

        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        content_type = self.headers.get("Content-Type", "")

        try:
            if "multipart/form-data" in content_type:
                wav_bytes = _extract_multipart_file(body, content_type)
            else:
                wav_bytes = body

            pcm = _wav_to_pcm(wav_bytes)
            text = STT.transcribe(pcm)
            payload = json.dumps({"text": text}, ensure_ascii=False).encode("utf-8")
            self._cors(200, "application/json")
            self.wfile.write(payload)
        except Exception as exc:
            payload = json.dumps({"error": str(exc)}, ensure_ascii=False).encode("utf-8")
            self._cors(500, "application/json")
            self.wfile.write(payload)

    def _cors(self, status: int, content_type: str = "text/plain"):
        self.send_response(status)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Authorization, Content-Type")
        self.send_header("Content-Type", content_type + "; charset=utf-8")
        self.end_headers()


if __name__ == "__main__":
    print(f"ASR proxy listening on http://{HOST}:{PORT}/asr")
    ThreadingHTTPServer((HOST, PORT), Handler).serve_forever()
