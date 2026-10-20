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
  const scheme = url.match(/^([a-z][a-z\d+.-]*):/i)?.[1].toLowerCase();
  return validateLink(url) && (!scheme || ['http', 'https', 'mailto'].includes(scheme));
};

export function renderInto(parent, source) {
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
  parent.replaceChildren(fragment);
}
