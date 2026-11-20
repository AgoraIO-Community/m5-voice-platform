# Interfaces

## `GET /api/session/me`

Returns authentication state, auth mode, and the current user. It has no quota or storage side effects.

## `/api/v3/[...resource]`

Authenticated, route-table-backed access to the v3 preview API through the `agora-agents@3.0.0-alpha.4` SDK (`client.v3.*`). Resources: `agents`, `secrets` (id = set name), `numbers`, `campaigns`, `sessions`. `GET`/`POST` on the collection list/create; `GET`/`PATCH`/`DELETE` on `{id}`; `POST campaigns/{id}/{pause|resume|cancel}` and `POST sessions/{id}/stop`. Sessions are read-only except stop (start goes through `/api/call`). The server fetches ETags itself for agent/number/campaign PATCH and secret-set DELETE. Bodies and responses use v3 wire JSON; Errors map by the SDK's error category: API errors return the upstream status with `{error, reason, requestId, details}` (a 2xx response that violates the API contract becomes 502); validation errors and malformed input are 400; connection failures 502 and timeouts 504; configuration errors 500 with their message; unexpected failures a generic 500 logged server-side. The selected project comes from a signed HTTP-only cookie, with `AGORA_APP_ID` as fallback.

## `/api/projects`

- `GET`: returns safe project objects and the selected App ID.
- `POST`: accepts `{ "name": "Example customer" }`, creates a project with its certificate enabled, strips the certificate from the response, and selects the project.
- `PUT`: accepts `{ "appId": "..." }`, verifies that it belongs to the authenticated Agora customer, and stores it in a signed HTTP-only cookie.

Project responses intentionally omit `sign_key`, recording-server data, and every other server-only Console field.

## `POST /api/call`

Input:

```json
{ "to": "+15551234567", "agentId": "<v3 agent uuid>", "phoneNumberId": "<v3 number uuid>" }
```

This is the demo's only outbound-call interface. It starts a v3 session with a telephony transport (`from` = number UUID, `to` = E.164), a 120 s idle timeout and 300 s max duration, and returns `{ok, agentSessionId, status}`. The v3 API remains authoritative for call eligibility and request validation.

## `/api/agent-test`

Authenticated browser voice test for a saved agent in the dashboard's selected project.

- `POST` accepts `{agentId}` and returns `{appId, channel, uid, token, agentUid}`. The dashboard uses the official Next.js quickstart’s React RTC hooks for microphone and playback and its RTM toolkit for conversation events. The combined RTC and RTM token uses the returned user UID. The agent uses the next UID, wrapping to 1 if necessary.
- `PUT` accepts `{agentId, channel}` and returns `{agentId, sessionId, handle}`. The signed channel authorizes a start for the selected agent for two minutes. The agent listens only to the browser UID, with a two-minute idle timeout and a five-minute maximum runtime.
- `DELETE` accepts `{handle}` and stops the test session. The server verifies the handle against the current user and project.
