import MarkdownIt from 'markdown-it';
import { tex } from '@mdit/plugin-tex';
import katex from 'katex';
import DOMPurify from 'dompurify';
import highlight from 'highlight.js/lib/common';
import taskLists from 'markdown-it-task-lists';

const markdown = new MarkdownIt({
  html: false,
  linkify: true,
  breaks: true,
  highlight(code, language) {
    if (!language || !highlight.getLanguage(language)) return '';
    return highlight.highlight(code, { language, ignoreIllegals: true }).value;
  },
}).use(taskLists).use(tex, {
  delimiters: 'all',
  mathFence: true,
  render(source, displayMode) {
    return katex.renderToString(source, {
      displayMode,
      throwOnError: false,
      trust: false,
      strict: 'ignore',
      maxSize: 20,
      maxExpand: 1000,
      // 每个公式独立，模型定义的宏不会污染其他公式或其他会话。
      macros: {},
    });
  },
});

// 图片只接受普通网页地址或相对路径，不接受 data/file 等来源。
const validateLink = markdown.validateLink.bind(markdown);
markdown.validateLink = (url) => {
  const scheme = url.match(/^([a-z][a-z\d+.-]*):/i)?.[1]?.toLowerCase();
  return validateLink(url) && (!scheme || ['http', 'https', 'mailto'].includes(scheme));
};

function svgImage(source: string): HTMLImageElement | undefined {
  const parsed = new DOMParser().parseFromString(source, 'image/svg+xml');
  if (parsed.querySelector('parsererror') || parsed.documentElement.localName !== 'svg') return;
  const clean = DOMPurify.sanitize(source, {
    USE_PROFILES: { svg: true, svgFilters: true },
    FORBID_TAGS: ['foreignObject'],
  });
  const svg = new DOMParser().parseFromString(clean, 'image/svg+xml').documentElement;
  if (svg.localName !== 'svg') return;
  svg.setAttribute('xmlns', 'http://www.w3.org/2000/svg');
  const image = document.createElement('img');
  image.className = 'svg-preview';
  image.alt = parsed.querySelector('title')?.textContent || 'SVG 图形';
  // 使用图片上下文，SVG 内的样式和元素 ID 不会影响聊天页面。
  image.src = `data:image/svg+xml;charset=utf-8,${encodeURIComponent(new XMLSerializer().serializeToString(svg))}`;
  return image;
}

function enhanceCodeBlocks(fragment: DocumentFragment) {
  for (const code of fragment.querySelectorAll<HTMLElement>('pre > code')) {
    const language = [...code.classList].find((name) => name.startsWith('language-'))?.slice(9).toLowerCase() || '';
    const source = code.textContent || '';
    const pre = code.parentElement!;
    if (language === 'svg' || (['xml', ''].includes(language) && /^\s*(?:<\?xml[^?]*\?>\s*)?<svg[\s>]/i.test(source))) {
      const image = svgImage(source);
      if (!image) continue;
      const figure = document.createElement('figure');
      figure.className = 'svg-artifact';
      const details = document.createElement('details');
      const summary = document.createElement('summary');
      summary.textContent = '查看 SVG 源码';
      details.append(summary);
      pre.replaceWith(figure);
      details.append(pre);
      figure.append(image, details);
    } else if (['html', 'htm'].includes(language)) {
      const panel = document.createElement('section');
      panel.className = 'html-artifact';
      const toolbar = document.createElement('div');
      toolbar.className = 'artifact-toolbar';
      const label = document.createElement('span');
      label.textContent = 'HTML';
      const toggle = document.createElement('button');
      toggle.type = 'button';
      toggle.textContent = '预览';
      toggle.setAttribute('aria-expanded', 'false');
      let frame: HTMLIFrameElement | undefined;
      toggle.addEventListener('click', () => {
        if (!frame) {
          frame = document.createElement('iframe');
          frame.className = 'html-preview';
          frame.title = 'HTML 预览';
          frame.setAttribute('sandbox', 'allow-scripts');
          frame.referrerPolicy = 'no-referrer';
          // 独立来源运行内联脚本，禁止访问聊天数据和加载外部资源。
          frame.srcdoc = `<!doctype html><meta charset="utf-8"><meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src data: blob:; font-src data:; media-src data: blob:; base-uri 'none'; form-action 'none'"><meta name="viewport" content="width=device-width, initial-scale=1">${source}`;
          panel.append(frame);
        }
        const previewing = !pre.hidden;
        pre.hidden = previewing;
        frame.hidden = !previewing;
        toggle.textContent = previewing ? '查看源码' : '预览';
        toggle.setAttribute('aria-expanded', String(previewing));
      });
      toolbar.append(label, toggle);
      pre.replaceWith(panel);
      panel.append(toolbar, pre);
    }
  }
}

export function renderInto(parent: HTMLElement, source: string) {
  const fragment = DOMPurify.sanitize(markdown.render(source), {
    RETURN_DOM_FRAGMENT: true,
    USE_PROFILES: { html: true, mathMl: true, svg: true },
    FORBID_TAGS: ['style'],
  });
  for (const link of fragment.querySelectorAll('a')) {
    link.target = '_blank';
    link.rel = 'noopener noreferrer';
  }
  for (const image of fragment.querySelectorAll('img')) {
    image.loading = 'lazy';
    image.decoding = 'async';
    image.referrerPolicy = 'no-referrer';
  }
  // 表格在窄屏上局部滚动，保留 table 本身的语义。
  for (const table of fragment.querySelectorAll('table')) {
    const scroll = document.createElement('div');
    scroll.className = 'table-scroll';
    scroll.tabIndex = 0;
    scroll.setAttribute('role', 'region');
    scroll.setAttribute('aria-label', '表格，可横向滚动');
    table.replaceWith(scroll);
    scroll.append(table);
  }
  enhanceCodeBlocks(fragment);
  parent.replaceChildren(fragment);
}
