import {mkdir, copyFile} from 'node:fs/promises';
const files = [
  ['@xterm/xterm/lib/xterm.js', 'xterm.js'],
  ['@xterm/xterm/css/xterm.css', 'xterm.css'],
  ['@xterm/xterm/LICENSE', 'xterm-LICENSE'],
  ['@xterm/addon-fit/lib/addon-fit.js', 'addon-fit.js'],
  ['@xterm/addon-fit/LICENSE', 'addon-fit-LICENSE'],
];
await mkdir('web/vendor', {recursive:true});
for (const [from, to] of files) await copyFile(`node_modules/${from}`, `web/vendor/${to}`);
