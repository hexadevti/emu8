// emu8 — "send from PC" relay. The phone opens a room under a short code (role=recv) and a desktop
// page (/send) joins it (role=send); this Durable Object only forwards WebSocket messages between the
// two sides. Nothing is stored: a file exists only in flight, and the room is gone when both close.
//
//   GET /api/relay/<CODE>?role=recv|send   (WebSocket upgrade)
//
// Everything else falls through to the static assets (dist/).

const CODE_RE = /^[A-HJ-NP-Z2-9]{5}$/;

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    const m = url.pathname.match(/^\/api\/relay\/([^/]+)$/);
    if (!m) return env.ASSETS.fetch(req);
    const code = m[1].toUpperCase();
    const role = url.searchParams.get('role');
    if (!CODE_RE.test(code) || (role !== 'recv' && role !== 'send')) return new Response('bad request', { status: 400 });
    if (req.headers.get('Upgrade') !== 'websocket') return new Response('websocket expected', { status: 426 });
    return env.RELAY.get(env.RELAY.idFromName(code)).fetch(req);
  },
};

export class Relay {
  constructor(ctx) { this.ctx = ctx; }

  async fetch(req) {
    const role = new URL(req.url).searchParams.get('role');
    const [client, server] = Object.values(new WebSocketPair());
    if (role === 'recv') {
      // one phone per code: a newer one (page reloaded) replaces the old
      for (const ws of this.ctx.getWebSockets('recv')) ws.close(4000, 'replaced');
      this.ctx.acceptWebSocket(server, ['recv']);
      for (const ws of this.ctx.getWebSockets('send')) safeSend(ws, '{"t":"peer","on":true}');
    } else {
      this.ctx.acceptWebSocket(server, ['send']);
      const on = this.ctx.getWebSockets('recv').length > 0;
      safeSend(server, JSON.stringify({ t: 'peer', on }));
      if (on) for (const ws of this.ctx.getWebSockets('recv')) safeSend(ws, '{"t":"peer","on":true}');
    }
    return new Response(null, { status: 101, webSocket: client });
  }

  webSocketMessage(ws, msg) {
    const other = this.ctx.getTags(ws)[0] === 'recv' ? 'send' : 'recv';
    for (const peer of this.ctx.getWebSockets(other)) safeSend(peer, msg);
  }

  webSocketClose(ws, code) {
    const role = this.ctx.getTags(ws)[0];
    const other = role === 'recv' ? 'send' : 'recv';
    const left = this.ctx.getWebSockets(role).filter((w) => w !== ws).length;
    if (!left) for (const peer of this.ctx.getWebSockets(other)) safeSend(peer, '{"t":"peer","on":false}');
    try { ws.close(code === 1005 ? 1000 : code); } catch (e) {}
  }

  webSocketError(ws) { this.webSocketClose(ws, 1011); }
}

function safeSend(ws, msg) { try { ws.send(msg); } catch (e) {} }
