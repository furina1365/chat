// 只在更新 Markdown 依赖时执行；生成的 vendor 文件随项目提交。
const fs = require('node:fs');
const path = require('node:path');
const esbuild = require('esbuild');

async function build() {
  const root = path.resolve(__dirname, '..');
  const vendor = path.join(root, 'vendor');
  const katexDist = path.join(root, 'node_modules/katex/dist');
  fs.mkdirSync(path.join(vendor, 'fonts'), { recursive: true });
  await esbuild.build({
    absWorkingDir: root,
    entryPoints: ['src/markdown.js'],
    outfile: 'vendor/markdown.js',
    bundle: true,
    format: 'iife',
    globalName: 'ChatMarkdown',
    target: ['es2020'],
    minify: true,
    legalComments: 'inline',
  });
  fs.rmSync(path.join(vendor, 'markdown.js.LEGAL.txt'), { force: true });
  // 现代浏览器使用 WOFF2；不复制重复的 WOFF/TTF 字体。
  const css = fs.readFileSync(path.join(katexDist, 'katex.min.css'), 'utf8')
    .replace(/,url\([^)]*\.(?:woff|ttf)\) format\("[^"]*"\)/g, '');
  fs.writeFileSync(path.join(vendor, 'katex.css'), css);
  for (const filename of fs.readdirSync(path.join(katexDist, 'fonts'))) {
    if (filename.endsWith('.woff2')) fs.copyFileSync(path.join(katexDist, 'fonts', filename), path.join(vendor, 'fonts', filename));
  }
  // 保存运行时依赖的版本与许可证，便于复核本地资源的来源。
  const lock = JSON.parse(fs.readFileSync(path.join(root, 'package-lock.json'), 'utf8'));
  const notices = [];
  for (const [name, info] of Object.entries(lock.packages)) {
    if (!name || info.dev) continue;
    const folder = path.join(root, name);
    const license = fs.readdirSync(folder).find((file) => /^licen[sc]e(?:[.-].*)?$/i.test(file));
    notices.push(`${name} ${info.version}\n${info.resolved}\n${license ? fs.readFileSync(path.join(folder, license), 'utf8') : `License: ${info.license}`}\n`);
  }
  fs.writeFileSync(path.join(vendor, 'THIRD-PARTY-NOTICES.txt'), notices.join('\n--------------------\n\n'));
}

build().catch((error) => { console.error(error); process.exitCode = 1; });
