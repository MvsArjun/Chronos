import json
import os
import unittest
from unittest.mock import patch
import requests
from chronos.eulerpool import EulerpoolClient, EulerpoolError


class Response:
    def __init__(self, status, body, headers=None):
        self.status_code, self.body, self.headers = status, body, headers or {}
    def close(self):
        pass
    def iter_content(self, size):
        yield self.body


class Session:
    def __init__(self, responses):
        self.responses, self.calls = iter(responses), []
    def get(self, url, **kwargs):
        self.calls.append((url, kwargs))
        result = next(self.responses)
        if isinstance(result, Exception):
            raise result
        return result


class EulerpoolTests(unittest.TestCase):
    def client(self, responses, retries=2):
        self.session = Session(responses)
        with patch.dict(os.environ, {"EULERPOOL_API_KEY": "test-secret"}):
            return EulerpoolClient(session=self.session, retries=retries, sleep=lambda _: None)
    def test_auth_endpoint_and_success(self):
        client = self.client([Response(200, b'{}')])
        self.assertEqual(client.profile("AAPL"), {})
        url, kwargs = self.session.calls[0]
        self.assertEqual(url, "https://api.eulerpool.com/api/1/equity/profile/AAPL")
        self.assertNotIn("test-secret", url)
        self.assertFalse(kwargs["allow_redirects"])
    def test_rate_limit_and_network_retry(self):
        client = self.client([Response(429,b'',{"Retry-After":"0"}), requests.Timeout("test-secret"), Response(200,b'[]')])
        self.assertEqual(client.candles("AAPL"), [])
        self.assertEqual(len(self.session.calls), 3)
    def test_bad_response_and_no_auth_retry(self):
        for response in (Response(401,b'private'), Response(200,b'{bad'), Response(200,b'{}'), Response(200,b'[{}]')):
            client=self.client([response])
            with self.assertRaises(EulerpoolError) as error:
                client.candles("AAPL")
            self.assertNotIn("test-secret",str(error.exception))
            self.assertEqual(len(self.session.calls),1)
    def test_network_error_is_sanitized(self):
        client=self.client([requests.ConnectionError("test-secret")],retries=0)
        with self.assertRaises(EulerpoolError) as error: client.profile("AAPL")
        self.assertNotIn("test-secret",str(error.exception))
    def test_invalid_candles_and_range(self):
        client=self.client([Response(200,json.dumps([dict(timestamp=1,open=float('nan'),high=2,low=1,close=2)]).encode())])
        with self.assertRaises(EulerpoolError): client.candles("AAPL")
        with self.assertRaises(ValueError): client.candles("AAPL",range="undocumented")

if __name__ == "__main__": unittest.main()
