# Architecture

The Next.js dashboard lists stored Agora agents for the Testing project. Adding an agent to the Stick verifies the stored definition and records its ID and display name in a private Vercel Blob. Removing a membership preserves the underlying Agora agent. Conditional writes merge concurrent edits. [Assignment handler](../src/app/api/device/assignment/route.ts)

## Device conversation

1. The Stick authenticates its HTTPS library requests with its provisioned device key. Its Settings screen shows a stable hardware identifier, a simulated account, and the speaker volume.
2. The operator chooses an installed agent on the Stick. The firmware requests short-lived RTC credentials, joins the channel with RTSA Lite, and sends the selected stored agent ID to the session-start endpoint. The server verifies membership before starting an Agora agent in that channel.
3. In agent mode the front button gates microphone transmission. Releasing it lets the user listen to the reply. The firmware keeps the running session handle for cleanup.

The operator's web login and the board's credential serve separate purposes. The current demo has one provisioned device library per Agora project. [Device protocol](device-protocol.md)
