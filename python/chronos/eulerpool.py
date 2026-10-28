"""Small opt-in client for endpoints confirmed in official Eulerpool docs.

Raw vendor JSON is deliberately kept out of replay until a dataset-specific
timestamp/adjustment/availability contract has been established.
"""
from __future__ import annotations

import json
import math
import os
import time
from urllib.parse import quote

import requests


class EulerpoolError(RuntimeError):
    pass


class EulerpoolClient:
    BASE = "https://api.eulerpool.com/api/1"
    MAX_BODY = 16 * 1024 * 1024

    def __init__(self, *, session=None, timeout=15.0, retries=2, sleep=time.sleep):
        self._key = os.environ.get("EULERPOOL_API_KEY")
        if not self._key:
            raise EulerpoolError("Set EULERPOOL_API_KEY before using the optional adapter")
        if not math.isfinite(timeout) or timeout <= 0 or not isinstance(retries, int) or not 0 <= retries <= 5:
            raise ValueError("Invalid timeout or retry count")
        self._session = session or requests.Session()
        self._timeout, self._retries, self._sleep = timeout, retries, sleep

    def _get(self, endpoint, params, expected):
        for attempt in range(self._retries + 1):
            response = None
            retry_delay = min(2**attempt, 30)
            try:
                response = self._session.get(
                    self.BASE + endpoint, params=params,
                    headers={"Authorization": "Bearer " + self._key, "Accept": "application/json"},
                    timeout=self._timeout, allow_redirects=False, stream=True,
                )
                if response.status_code == 429 or 500 <= response.status_code <= 599:
                    hint = response.headers.get("Retry-After", "")
                    if hint.isdigit():
                        retry_delay = min(int(hint), 30)
                    # Never wait a month for exhausted monthly quota.
                    if attempt == self._retries:
                        raise EulerpoolError("Eulerpool temporarily unavailable or rate limited")
                elif response.status_code != 200:
                    raise EulerpoolError(f"Eulerpool request failed (HTTP {response.status_code})")
                else:
                    chunks, length = [], 0
                    for chunk in response.iter_content(65536):
                        length += len(chunk)
                        if length > self.MAX_BODY:
                            raise EulerpoolError("Eulerpool response exceeds size limit")
                        chunks.append(chunk)
                    try:
                        result = json.loads(b"".join(chunks))
                    except (ValueError, UnicodeError):
                        raise EulerpoolError("Malformed Eulerpool JSON") from None
                    if not isinstance(result, expected):
                        raise EulerpoolError("Unexpected Eulerpool response schema")
                    return result
            except requests.RequestException:
                if attempt == self._retries:
                    # Exceptions can contain URLs/headers. Never propagate vendor text.
                    raise EulerpoolError("Eulerpool network request failed") from None
            finally:
                if response is not None:
                    response.close()
            self._sleep(retry_delay)
        raise EulerpoolError("Eulerpool retries exhausted")

    @staticmethod
    def _identifier(identifier):
        if not isinstance(identifier, str) or not identifier or len(identifier) > 64:
            raise ValueError("Invalid instrument identifier")
        return quote(identifier, safe="")

    def profile(self, identifier):
        return self._get("/equity/profile/" + self._identifier(identifier), {"language": "en"}, dict)

    def candles(self, identifier, range="1y"):
        if range not in {"1m", "3m", "6m", "1y", "2y", "5y", "max"}:
            raise ValueError("Undocumented candle range")
        rows = self._get("/equity/candles/" + self._identifier(identifier), {"range": range}, list)
        for row in rows:
            if not isinstance(row, dict) or not {"timestamp", "open", "high", "low", "close"} <= row.keys():
                raise EulerpoolError("Missing candle fields")
            for name in ("timestamp", "open", "high", "low", "close"):
                value = row[name]
                if isinstance(value, bool) or not isinstance(value, (float, int)) or not math.isfinite(value):
                    raise EulerpoolError("Invalid candle field")
        return rows
