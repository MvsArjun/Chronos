# Eulerpool integration review — 2026-10-28

The optional Python client implements only two documented GET endpoints. It
returns raw JSON; it does not invent historical availability or normalize an
unverified vendor dataset into trading events. No authenticated live response was
available during this implementation. Adapter tests use explicitly synthetic
HTTP fixtures and establish client behavior, not vendor compatibility.

## Confirmed public documentation

* [Authentication](https://eulerpool.com/developers/authentication): HTTPS base
  `https://api.eulerpool.com/api/1`; query token or Bearer header. Chronos uses a
  Bearer header from `EULERPOOL_API_KEY`, refuses redirects, and sanitizes errors.
* [Profile](https://eulerpool.com/developers/api/equity/profile):
  `/equity/profile/{identifier}`, `language=en`. The detailed example is an empty
  object, so the adapter asserts object shape without inventing required fields.
* [Candles](https://eulerpool.com/developers/api/equity/candles):
  `/equity/candles/{identifier}`, range in `1m,3m,6m,1y,2y,5y,max`.
  It describes daily OHLC approximated from quotes; volume is optional. Example
  fields include numeric timestamp and OHLC. This is unsuitable evidence for
  precise intraday liquidity. The timestamp example resembles Unix milliseconds,
  but the adapter preserves it raw pending a confirmed unit/timezone contract.
* [Rate limits](https://eulerpool.com/developers/rate-limits): public free tier is
  described as 100,000 requests per month; quota headers expose limit, remaining
  and UTC reset. A 429 may represent monthly exhaustion. Retries are bounded,
  honor numeric Retry-After up to 30 seconds and never wait for a monthly reset.
* [Historical data](https://eulerpool.com/financial-data-api/historical-data) and
  [fundamentals](https://eulerpool.com/financial-data-api/fundamentals-data) pages
  advertise long histories, corporate-action adjustments, as-reported statements
  and point-in-time data. Those are vendor claims. Detailed price routes differ
  between overview examples and the endpoint catalog; Chronos uses the catalog.

## Deliberate integration boundary

The income-statement catalog documents
[/equity/incomestatement/{identifier}](https://eulerpool.com/developers/api/equity/incomestatement).
Income statements, balance sheets and cash-flow statements are advertised, but
their availability/revision contracts have not been validated here. Do not put
fiscal-period dates into the factor framework's publication/availability fields.

Profile and candle endpoint pages specify no pagination parameters. The adapter
does not add any. Other historical overview pages mention limit/offset, which is
insufficient evidence to apply those parameters to these endpoints. A paginated
object instead of the documented candle array is rejected. No silent first-page
truncation is accepted as a complete historical dataset.

Before extending integration: verify the actual response and entitlement, archive
schema/version and request metadata without credentials, establish exchange
session/timezone, timestamp units, receipt and publication timestamps, revision
policy, adjustment basis and pagination termination. Convert through the local
CSV/Parquet schema and run its validators. Use unadjusted prices with explicit
corporate actions, or adjusted research prices without double-counting actions.
Coverage and plan entitlements must be checked for the user's account; no
subscription purchase is required for Chronos's local workflows.

```python
from chronos.eulerpool import EulerpoolClient
client = EulerpoolClient()  # requires EULERPOOL_API_KEY in the environment
raw_profile = client.profile("US0378331005")
raw_candles = client.candles("US0378331005", range="1y")
```
