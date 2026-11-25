declare module 'markdown-it-task-lists' {
  import type { MarkdownIt } from 'markdown-it';
  const taskLists: (markdown: MarkdownIt) => void;
  export default taskLists;
}
