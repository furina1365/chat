const host = window.location.hostname || 'localhost';
const apiBase = location.protocol !== 'file:' && location.port !== '8000'
  ? location.origin : `http://${host.includes(':') ? `[${host.replace(/^\[|\]$/g, '')}]` : host}:8080`;
const storageKey = 'deepseek.conversations.v1';
const $ = (selector) => document.querySelector(selector);
const shell = $('.app-shell');
const workspace = $('#workspace');
const list = $('#conversation-list');
const messagesElement = $('#messages');
const prompt = $('#prompt');
const sendButton = $('#send');
const status = $('#status');
const retryButton = $('#retry');
const modelSelect = $('#model-select');
const modelMenu = $('#model-menu');
const stopButton = $('#stop');
let models = [];
let modelError = '';
let defaultModel = null;
let streamFrame = null;
const scrollArea = $('#conversation-scroll');
const pending = new Map();
const failures = new Map();
const notices = new Map();
const drafts = new Map();
const messageContentCache = new WeakMap();
const reasoningViews = new WeakMap();
let conversations = [];
let activeId = null;
let storageWarning = '';
let formError = '';

function icon(name) {
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.setAttribute('aria-hidden', 'true');
  const use = document.createElementNS('http://www.w3.org/2000/svg', 'use');
  use.setAttribute('href', `#icon-${name}`);
  svg.append(use);
  return svg;
}

function load() {
  try {
    const saved = JSON.parse(localStorage.getItem(storageKey) || 'null');
    if (!saved || !Array.isArray(saved.conversations)) return;
    conversations = saved.conversations.filter((conversation) =>
      conversation && typeof conversation.id === 'string' &&
      typeof conversation.title === 'string' && Array.isArray(conversation.messages) &&
      conversation.messages.length > 0 && conversation.messages.every((message, index) =>
        message && message.role === (index % 2 === 0 ? 'user' : 'assistant') &&
        typeof message.content === 'string' &&
        (message.reasoning === undefined || typeof message.reasoning === 'string') &&
        (message.content.trim().length > 0 || (message.role === 'assistant' &&
          message.interrupted === true && message.reasoning?.trim().length > 0))));
    // 本地记录损坏时不让重复 id 指向同一个会话。
    conversations = conversations.filter((item, index, items) =>
      items.findIndex((other) => other.id === item.id) === index);
    activeId = conversations.some((item) => item.id === saved.activeId) ? saved.activeId : null;
  } catch {
    storageWarning = '无法读取本地历史；当前对话仍可使用。';
  }
}

function save() {
  try {
    localStorage.setItem(storageKey, JSON.stringify({ conversations, activeId }));
    storageWarning = '';
  } catch {
    storageWarning = '本地历史保存失败，刷新后可能丢失。请清理浏览器存储空间。';
  }
}

function currentConversation() {
  return conversations.find((item) => item.id === activeId);
}

function makeId() {
  const bytes = new Uint32Array(2);
  crypto.getRandomValues(bytes);
  return `${Date.now().toString(36)}-${bytes[0].toString(36)}${bytes[1].toString(36)}`;
}

function closeSidebar() {
  shell.classList.remove('sidebar-open');
  $('#sidebar-overlay').hidden = true;
  $('#sidebar').inert = window.innerWidth <= 720;
  $('#open-sidebar').setAttribute('aria-expanded', String(!shell.classList.contains('sidebar-collapsed') && window.innerWidth > 720));
}

function switchConversation(id) {
  drafts.set(activeId || 'new', prompt.value);
  activeId = id;
  formError = '';
  prompt.value = drafts.get(id || 'new') || '';
  save();
  closeSidebar();
  render();
  resizePrompt();
  scrollToEnd();
  if (!prompt.disabled) prompt.focus();
}

function newConversation() {
  drafts.delete('new');
  if (activeId === null) prompt.value = '';
  switchConversation(null);
}

function renderList() {
  list.replaceChildren();
  for (const conversation of conversations) {
    const row = document.createElement('div');
    row.className = `conversation-item${conversation.id === activeId ? ' active' : ''}`;
    const link = document.createElement('button');
    link.className = 'conversation-link';
    link.textContent = conversation.title;
    link.title = conversation.title;
    if (conversation.id === activeId) link.setAttribute('aria-current', 'page');
    link.addEventListener('click', () => switchConversation(conversation.id));
    const remove = document.createElement('button');
    remove.className = 'icon-button delete-conversation';
    remove.setAttribute('aria-label', `删除对话：${conversation.title}`);
    remove.title = '删除对话';
    remove.append(icon('trash'));
    remove.addEventListener('click', () => {
      pending.get(conversation.id)?.controller.abort();
      pending.delete(conversation.id);
      failures.delete(conversation.id);
      notices.delete(conversation.id);
      drafts.delete(conversation.id);
      conversations = conversations.filter((item) => item.id !== conversation.id);
      if (activeId === conversation.id) {
        activeId = null;
        prompt.value = drafts.get('new') || '';
        formError = '';
      }
      save();
      render();
      resizePrompt();
    });
    row.append(link, remove);
    list.append(row);
  }
  $('#history-empty').hidden = conversations.length > 0;
}

function renderContent(parent, text) {
  if (window.ChatMarkdown) ChatMarkdown.renderInto(parent, text);
  else parent.textContent = text;
}

function reasoningBlock(owner, text, state) {
  let view = reasoningViews.get(owner);
  if (!view) {
    const details = document.createElement('details');
    details.className = 'thinking';
    const summary = document.createElement('summary');
    const title = document.createElement('span');
    title.className = 'thinking-title';
    summary.append(title, icon('chevron-down'));
    const content = document.createElement('div');
    content.className = 'message-content thinking-content';
    details.append(summary, content);
    view = { details, title, content, text: '' };
    reasoningViews.set(owner, view);
  }
  view.details.dataset.state = state;
  view.title.textContent = state === 'thinking' ? '正在思考…' : state === 'interrupted' ? '思考已中断' : '思考过程';
  if (view.text !== text) { renderContent(view.content, text); view.text = text; }
  return view.details;
}

function modelHistory(messages) {
  const history = [];
  for (const message of messages) {
    // 只收到思考就中断的这一轮不作为已完成对话传给模型；重新生成仍使用原问题。
    if (message.role === 'assistant' && !message.content.trim()) {
      history.pop();
      continue;
    }
    history.push({ role: message.role, content: message.content });
  }
  return history;
}

function renderMessages() {
  messagesElement.replaceChildren();
  const conversation = currentConversation();
  if (!conversation) return;
  for (const message of conversation.messages) {
    const flight = pending.get(conversation.id);
    if (flight?.regenerate && (flight.text || flight.reasoning) && message === conversation.messages.at(-1)) continue;
    const article = document.createElement('article');
    article.className = `message ${message.role}`;
    article.setAttribute('aria-label', message.role === 'user' ? '你' : 'AI 的回复');
    if (message.role === 'user') article.textContent = message.content;
    else {
      let content = messageContentCache.get(message);
      if (!content) {
        content = document.createElement('div');
        content.className = 'message-content';
        renderContent(content, message.content);
        // 流式更新只解析正在生成的回复，历史消息的公式与高亮复用已有节点。
        messageContentCache.set(message, content);
      }
      const actions = document.createElement('div');
      actions.className = 'message-actions';
      const copy = document.createElement('button');
      copy.className = 'icon-button';
      copy.title = '复制回复';
      copy.setAttribute('aria-label', '复制回复');
      copy.append(icon('copy'));
      copy.hidden = !message.content.trim();
      copy.addEventListener('click', async () => {
        try {
          if (navigator.clipboard?.writeText) await navigator.clipboard.writeText(message.content);
          else {
            const field = document.createElement('textarea');
            field.value = message.content;
            field.style.position = 'fixed';
            field.style.opacity = '0';
            document.body.append(field);
            field.select();
            const copied = document.execCommand('copy');
            field.remove();
            if (!copied) throw new Error('copy failed');
          }
          copy.title = '已复制';
          copy.setAttribute('aria-label', '已复制');
        } catch {
          copy.title = '复制失败，请选择文字复制';
          copy.setAttribute('aria-label', copy.title);
        }
      });
      actions.append(copy);
      if (message === conversation.messages.at(-1)) {
        const regenerate = document.createElement('button');
        regenerate.className = 'regenerate-button';
        regenerate.title = '重新生成这条回复';
        regenerate.setAttribute('aria-label', '重试回复');
        regenerate.disabled = pending.has(conversation.id);
        regenerate.append(icon('retry'));
        regenerate.addEventListener('click', () => requestReply(conversation, { regenerate: true }));
        actions.append(regenerate);
      }
      if (message.reasoning) article.append(reasoningBlock(message, message.reasoning, message.interrupted && !message.content ? 'interrupted' : 'done'));
      if (message.content) article.append(content);
      if (message.truncated || message.interrupted) {
        const note = document.createElement('p');
        note.className = 'truncated-note';
        note.textContent = message.interrupted ? '生成已中断，以上是已收到的内容。' : '回复达到长度限制，可以发送“请继续”。';
        article.append(note);
      }
      article.append(actions);
    }
    messagesElement.append(article);
  }
  const flight = pending.get(activeId);
  if (flight?.text || flight?.reasoning) {
    const article = document.createElement('article');
    article.className = 'message assistant streaming';
    article.setAttribute('aria-label', 'AI 正在回复');
    const content = document.createElement('div');
    content.className = 'message-content';
    renderContent(content, flight.text);
    if (flight.reasoning) article.append(reasoningBlock(flight, flight.reasoning, flight.text ? 'done' : 'thinking'));
    if (flight.text) article.append(content);
    messagesElement.append(article);
  } else if (flight) {
    const waiting = document.createElement('div');
    waiting.className = 'pending';
    const dot = document.createElement('span');
    dot.className = 'pending-dot';
    waiting.append(dot, document.createTextNode('正在等待回复…'));
    messagesElement.append(waiting);
  }
}

function updateComposer() {
  const conversation = currentConversation();
  const waiting = pending.has(activeId);
  const thinking = waiting && pending.get(activeId).reasoning && !pending.get(activeId).text;
  const unanswered = conversation?.messages.at(-1)?.role === 'user';
  prompt.disabled = waiting || Boolean(unanswered);
  prompt.placeholder = thinking ? '正在思考…' : waiting ? '正在等待回复…' : unanswered ? '重试上一条消息，或新建对话' : '有什么我可以帮你？';
  const chosen = selectedModel();
  sendButton.hidden = waiting;
  stopButton.hidden = !waiting;
  stopButton.disabled = pending.get(activeId)?.stopped === true;
  sendButton.disabled = prompt.disabled || !prompt.value.trim() || !chosen;
  modelSelect.disabled = waiting || !models.length;
  if (modelSelect.disabled) closeModelMenu();
  messagesElement.setAttribute('aria-busy', String(waiting));
  retryButton.hidden = waiting || (!unanswered && !failures.has(activeId));
  const error = formError || failures.get(activeId) || modelError;
  status.classList.toggle('error', Boolean(error));
  status.textContent = error || notices.get(activeId) || storageWarning || (thinking ? '正在思考，可以随时停止。' : waiting ? '正在生成，可以随时停止。' : '') || (unanswered && !waiting ? '上一条消息还未收到回复，可以重试。' : '');
}

function render() {
  const conversation = currentConversation();
  workspace.classList.toggle('empty', !conversation);
  $('#conversation-title').textContent = conversation?.title || '新对话';
  document.title = `Chat · ${conversation?.title || '新对话'}`;
  syncModelSelect();
  renderList();
  renderMessages();
  updateComposer();
}

function resizePrompt() {
  prompt.style.height = 'auto';
  prompt.style.height = `${Math.min(prompt.scrollHeight, 220)}px`;
}

function scrollToEnd() {
  requestAnimationFrame(() => { scrollArea.scrollTop = scrollArea.scrollHeight; });
}

function modelKey(model) { return JSON.stringify([model.provider, model.model]); }
function selectedModel() {
  const conversation = currentConversation();
  const choice = conversation?.provider ? conversation : defaultModel;
  return models.find((model) => choice && model.provider === choice.provider && model.model === choice.model) || null;
}
function syncModelSelect() {
  closeModelMenu();
  const chosen = selectedModel();
  const label = chosen ? `${chosen.provider} / ${chosen.model}` :
    models.length ? '模型不可用' : modelError ? '模型加载失败' : '正在加载模型…';
  $('#model-label').textContent = label;
  modelSelect.title = label;
  modelSelect.setAttribute('aria-label', `选择模型：${label}`);
  for (const option of modelMenu.children) {
    option.setAttribute('aria-selected', String(Boolean(chosen) && option.dataset.key === modelKey(chosen)));
  }
}

function closeModelMenu() {
  modelMenu.hidePopover();
  modelSelect.setAttribute('aria-expanded', 'false');
}

function openModelMenu() {
  if (modelSelect.disabled) return;
  modelMenu.showPopover({ source: modelSelect });
  modelSelect.setAttribute('aria-expanded', 'true');
  const trigger = modelSelect.getBoundingClientRect();
  const gap = 8;
  const above = trigger.top - gap - 12;
  const below = window.innerHeight - trigger.bottom - gap - 12;
  const openAbove = above >= Math.min(modelMenu.scrollHeight, 360) || above > below;
  modelMenu.style.maxHeight = `${Math.max(0, Math.min(360, openAbove ? above : below))}px`;
  const menu = modelMenu.getBoundingClientRect();
  modelMenu.style.left = `${Math.max(12, Math.min(trigger.right - menu.width, window.innerWidth - menu.width - 12))}px`;
  modelMenu.style.top = `${openAbove ? trigger.top - gap - menu.height : trigger.bottom + gap}px`;
  (modelMenu.querySelector('[aria-selected="true"]') || modelMenu.firstElementChild)?.focus();
}

function renderModelOptions() {
  modelMenu.replaceChildren();
  for (const model of models) {
    const option = document.createElement('button');
    option.type = 'button';
    option.className = 'model-option';
    option.setAttribute('role', 'option');
    option.setAttribute('aria-label', `${model.provider} / ${model.model}`);
    option.title = `${model.provider} / ${model.model}`;
    option.tabIndex = -1;
    option.dataset.key = modelKey(model);
    const name = document.createElement('span');
    name.className = 'model-option-name';
    name.textContent = model.model;
    option.append(name, icon('check'));
    option.addEventListener('click', () => {
      const conversation = currentConversation();
      if (conversation) { conversation.provider = model.provider; conversation.model = model.model; save(); }
      else defaultModel = model;
      syncModelSelect();
      updateComposer();
      modelSelect.focus();
    });
    modelMenu.append(option);
  }
}
async function loadModels() {
  modelError = '';
  try {
    const response = await fetch(`${apiBase}/api/models`);
    if (!response.ok) throw new Error('模型列表加载失败。');
    const data = await response.json();
    if (!Array.isArray(data.models) || !data.models.length || !data.models.every((item) =>
      typeof item.provider === 'string' && typeof item.model === 'string')) throw new Error('没有可用模型，请检查后端配置。');
    models = data.models;
    if (!defaultModel || !models.some((model) => modelKey(model) === modelKey(defaultModel))) defaultModel = models[0];
    renderModelOptions();
  } catch (error) {
    modelError = error instanceof TypeError ? '无法连接后端，请确认服务已启动。' : error.message;
  }
  $('#reload-models').hidden = !modelError;
  render();
}
modelSelect.addEventListener('click', (event) => {
  event.preventDefault();
  if (modelMenu.matches(':popover-open')) closeModelMenu();
  else openModelMenu();
});
modelSelect.addEventListener('keydown', (event) => {
  if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
    event.preventDefault();
    openModelMenu();
  }
});
modelMenu.addEventListener('toggle', (event) => {
  modelSelect.setAttribute('aria-expanded', String(event.newState === 'open'));
});
modelMenu.addEventListener('keydown', (event) => {
  const options = [...modelMenu.children];
  const index = options.indexOf(document.activeElement);
  let next;
  if (event.key === 'ArrowDown') next = (index + 1) % options.length;
  else if (event.key === 'ArrowUp') next = (index - 1 + options.length) % options.length;
  else if (event.key === 'Home') next = 0;
  else if (event.key === 'End') next = options.length - 1;
  else if (event.key === 'Escape' || event.key === 'Tab') {
    closeModelMenu();
    modelSelect.focus();
  }
  if (next !== undefined) {
    event.preventDefault();
    options[next].focus();
  }
});
$('#reload-models').addEventListener('click', loadModels);
stopButton.addEventListener('click', () => {
  const flight = pending.get(activeId);
  if (flight) { flight.stopped = true; flight.controller.abort(); updateComposer(); }
});

// POST fetch 的响应体按 SSE 行解析；网络读取块不等于完整事件。
async function readEvents(response, onEvent) {
  if (!response.headers.get('content-type')?.includes('text/event-stream') || !response.body)
    throw new Error('后端没有返回流式回复。');
  const reader = response.body.getReader();
  const decoder = new TextDecoder('utf-8', { fatal: true });
  let buffer = '', event = '', data = [], eventSize = 0;
  function line(text) {
    if (!text) {
      if (data.length) {
        const stop = onEvent(event || 'message', JSON.parse(data.join('\n')));
        event = ''; data = []; eventSize = 0;
        return stop;
      }
      event = ''; return false;
    }
    if (text.startsWith(':')) return false;
    const colon = text.indexOf(':');
    const field = colon < 0 ? text : text.slice(0, colon);
    let value = colon < 0 ? '' : text.slice(colon + 1);
    if (value.startsWith(' ')) value = value.slice(1);
    if (field === 'event') event = value;
    if (field === 'data') { data.push(value); eventSize += value.length + 1; }
    if (eventSize > 1024 * 1024) throw new Error('回复事件过大，已停止接收。');
    return false;
  }
  try {
    for (; ;) {
      const { value, done } = await reader.read();
      buffer += decoder.decode(value, { stream: !done });
      let match;
      while ((match = /\r\n|\r|\n/.exec(buffer))) {
        if (!done && match[0] === '\r' && match.index === buffer.length - 1) break;
        const text = buffer.slice(0, match.index);
        buffer = buffer.slice(match.index + match[0].length);
        if (line(text)) return;
      }
      if (buffer.length + eventSize > 1024 * 1024) throw new Error('回复事件过大，已停止接收。');
      if (done) throw new Error('连接意外中断，回复未完整生成。');
    }
  } finally { await reader.cancel().catch(() => { }); reader.releaseLock(); }
}
function paintStream(id) {
  if (activeId !== id || streamFrame !== null) return;
  streamFrame = requestAnimationFrame(() => {
    streamFrame = null;
    const follow = scrollArea.scrollHeight - scrollArea.scrollTop - scrollArea.clientHeight < 120;
    renderMessages(); updateComposer();
    if (follow) scrollArea.scrollTop = scrollArea.scrollHeight;
  });
}
async function requestReply(conversation, { regenerate = false } = {}) {
  const id = conversation.id;
  if (pending.has(id)) return;
  const chosen = selectedModel();
  if (!chosen) { formError = '当前模型不可用，请切换模型后重试。'; updateComposer(); return; }
  const lastRole = conversation.messages.at(-1)?.role;
  if (lastRole !== (regenerate ? 'assistant' : 'user')) return;
  const history = regenerate ? conversation.messages.slice(0, -1) : conversation.messages;
  const flight = { controller: new AbortController(), text: '', reasoning: '', regenerate, stopped: false };
  pending.set(id, flight);
  failures.delete(id); notices.delete(id); formError = '';
  conversation.provider = chosen.provider; conversation.model = chosen.model;
  save();
  render(); scrollToEnd();
  try {
    const response = await fetch(`${apiBase}/api/chat`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        provider: chosen.provider, model: chosen.model, stream: true,
        messages: modelHistory(history)
      }),
      signal: flight.controller.signal,
    });
    if (!response.ok) {
      const data = await response.json().catch(() => null);
      throw new Error(typeof data?.error === 'string' ? data.error : '请求失败，请重试。');
    }
    await readEvents(response, (event, data) => {
      if (event === 'error') throw new Error(typeof data.error === 'string' ? data.error : '生成失败，请重试。');
      if (event === 'delta') {
        if (typeof data.text !== 'string') throw new Error('回复格式异常。');
        flight.text += data.text; paintStream(id);
      }
      if (event === 'reasoning_delta') {
        if (typeof data.text !== 'string') throw new Error('思考内容格式异常。');
        flight.reasoning += data.text; paintStream(id);
      }
      if (event === 'done') {
        if (!['stop', 'length', 'refusal', 'other'].includes(data.finish_reason)) throw new Error('回复结束状态异常。');
        if (!conversations.some((item) => item.id === id)) return true;
        if (!flight.text.trim() && data.finish_reason !== 'refusal') throw new Error('没有收到有效回复，请重试。');
        const reply = { role: 'assistant', content: flight.text || '模型拒绝回答此问题。', reasoning: flight.reasoning, truncated: data.truncated === true };
        if (regenerate) conversation.messages[conversation.messages.length - 1] = reply;
        else conversation.messages.push(reply);
        save(); return true;
      }
      return false;
    });
  } catch (error) {
    if (!conversations.some((item) => item.id === id)) return;
    if (!regenerate && (flight.text.trim() || flight.reasoning.trim())) {
      conversation.messages.push({ role: 'assistant', content: flight.text, reasoning: flight.reasoning, interrupted: true }); save();
    }
    if (flight.stopped) notices.set(id, regenerate ? '已停止，原回复已保留。' : '已停止生成。');
    else failures.set(id, error instanceof TypeError ? '连接失败，请确认后端服务可用。' : error.message);
  } finally {
    if (pending.get(id) === flight) pending.delete(id);
    render();
    if (activeId === id) { scrollToEnd(); if (!prompt.disabled) prompt.focus(); }
  }
}

$('#chat-form').addEventListener('submit', (event) => {
  event.preventDefault();
  const text = prompt.value.trim();
  if (!text || prompt.disabled || !selectedModel()) return;
  let conversation = currentConversation();
  const nextMessages = modelHistory([...(conversation?.messages || []), { role: 'user', content: text }]);
  if (nextMessages.length > 200 || new TextEncoder().encode(JSON.stringify({ messages: nextMessages })).length > 240 * 1024) {
    formError = '对话内容过长，请新建对话继续。';
    updateComposer();
    return;
  }
  if (!conversation) {
    conversation = { id: makeId(), title: text.replace(/\s+/g, ' ').slice(0, 28), messages: [] };
    conversations.unshift(conversation);
    activeId = conversation.id;
    drafts.delete('new');
  }
  conversation.messages.push({ role: 'user', content: text });
  prompt.value = '';
  drafts.delete(activeId);
  resizePrompt();
  save();
  requestReply(conversation);
});

prompt.addEventListener('input', () => { formError = ''; resizePrompt(); updateComposer(); });
prompt.addEventListener('keydown', (event) => {
  if (event.key === 'Enter' && !event.shiftKey && !event.isComposing) {
    event.preventDefault();
    if (!sendButton.disabled) $('#chat-form').requestSubmit();
  }
});
retryButton.addEventListener('click', () => {
  const conversation = currentConversation();
  if (conversation) requestReply(conversation, { regenerate: conversation.messages.at(-1)?.role === 'assistant' });
});
$('#new-chat').addEventListener('click', newConversation);
$('#brand').addEventListener('click', (event) => { event.preventDefault(); newConversation(); });
$('#open-sidebar').addEventListener('click', () => {
  if (window.innerWidth <= 720) {
    $('#sidebar').inert = false;
    shell.classList.add('sidebar-open');
    $('#sidebar-overlay').hidden = false;
    $('#open-sidebar').setAttribute('aria-expanded', 'true');
    $('#close-sidebar').focus();
  } else {
    shell.classList.remove('sidebar-collapsed');
    $('#open-sidebar').setAttribute('aria-expanded', 'true');
  }
});
$('#close-sidebar').addEventListener('click', () => {
  closeSidebar();
  if (window.innerWidth > 720) shell.classList.add('sidebar-collapsed');
  $('#open-sidebar').setAttribute('aria-expanded', 'false');
  $('#open-sidebar').focus();
});
$('#sidebar-overlay').addEventListener('click', closeSidebar);
document.addEventListener('keydown', (event) => { if (event.key === 'Escape') closeSidebar(); });
window.addEventListener('resize', () => { closeModelMenu(); closeSidebar(); resizePrompt(); });
closeSidebar();
load();
render();
resizePrompt();
scrollToEnd();

loadModels();
