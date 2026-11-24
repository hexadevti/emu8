// emu8 — page translations. The language follows the browser (navigator.languages) unless the user
// picked one in the Files dialog (localStorage 'emu8-lang') or the URL has ?lang=xx.
// Static labels in index.html carry data-i18n (text), data-i18n-html, data-i18n-title, data-i18n-aria.
'use strict';

const I18N = {
  pt: {
    _name: 'Português',
    'boot.loading': 'Carregando…',
    'boot.noSAB': 'Este navegador não liberou SharedArrayBuffer (cross-origin isolation). Abra o endereço com https:// no Chrome ou Safari (não dentro do WhatsApp/Instagram) e recarregue a página.',
    'boot.copying': 'Copiando sdcard local: {0}',
    'boot.fail': 'Falha ao carregar: {0}',
    'tb.menu': 'Menu (F10)', 'tb.kbd': 'Teclado', 'tb.files': 'Arquivos', 'tb.reset': 'Reset', 'tb.sound': 'Som', 'tb.fs': 'Tela cheia',
    'ms.kicker': 'Menu', 'ms.close': 'Fechar', 'ms.settings': 'Ajustes', 'ms.volume': 'Volume', 'ms.files': 'Arquivos',
    'ms.config': 'Config', 'ms.help': 'Ajuda', 'ms.snapshots': 'Snapshots', 'ms.classic': 'Clássico',
    'ms.partial': '(lista parcial)', 'ms.empty': 'Nenhum arquivo — envie pelo 📁', 'ms.back': 'Voltar', 'ms.inUse': 'em uso',
    'act.MOUNT': 'Montar', 'act.UNMOUNT': 'Desmontar', 'act.M+REBOOT': 'Montar e reiniciar', 'act.MOUNT&RUN': 'Montar e rodar',
    'act.LOAD & RUN': 'Carregar e rodar', 'act.REBOOT': 'Trocar sistema', 'act.MOUNT A:': 'Montar A:', 'act.MOUNT C:': 'Montar C:',
    'act.EJECT A:': 'Ejetar A:', 'act.EJECT C:': 'Ejetar C:', 'act.LOAD': 'Carregar', 'act.RUN': 'Rodar',
    'files.title': 'Arquivos',
    'files.hint': 'Os arquivos ficam salvos só neste navegador. As ROMs de sistema (BIOS/Kernal/etc.) vão na pasta <b>roms</b>; os jogos/discos na pasta do sistema. Depois use o menu do emulador (☰) para escolher o sistema e carregar o arquivo.',
    'files.folder': 'Pasta', 'files.upload': 'Enviar arquivos…', 'files.reboot': 'Reiniciar emulador', 'files.lang': 'Idioma',
    'files.empty': '(vazia)', 'files.delete': 'Apagar', 'files.confirmDelete': 'Apagar {0}?', 'files.deleteFail': 'Não foi possível apagar: {0}',
    'files.uploading': 'Enviando {0}…', 'files.uploadFail': 'Falha em {0}: {1}', 'files.saved': '{0} arquivo(s) salvo(s) em {1}/.',
    'confirm.reboot': 'Reiniciar o emulador?',
    'recv.start': '💻 Receber do PC', 'recv.stop': 'Parar',
    'recv.how': 'No computador, abra o endereço abaixo (ou <b>/send</b> neste site) e digite o código. Mantenha esta tela aberta.',
    'recv.connecting': 'Conectando…', 'recv.waiting': 'Aguardando o PC…', 'recv.pc': 'PC conectado — pode enviar.',
    'recv.getting': 'Recebendo {0}… {1}%', 'recv.saved': '{0} salvo em {1}/.', 'recv.closed': 'Conexão encerrada.',
    'recv.badFile': 'arquivo incompleto',
    'send.intro': 'Enviar arquivos do PC para o emu8 no celular. No celular, abra 📁 → <b>Receber do PC</b> e digite aqui o código que aparece.',
    'send.code': 'Código', 'send.connect': 'Conectar', 'send.folder': 'Pasta no celular', 'send.auto': 'automática (pela extensão)',
    'send.drop': 'Arraste os arquivos para cá', 'send.pick': 'Escolher arquivos…',
    'send.badCode': 'Código inválido (5 letras/números).', 'send.connecting': 'Conectando…', 'send.connected': 'Celular conectado',
    'send.noPhone': 'Celular não encontrado — confira o código e se a tela Receber está aberta.', 'send.closed': 'Conexão encerrada.',
    'send.phoneGone': 'o celular desconectou', 'send.queued': 'na fila', 'send.sending': 'enviando…',
    'send.tooBig': 'grande demais (máx. 64 MB)', 'send.failed': 'falhou',
  },
  en: {
    _name: 'English',
    'boot.loading': 'Loading…',
    'boot.noSAB': 'This browser did not enable SharedArrayBuffer (cross-origin isolation). Open the address with https:// in Chrome or Safari (not inside WhatsApp/Instagram) and reload the page.',
    'boot.copying': 'Copying local sdcard: {0}',
    'boot.fail': 'Failed to load: {0}',
    'tb.menu': 'Menu (F10)', 'tb.kbd': 'Keyboard', 'tb.files': 'Files', 'tb.reset': 'Reset', 'tb.sound': 'Sound', 'tb.fs': 'Full screen',
    'ms.kicker': 'Menu', 'ms.close': 'Close', 'ms.settings': 'Settings', 'ms.volume': 'Volume', 'ms.files': 'Files',
    'ms.config': 'Config', 'ms.help': 'Help', 'ms.snapshots': 'Snapshots', 'ms.classic': 'Classic',
    'ms.partial': '(partial list)', 'ms.empty': 'No files — upload with 📁', 'ms.back': 'Back', 'ms.inUse': 'in use',
    'act.MOUNT': 'Mount', 'act.UNMOUNT': 'Unmount', 'act.M+REBOOT': 'Mount & reboot', 'act.MOUNT&RUN': 'Mount & run',
    'act.LOAD & RUN': 'Load & run', 'act.REBOOT': 'Change system', 'act.MOUNT A:': 'Mount A:', 'act.MOUNT C:': 'Mount C:',
    'act.EJECT A:': 'Eject A:', 'act.EJECT C:': 'Eject C:', 'act.LOAD': 'Load', 'act.RUN': 'Run',
    'files.title': 'Files',
    'files.hint': 'Files are stored only in this browser. System ROMs (BIOS/Kernal/etc.) go in the <b>roms</b> folder; games/disks in the system\'s folder. Then use the emulator menu (☰) to pick the system and load the file.',
    'files.folder': 'Folder', 'files.upload': 'Upload files…', 'files.reboot': 'Restart emulator', 'files.lang': 'Language',
    'files.empty': '(empty)', 'files.delete': 'Delete', 'files.confirmDelete': 'Delete {0}?', 'files.deleteFail': 'Could not delete: {0}',
    'files.uploading': 'Uploading {0}…', 'files.uploadFail': 'Failed on {0}: {1}', 'files.saved': '{0} file(s) saved to {1}/.',
    'confirm.reboot': 'Restart the emulator?',
    'recv.start': '💻 Receive from PC', 'recv.stop': 'Stop',
    'recv.how': 'On the computer, open the address below (or <b>/send</b> on this site) and type the code. Keep this screen open.',
    'recv.connecting': 'Connecting…', 'recv.waiting': 'Waiting for the PC…', 'recv.pc': 'PC connected — send away.',
    'recv.getting': 'Receiving {0}… {1}%', 'recv.saved': '{0} saved to {1}/.', 'recv.closed': 'Connection closed.',
    'recv.badFile': 'incomplete file',
    'send.intro': 'Send files from this PC to emu8 on your phone. On the phone, open 📁 → <b>Receive from PC</b> and type the code it shows here.',
    'send.code': 'Code', 'send.connect': 'Connect', 'send.folder': 'Folder on the phone', 'send.auto': 'automatic (by extension)',
    'send.drop': 'Drop files here', 'send.pick': 'Choose files…',
    'send.badCode': 'Invalid code (5 letters/digits).', 'send.connecting': 'Connecting…', 'send.connected': 'Phone connected',
    'send.noPhone': 'Phone not found — check the code and that the Receive screen is open.', 'send.closed': 'Connection closed.',
    'send.phoneGone': 'the phone disconnected', 'send.queued': 'queued', 'send.sending': 'sending…',
    'send.tooBig': 'too large (max 64 MB)', 'send.failed': 'failed',
  },
  es: {
    _name: 'Español',
    'boot.loading': 'Cargando…',
    'boot.noSAB': 'Este navegador no habilitó SharedArrayBuffer (cross-origin isolation). Abra la dirección con https:// en Chrome o Safari (no dentro de WhatsApp/Instagram) y recargue la página.',
    'boot.copying': 'Copiando sdcard local: {0}',
    'boot.fail': 'Error al cargar: {0}',
    'tb.menu': 'Menú (F10)', 'tb.kbd': 'Teclado', 'tb.files': 'Archivos', 'tb.reset': 'Reset', 'tb.sound': 'Sonido', 'tb.fs': 'Pantalla completa',
    'ms.kicker': 'Menú', 'ms.close': 'Cerrar', 'ms.settings': 'Ajustes', 'ms.volume': 'Volumen', 'ms.files': 'Archivos',
    'ms.config': 'Config', 'ms.help': 'Ayuda', 'ms.snapshots': 'Snapshots', 'ms.classic': 'Clásico',
    'ms.partial': '(lista parcial)', 'ms.empty': 'Ningún archivo — súbalo con 📁', 'ms.back': 'Volver', 'ms.inUse': 'en uso',
    'act.MOUNT': 'Montar', 'act.UNMOUNT': 'Desmontar', 'act.M+REBOOT': 'Montar y reiniciar', 'act.MOUNT&RUN': 'Montar y ejecutar',
    'act.LOAD & RUN': 'Cargar y ejecutar', 'act.REBOOT': 'Cambiar sistema', 'act.MOUNT A:': 'Montar A:', 'act.MOUNT C:': 'Montar C:',
    'act.EJECT A:': 'Expulsar A:', 'act.EJECT C:': 'Expulsar C:', 'act.LOAD': 'Cargar', 'act.RUN': 'Ejecutar',
    'files.title': 'Archivos',
    'files.hint': 'Los archivos se guardan solo en este navegador. Las ROMs del sistema (BIOS/Kernal/etc.) van en la carpeta <b>roms</b>; los juegos/discos en la carpeta del sistema. Luego use el menú del emulador (☰) para elegir el sistema y cargar el archivo.',
    'files.folder': 'Carpeta', 'files.upload': 'Subir archivos…', 'files.reboot': 'Reiniciar emulador', 'files.lang': 'Idioma',
    'files.empty': '(vacía)', 'files.delete': 'Borrar', 'files.confirmDelete': '¿Borrar {0}?', 'files.deleteFail': 'No se pudo borrar: {0}',
    'files.uploading': 'Subiendo {0}…', 'files.uploadFail': 'Error en {0}: {1}', 'files.saved': '{0} archivo(s) guardado(s) en {1}/.',
    'confirm.reboot': '¿Reiniciar el emulador?',
    'recv.start': '💻 Recibir del PC', 'recv.stop': 'Detener',
    'recv.how': 'En la computadora, abra la dirección de abajo (o <b>/send</b> en este sitio) y escriba el código. Mantenga esta pantalla abierta.',
    'recv.connecting': 'Conectando…', 'recv.waiting': 'Esperando el PC…', 'recv.pc': 'PC conectado — ya puede enviar.',
    'recv.getting': 'Recibiendo {0}… {1}%', 'recv.saved': '{0} guardado en {1}/.', 'recv.closed': 'Conexión cerrada.',
    'recv.badFile': 'archivo incompleto',
    'send.intro': 'Enviar archivos de este PC a emu8 en el celular. En el celular, abra 📁 → <b>Recibir del PC</b> y escriba aquí el código que aparece.',
    'send.code': 'Código', 'send.connect': 'Conectar', 'send.folder': 'Carpeta en el celular', 'send.auto': 'automática (por extensión)',
    'send.drop': 'Arrastre los archivos aquí', 'send.pick': 'Elegir archivos…',
    'send.badCode': 'Código inválido (5 letras/números).', 'send.connecting': 'Conectando…', 'send.connected': 'Celular conectado',
    'send.noPhone': 'Celular no encontrado — revise el código y que la pantalla Recibir esté abierta.', 'send.closed': 'Conexión cerrada.',
    'send.phoneGone': 'el celular se desconectó', 'send.queued': 'en cola', 'send.sending': 'enviando…',
    'send.tooBig': 'demasiado grande (máx. 64 MB)', 'send.failed': 'falló',
  },
};

let LANG = (() => {
  const ok = (l) => l && I18N[l.toLowerCase().slice(0, 2)] ? l.toLowerCase().slice(0, 2) : null;
  let saved = null;
  try { saved = new URLSearchParams(location.search).get('lang') || localStorage.getItem('emu8-lang'); } catch (e) {}
  if (ok(saved)) return ok(saved);
  for (const l of navigator.languages || [navigator.language]) if (ok(l)) return ok(l);
  return 'en';
})();

// t('files.saved', 3, 'c64') → "3 file(s) saved to c64/."; unknown keys fall back to English, then the key
function t(key, ...args) {
  const s = (I18N[LANG] && I18N[LANG][key]) ?? I18N.en[key] ?? key;
  return s.replace(/\{(\d)\}/g, (_, i) => args[i] ?? '');
}

function applyI18n(root = document) {
  document.documentElement.lang = { pt: 'pt-BR', en: 'en', es: 'es' }[LANG] || LANG;
  root.querySelectorAll('[data-i18n]').forEach((el) => { el.textContent = t(el.dataset.i18n); });
  root.querySelectorAll('[data-i18n-html]').forEach((el) => { el.innerHTML = t(el.dataset.i18nHtml); });
  root.querySelectorAll('[data-i18n-title]').forEach((el) => { el.title = t(el.dataset.i18nTitle); });
  root.querySelectorAll('[data-i18n-aria]').forEach((el) => { el.setAttribute('aria-label', t(el.dataset.i18nAria)); });
}

function setLang(l) {
  if (!I18N[l]) return;
  LANG = l;
  try { localStorage.setItem('emu8-lang', l); } catch (e) {}
  applyI18n();
  document.dispatchEvent(new Event('emu8-lang'));      // app.js re-renders its dynamic labels
}

applyI18n();
