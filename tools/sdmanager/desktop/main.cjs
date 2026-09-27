// main.cjs - emu8 SD Manager desktop app: the web app (../web) in an Electron window.
//
// The page is served from a privileged app:// scheme (a secure context, so Web Serial is allowed)
// out of web/, which `npm run web` copies from ../web/dist. Electron has no built-in serial port
// chooser, so navigator.serial.requestPort() is answered here with a native dialog.
const { app, BrowserWindow, dialog, net, protocol, session, shell } = require('electron');
const path = require('node:path');
const { pathToFileURL } = require('node:url');

const WEB_ROOT = path.join(__dirname, 'web');
const ORIGIN = 'app://sdmanager';

protocol.registerSchemesAsPrivileged([
  { scheme: 'app', privileges: { standard: true, secure: true, supportFetchAPI: true } },
]);

function serveWeb(req) {
  const rel = decodeURIComponent(new URL(req.url).pathname).replace(/^\/+/, '') || 'index.html';
  const file = path.join(WEB_ROOT, rel);
  if (file !== WEB_ROOT && !file.startsWith(WEB_ROOT + path.sep)) return new Response('Forbidden', { status: 403 });
  return net.fetch(pathToFileURL(file).toString());
}

// "USB Serial Device (COM5)" on Windows already names the port; elsewhere add it.
function portLabel(p) {
  const name = p.displayName || 'Serial port';
  return name.includes(p.portName) ? name : `${name} (${p.portName})`;
}

async function pickPort(win, ports) {
  if (!ports.length) {
    await dialog.showMessageBox(win, {
      type: 'warning', title: 'emu8 SD Manager', message: 'No serial ports found',
      detail: 'Plug the board in over USB, make sure it is running, and click Connect again.',
    });
    return '';
  }
  const { response } = await dialog.showMessageBox(win, {
    type: 'question', title: 'emu8 SD Manager', message: 'Pick the board\'s serial port',
    detail: 'CYD / JC4827W543: USB-SERIAL CH340 or CP210x. PicoCalc or P4 in CDC mode: USB Serial Device.',
    buttons: [...ports.map(portLabel), 'Cancel'],
    cancelId: ports.length, noLink: true,
  });
  return ports[response]?.portId ?? '';
}

function createWindow() {
  const win = new BrowserWindow({
    width: 1200, height: 800, minWidth: 720, minHeight: 480,
    backgroundColor: '#101418',
    title: 'emu8 SD Manager',
    icon: path.join(__dirname, 'build', 'icon.png'),
    autoHideMenuBar: true,
    webPreferences: { contextIsolation: true, sandbox: true },
  });
  win.removeMenu();   // keeps F5 / Backspace etc. for the app's own shortcuts

  win.webContents.session.on('select-serial-port', (event, ports, _wc, callback) => {
    event.preventDefault();
    pickPort(win, ports).then(callback, () => callback(''));
  });

  // The page blocks unload while a transfer is running; Chrome asks, Electron would just refuse.
  win.webContents.on('will-prevent-unload', event => {
    const choice = dialog.showMessageBoxSync(win, {
      type: 'warning', title: 'emu8 SD Manager', message: 'A transfer is still running',
      detail: 'Closing now aborts it. An interrupted upload leaves only a .part file on the card.',
      buttons: ['Close anyway', 'Keep running'], defaultId: 1, cancelId: 1, noLink: true,
    });
    if (choice === 0) event.preventDefault();
  });

  win.webContents.setWindowOpenHandler(({ url }) => {
    if (/^https?:/.test(url)) shell.openExternal(url);
    return { action: 'deny' };
  });
  win.webContents.on('will-navigate', (event, url) => {
    if (!url.startsWith(ORIGIN)) { event.preventDefault(); if (/^https?:/.test(url)) shell.openExternal(url); }
  });

  win.webContents.on('before-input-event', (_e, input) => {
    if (input.type === 'keyDown' && (input.key === 'F12' || (input.control && input.shift && input.key.toLowerCase() === 'i')))
      win.webContents.toggleDevTools();
  });

  win.loadURL(`${ORIGIN}/index.html`);
}

app.whenReady().then(() => {
  protocol.handle('app', serveWeb);
  const ses = session.defaultSession;
  ses.setPermissionCheckHandler((_wc, permission) => permission === 'serial' || permission === 'clipboard-sanitized-write');
  ses.setDevicePermissionHandler(details => details.deviceType === 'serial');
  createWindow();
});

app.on('window-all-closed', () => app.quit());
