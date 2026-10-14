// The one place where the viewer meets your server.
//
// Open the page with ?demo to see it run on a fake server (demo.ts). Without
// ?demo it uses useYourServer() below, which you write: connect to your
// server, keep `state`, `events` and `preview` up to date, and carry out the
// actions. The rest of the viewer only uses the ServerLink interface.

import { useDemoServer } from './demo.ts';
import { DISCONNECTED, type ServerLink } from './link.ts';

const demo = typeof window !== 'undefined' && new URLSearchParams(window.location.search).has('demo');

export function useServer(): ServerLink {
  const fake = useDemoServer(demo);
  const yours = useYourServer(!demo);
  return demo ? fake : yours;
}

// TODO: connect the viewer to your server.
function useYourServer(enabled: boolean): ServerLink {
  void enabled;
  return {
    ...DISCONNECTED,
    problem: 'not connected to a server: write useYourServer() in viewer/src/server/useServer.ts, or open the page with ?demo',
  };
}
