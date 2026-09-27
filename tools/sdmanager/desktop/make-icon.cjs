// make-icon.cjs - renders ../web/public/icon.svg to build/icon.png (256x256) for the .exe icon.
// Run under Electron (`npm run icon`), whose Chromium does the SVG rasterising: the SVG is drawn
// onto a canvas in a hidden window and read back as PNG.
const { app, BrowserWindow } = require('electron');
const fs = require('node:fs');
const path = require('node:path');

const SIZE = 256;
const svg = fs.readFileSync(path.join(__dirname, '..', 'web', 'public', 'icon.svg'));
const src = `data:image/svg+xml;base64,${svg.toString('base64')}`;

setTimeout(() => { console.error('make-icon: timed out'); app.exit(1); }, 30_000);

app.whenReady().then(async () => {
  const win = new BrowserWindow({ show: false });
  await win.loadURL('about:blank');
  const dataUrl = await win.webContents.executeJavaScript(`new Promise((resolve, reject) => {
    const img = new Image();
    img.onload = () => {
      const c = document.createElement('canvas');
      c.width = c.height = ${SIZE};
      c.getContext('2d').drawImage(img, 0, 0, ${SIZE}, ${SIZE});
      resolve(c.toDataURL('image/png'));
    };
    img.onerror = () => reject(new Error('cannot load icon.svg'));
    img.src = ${JSON.stringify(src)};
  })`);
  fs.mkdirSync(path.join(__dirname, 'build'), { recursive: true });
  fs.writeFileSync(path.join(__dirname, 'build', 'icon.png'), Buffer.from(dataUrl.split(',')[1], 'base64'));
  app.exit(0);
}).catch(e => { console.error(e); app.exit(1); });
