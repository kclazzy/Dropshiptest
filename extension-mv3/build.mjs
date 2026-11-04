// Сборка без фреймворков: три точки входа + статические файлы -> dist/.
import { build } from 'esbuild';
import { cpSync, mkdirSync, rmSync } from 'node:fs';

rmSync('dist', { recursive: true, force: true });
mkdirSync('dist', { recursive: true });

const common = { bundle: true, target: 'chrome116', sourcemap: false, logLevel: 'info' };
await build({ ...common, entryPoints: { background: 'src/background/service-worker.ts' }, outdir: 'dist', format: 'esm' });
await build({ ...common, entryPoints: { popup: 'src/popup/popup.ts' }, outdir: 'dist', format: 'esm' });
// Скрипт внедряется по клику в изолированный мир страницы, поэтому — IIFE без импортов.
await build({ ...common, entryPoints: { extractor: 'src/content/extractor.ts' }, outdir: 'dist', format: 'iife' });

cpSync('manifest.json', 'dist/manifest.json');
cpSync('src/popup/popup.html', 'dist/popup.html');
