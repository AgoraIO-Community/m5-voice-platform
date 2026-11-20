# Device protocol

The M5StickS3 firmware is an RTC audio client. It fetches short-lived session
credentials from the Next.js server, joins Agora directly through RTSA Lite,
and asks the server to start or stop the cloud agent. Audio never passes
through Next.js.

## Current firmware compatibility routes

The board sends its provisioned key as an HTTP bearer token over HTTPS on each request. Set the matching server environment variable `VOICE_DEVICE_KEY`. The board uses the fixed `AGORA_APP_ID` and `AGORA_APP_CERTIFICATE` project. The Hermes dashboard maintains an ordered library of saved agents for the Stick. Its authenticated `/api/device/assignment` route supports GET, PUT to add and DELETE to remove. Both mutations accept `{"agentId":"<stored agent ID>"}` and return the updated library. Removing an agent from the library preserves its Agora configuration. An existing single-agent assignment becomes the first entry and displays its ID until added again to refresh the name.

### `GET /device_agents`

The board reads the library with its provisioned bearer key and supplies its displayed hardware ID (12 uppercase hexadecimal MAC digits) in the optional `X-Device-Id` header. The dashboard and board receive that ID as `deviceId` after the first sync. A request without the header reads the library and preserves the last reported ID. An empty library returns an empty array. Agents appear in assignment order. Dashboard additions must fit the board’s 127-byte stored ID and 16 KiB response limits.

```json
{"code":0,"msg":"success","data":{"agents":[{"id":"<stored agent ID>","name":"Kitchen helper"}],"account":"hermes"}}
```

### `GET /get_config`

The optional `channel` parameter provides a short prefix (1–16 letters, digits, underscores, or hyphens). The server appends a unique signed suffix. Use the complete returned channel for RTC and start. The device can request a positive 32-bit RTC UID. When the value is missing, zero, or negative, the server assigns a random UID. The agent UID is the next UID, wrapping to 1 when the device UID is 4294967295. Each conversation has its own channel.

```json
{
  "code": 0,
  "msg": "success",
  "data": {
    "app_id": "<agora-app-id>",
    "token": "<short-lived-rtc-token>",
    "uid": 1001,
    "channel_name": "ai-conversation-...",
    "agent_uid": 1002
  }
}
```

Although the upstream service serializes UIDs as strings, the firmware accepts
either JSON numbers or decimal strings. It rejects zero, values outside the
`uint32` range, equal user/agent UIDs, malformed JSON, non-zero `code`, and
responses larger than 16 KiB.

### `POST /startAgent`

The current firmware sends:

```json
{
  "channelName": "<channel from get_config>",
  "agentId": "<stored agent ID from device_agents>",
  "rtcUid": 1002,
  "userUid": 1001,
  "parameters": {
    "output_audio_codec": "g722"
  }
}
```

The server verifies the channel and UIDs it issued, then verifies the selected agent still belongs to the library and starts it using the v2 Conversational AI Engine join endpoint. An empty library or a removed selection returns HTTP 409. Firmware that omits `agentId` starts the first agent in the library. The channel is the idempotency key for retries. The success response is:

```json
{
  "code": 0,
  "msg": "success",
  "data": {
    "agent_id": "<signed runtime session handle>",
    "channel_name": "<channel>",
    "status": "started"
  }
}
```

`agent_id` is an opaque cleanup handle that binds the runtime agent ID to this device API. Pass it back unchanged.

### `POST /stopAgent`

Request:

```json
{ "agentId": "<signed runtime session handle>" }
```

Success response:

```json
{ "code": 0, "msg": "success" }
```

Stopping must be idempotent. The firmware releases its local RTC connection
even when the HTTP cleanup request fails and retains the runtime ID so a later
stop/start can retry cloud cleanup.

## Stick session and hardware audio

Adding an agent from the v3 dashboard stores a private snapshot of its retrieved settings alongside its Stick assignment in Vercel Blob. Adding it again refreshes the snapshot. Existing ID-only entries fetch their settings on first use. The board receives only IDs and names.

The backend starts the selected managed speech pipeline with Engine v2, passing its instructions, greeting, voice, RTC token, participant UIDs and requested output codec (G.722 by default, or PCMA). Bring-your-own-key credentials are redacted by v3 and cannot be recovered from a snapshot. A successful join returns a signed handle to the v2 runtime agent. Stop verifies the handle and calls v2 leave. Previously issued v3 handles still stop their original session. The dashboard and browser tester use v3.
