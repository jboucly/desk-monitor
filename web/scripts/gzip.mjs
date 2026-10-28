// Compresses dist/index.html into dist/index.html.gz, the file the firmware embeds.
import { readFileSync, writeFileSync } from 'node:fs';
import { gzipSync } from 'node:zlib';

const html = readFileSync('dist/index.html');
const gz = gzipSync(html, { level: 9 });
writeFileSync('dist/index.html.gz', gz);
console.log(`index.html: ${(html.length / 1024).toFixed(1)} KiB → index.html.gz: ${(gz.length / 1024).toFixed(1)} KiB`);
