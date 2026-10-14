import { useEffect, useState } from 'react';
import { ImageView } from './components/ImageView.tsx';
import { MotionControls } from './components/MotionControls.tsx';
import { AlarmList, EventLog, CameraDetails, CameraList, PipelinePanel, RecordingPanel, UploadPanel } from './components/Panels.tsx';
import { PoseView } from './components/PoseView.tsx';
import { TestPanel } from './components/TestPanel.tsx';
import { staleness, useNow } from './hooks/useClock.ts';
import { useJog } from './hooks/useJog.ts';
import { useServer } from './server/useServer.ts';

const PREVIEW_FPS = 10;

export function App() {
  const link = useServer();
  const nowMs = useNow(250);
  const st = staleness(link, nowMs);
  const [selected, setSelected] = useState<string | null>(null);

  const cameras = link.state?.cameras ?? [];
  const firstCamera = cameras[0]?.cameraId ?? null;
  useEffect(() => {
    if (selected === null && firstCamera) setSelected(firstCamera);
  }, [selected, firstCamera]);

  const { watch } = link;
  useEffect(() => {
    watch(selected, PREVIEW_FPS);
  }, [selected, watch]);

  const camera = cameras.find((h) => h.cameraId === selected) ?? null;
  const jog = useJog(link.jog, selected, st.live && camera?.link === 'connected');

  return (
    <div className={`app ${st.live ? '' : 'app-stale'}`}>
      <header className="topbar">
        <h1>Pan-Tilt Viewer</h1>
        <span className="mono">{link.state?.serverId ?? 'server ?'}</span>
        <span className="spacer" />
        <span className={`pill ${st.live ? 'good' : 'bad'}`}>{st.live ? `live · ${Math.round(st.ageMs ?? 0)} ms` : 'NOT LIVE'}</span>
      </header>

      {!st.live && (
        <div className="stale-banner" role="alert">
          <b>{st.reason}.</b> The values below are the last known values, not the current state. Motion controls are disabled.
        </div>
      )}

      <main className="layout">
        <aside className="col-left">
          <CameraList cameras={cameras} selected={selected} onSelect={setSelected} />
          {link.state && <PipelinePanel pipeline={link.state.pipeline} />}
          {link.state && <UploadPanel state={link.state} serverNowUs={st.serverNowUs} />}
          {link.state && <RecordingPanel link={link} live={st.live} />}
        </aside>

        <div className="col-main">
          {link.state && <AlarmList alarms={link.state.alarms} serverNowUs={st.serverNowUs} />}
          {camera ? (
            <>
              <div className="grid-2">
                <ImageView received={link.preview} nowMs={nowMs} serverNowUs={st.serverNowUs} live={st.live} />
                <PoseView camera={camera} />
              </div>
              <MotionControls camera={camera} live={st.live} jog={jog} command={link.command} />
              <CameraDetails camera={camera} serverNowUs={st.serverNowUs} />
            </>
          ) : (
            <section className="panel">
              <p className="muted">{link.state ? 'Select a camera.' : 'Waiting for the server…'}</p>
            </section>
          )}
          <TestPanel />
          <EventLog events={link.events} serverNowUs={st.serverNowUs} />
        </div>
      </main>
    </div>
  );
}
