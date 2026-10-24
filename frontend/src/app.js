const host = window.location.hostname || 'localhost';
const apiBase = location.protocol !== 'file:' && location.port !== '8000'
  ? location.origin : `http://${host.includes(':') ? `[${host.replace(/^\[|\]$/g, '')}]` : host}:53411`;
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
let historyError = '';
let currentUser = null;
let authEpoch = 0;
let authMode = 'login';
let nextHistory = null;
const polling = new Map();
let formError = '';
let accountView = 'home';
let accountViewVersion = 0;
let logoutAllReturnView = 'home';
let selectedDevice = null;
let deviceSessions = [];
let devicesCursor = '0';
let devicesLoading = false;
let emailVerifiedUntil = 0;
let emailVerificationTimer = null;

function icon(name) {
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.setAttribute('aria-hidden', 'true');
  const use = document.createElementNS('http://www.w3.org/2000/svg', 'use');
  use.setAttribute('href', `#icon-${name}`);
  svg.append(use);
  return svg;
}

class ApiError extends Error {
  constructor(message, code) { super(message); this.code = code; }
}
async function api(path, body) {
  const epoch = authEpoch;
  const response = await fetch(`${apiBase}${path}`, {
    method: body === undefined ? 'GET' : 'POST', credentials: 'include',
    headers: body === undefined ? {} : { 'Content-Type': 'application/json', 'X-Chat-Request': '1' },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const data = await response.json().catch(() => ({}));
  if (epoch !== authEpoch) throw new ApiError('账号已切换。', 'session_changed');
  if (!response.ok) {
    if (data.code === 'unauthenticated' && currentUser) signedOut('登录已过期，请重新登录。');
    throw new ApiError(data.error || '请求失败，请稍后重试。', data.code || 'request_failed');
  }
  return data;
}
function feedback(target, message, success = false) {
  target.textContent = message;
  target.classList.toggle('success', success);
}
function setAuthMode(mode, message = '') {
  authMode = mode;
  const labels = {
    username: mode === 'register', identifier: mode === 'login',
    email: ['register', 'forgot', 'reset'].includes(mode), code: mode === 'reset',
    password: mode !== 'forgot', confirm: ['register', 'reset'].includes(mode),
  };
  for (const [name, visible] of Object.entries(labels)) {
    $(`#auth-${name}-label`).hidden = !visible;
    const input = $(`#auth-${name}`);
    input.disabled = !visible;
    input.required = visible && !(name === 'email' && mode === 'register');
  }
  $('#auth-email-hint').hidden = mode !== 'register';
  $('#auth-password').value = '';
  $('#auth-confirm').value = '';
  $('#auth-password').autocomplete = mode === 'login' ? 'current-password' : 'new-password';
  $('#auth-password').minLength = mode === 'login' ? 1 : 15;
  $('#auth-title').textContent = { login: '欢迎回来', register: '创建你的账号', forgot: '找回密码', reset: '设置新密码' }[mode];
  $('#auth-intro').textContent = {
    login: '登录后，继续你的对话。', register: '用一个账号，保存每一次对话。',
    forgot: '通过已绑定的邮箱接收找回验证码。', reset: '输入邮箱验证码，为账号设置新密码。',
  }[mode];
  $('#auth-submit').textContent = { login: '登录', register: '创建账号', forgot: '发送找回验证码', reset: '重置密码' }[mode];
  $('#auth-switch').textContent = mode === 'login' ? '创建账号' : '返回登录';
  $('#auth-forgot').hidden = mode !== 'login';
  $('#auth-reset').hidden = mode !== 'forgot';
  feedback($('#auth-status'), message);
}
function signedOut(message = '') {
  currentUser = null;
  ++authEpoch;
  for (const flight of pending.values()) flight.controller.abort();
  for (const timer of polling.values()) clearTimeout(timer);
  polling.clear(); pending.clear(); drafts.clear(); failures.clear(); notices.clear();
  conversations = []; activeId = null; models = []; defaultModel = null;
  prompt.value = ''; formError = ''; historyError = ''; modelError = '';
  if ($('#account-dialog').open) $('#account-dialog').close();
  $('#password-form').reset(); $('#email-form').reset(); $('#email-verify-form').reset();
  clearTimeout(emailVerificationTimer); emailVerifiedUntil = 0;
  shell.hidden = true; $('#auth-screen').hidden = false;
  setAuthMode('login', message);
  render();
}
async function startSession(user) {
  currentUser = user;
  ++authEpoch;
  $('#auth-form').reset();
  $('#auth-screen').hidden = true; shell.hidden = false;
  $('#account-name').textContent = user.username;
  $('#account-avatar').textContent = user.username[0].toUpperCase();
  render();
  await Promise.all([loadModels(), loadHistory()]);
}
async function loadHistory(more = false) {
  if (!currentUser) return;
  try {
    let query = '';
    if (more && nextHistory) query = `?before_updated_at=${nextHistory.before_updated_at}&before_id=${nextHistory.before_id}`;
    const data = await api(`/api/conversations${query}`);
    const existing = new Map(conversations.map((item) => [item.id, item]));
    const items = data.items.map((item) => ({ ...item, messages: existing.get(item.id)?.messages || [], loaded: existing.get(item.id)?.loaded || false, provider: existing.get(item.id)?.provider, model: existing.get(item.id)?.model }));
    conversations = more ? [...conversations, ...items.filter((item) => !existing.has(item.id))] : items;
    nextHistory = data.next;
    $('#more-history').hidden = !nextHistory;
    historyError = '';
    const hash = /^#conversation-(\d+)$/.exec(location.hash)?.[1];
    const id = activeId || (conversations.some((item) => item.id === hash) ? hash : null);
    if (id) await switchConversation(id);
    else render();
  } catch (error) { if (currentUser && error.code !== 'session_changed') { historyError = error.message; render(); } }
}
async function refreshConversation(conversation) {
  const data = await api(`/api/conversation?id=${conversation.id}`);
  if (!conversations.includes(conversation)) return;
  Object.assign(conversation, data.conversation, { messages: data.messages, loaded: true });
  const last = conversation.messages.at(-1);
  if (last?.error) failures.set(conversation.id, last.error);
  else failures.delete(conversation.id);
  if ((conversation.active_message_id || last?.state === 'generating') && !pending.has(conversation.id)) {
    clearTimeout(polling.get(conversation.id));
    polling.set(conversation.id, setTimeout(async () => {
      polling.delete(conversation.id);
      if (currentUser && conversations.includes(conversation) && !pending.has(conversation.id)) {
        try { await refreshConversation(conversation); render(); } catch (error) { historyError = error.message; render(); }
      }
    }, 1000));
  }
}
async function withForm(form, target, action) {
  const owner = form.closest('#account-dialog') || form;
  const controls = [...owner.querySelectorAll('input, button')];
  if (form.id === 'auth-form') controls.push(...document.querySelectorAll('.auth-links button'));
  if (owner.dataset.busy) return;
  owner.dataset.busy = 'true';
  controls.forEach((element) => { element.disabled = true; });
  feedback(target, '正在处理…');
  let reauthenticate = false;
  try { await action(); }
  catch (error) {
    if (error.code === 'reauthentication_required' && owner.id === 'account-dialog') {
      showAccountView('email', false); reauthenticate = true;
    }
    if (error.code !== 'session_changed') feedback(target, error instanceof TypeError ? '无法连接服务，请稍后重试。' : error.message);
  }
  finally {
    delete owner.dataset.busy; controls.forEach((element) => { element.disabled = false; });
    if (reauthenticate) focusAccountView('email');
  }
}
$('#auth-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const mode = authMode;
  const password = $('#auth-password').value;
  if (['register', 'reset'].includes(mode) && password !== $('#auth-confirm').value) { feedback($('#auth-status'), '两次输入的密码不一致。'); return; }
  const identifier = $('#auth-identifier').value.trim();
  const username = $('#auth-username').value.trim();
  const email = $('#auth-email').value.trim();
  const code = $('#auth-code').value.trim();
  await withForm(event.currentTarget, $('#auth-status'), async () => {
    if (mode === 'forgot') {
      const data = await api('/api/auth/password/forgot', { email });
      setAuthMode('reset', data.message);
      return;
    }
    if (mode === 'reset') {
      await api('/api/auth/password/reset', { email, code, new_password: password });
      setAuthMode('login', '密码已重置，请使用新密码登录。');
      return;
    }
    let registration;
    if (mode === 'register') registration = await api('/api/auth/register', { username, password, ...(email ? { email } : {}) });
    const data = await api('/api/auth/login', { identifier: mode === 'register' ? username : identifier, password });
    await startSession(data.user);
    if (registration && email) {
      openAccount('email');
      $('#email-form').elements.email.value = email;
      feedback($('#account-status'), registration.email_delivery === 'sent' ? '账号已创建，验证码已发送。请先验证当前密码，再填写邮箱验证码。' : '账号已创建。请先验证当前密码，再发送邮箱验证码。', registration.email_delivery === 'sent');
    }
  });
  // 模式切换后重新应用隐藏字段的 disabled / required。
  for (const label of $('#auth-form').querySelectorAll('label')) if (label.hidden) label.querySelector('input').disabled = true;
});
$('#auth-switch').addEventListener('click', () => setAuthMode(authMode === 'login' ? 'register' : 'login'));
$('#auth-forgot').addEventListener('click', () => setAuthMode('forgot'));
$('#auth-reset').addEventListener('click', () => setAuthMode('reset'));
function deviceName(agent) {
  const browser = /Edg\//.test(agent) ? 'Edge' : /OPR\//.test(agent) ? 'Opera' : /Firefox\/|FxiOS\//.test(agent) ? 'Firefox'
    : /Chrome\/|CriOS\//.test(agent) ? 'Chrome' : /Safari\//.test(agent) ? 'Safari' : '未知浏览器';
  const system = /iPhone|iPad|iPod/.test(agent) ? 'iOS' : /Android/.test(agent) ? 'Android'
    : /Windows/.test(agent) ? 'Windows' : /Macintosh|Mac OS X/.test(agent) ? 'macOS' : /Linux/.test(agent) ? 'Linux' : '未知系统';
  return `${browser} · ${system}`;
}
function renderDevices() {
  const list = $('#device-list');
  list.replaceChildren();
  const items = [...deviceSessions].sort((a, b) => Number(b.current) - Number(a.current) || b.last_active_at - a.last_active_at);
  for (const device of items) {
    const row = document.createElement('li'); row.className = 'device-row';
    const details = document.createElement('div'); details.className = 'device-details';
    const name = document.createElement('div'); name.className = 'device-name'; name.textContent = deviceName(device.user_agent);
    if (device.current) {
      const badge = document.createElement('span'); badge.className = 'current-device'; badge.textContent = '当前设备'; name.append(badge);
    }
    const time = document.createElement('small');
    const format = value => new Date(value).toLocaleString('zh-CN', { month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit' });
    time.textContent = `最近活动 ${format(device.last_active_at)}；登录于 ${format(device.created_at)}`;
    details.append(name, time);
    const button = document.createElement('button'); button.type = 'button'; button.className = 'text-button'; button.textContent = '退出登录';
    button.setAttribute('aria-label', `退出${device.current ? '当前设备' : deviceName(device.user_agent)}`);
    button.addEventListener('click', () => {
      selectedDevice = device;
      $('#logout-device-description').textContent = device.current ? '退出当前设备后，需要重新登录。其他设备保持登录。' : `退出 ${deviceName(device.user_agent)} 后，该设备需要重新登录。`;
      showAccountView('logout-device');
    });
    row.append(details, button); list.append(row);
  }
}
async function loadDevices(more = false) {
  if (!currentUser || accountView !== 'devices' || devicesLoading) return;
  devicesLoading = true;
  const version = accountViewVersion;
  $('#devices-more').disabled = true;
  feedback($('#devices-status'), '正在加载已登录设备…');
  try {
    const page = await api(`/api/auth/sessions?cursor=${more ? devicesCursor : '0'}`);
    if (version !== accountViewVersion) return;
    const existing = new Map((more ? deviceSessions : []).map(item => [item.id, item]));
    for (const device of page.items) existing.set(device.id, device);
    deviceSessions = [...existing.values()];
    devicesCursor = page.next_cursor;
    renderDevices();
    feedback($('#devices-status'), deviceSessions.length ? '' : '暂无已登录设备。');
    $('#devices-more').hidden = devicesCursor === '0';
    $('#devices-more').textContent = '加载更多';
  } catch (error) {
    if (version !== accountViewVersion) return;
    feedback($('#devices-status'), error instanceof TypeError ? '无法加载设备，请重试。' : error.message);
    $('#devices-more').hidden = false;
    $('#devices-more').textContent = '重试';
  } finally {
    if (version === accountViewVersion) { devicesLoading = false; $('#devices-more').disabled = false; }
  }
}
function focusAccountView(view) {
  const target = view === 'password' ? $('#password-form input') : view === 'email' ? $('#email-verify-form input') : view === 'email-edit' ? $('#email-form input')
    : view === 'logout-all' ? $('#cancel-logout-all') : view === 'logout-device' ? $('#cancel-logout-device') : $('#account-title');
  target.focus({ preventScroll: true });
}
function showAccountView(view, focus = true) {
  const titles = { home: '账号设置', password: '修改密码', email: '验证当前密码', 'email-edit': '邮箱', devices: '已登录设备', 'logout-all': '退出全部设备', 'logout-device': '退出设备' };
  if (!titles[view]) return;
  if (view === 'email-edit' && Date.now() >= emailVerifiedUntil) view = 'email';
  const continuingEmail = ['email', 'email-edit'].includes(accountView) && ['email', 'email-edit'].includes(view);
  clearTimeout(emailVerificationTimer);
  if (view !== 'email-edit') emailVerifiedUntil = 0;
  $('#email-verify-form').reset();
  if (view === 'logout-all') logoutAllReturnView = accountView === 'devices' ? 'devices' : 'home';
  accountView = view;
  ++accountViewVersion; devicesLoading = false;
  $('#password-form').reset();
  if (!continuingEmail) $('#email-form').reset();
  for (const panel of document.querySelectorAll('[data-account-panel]')) panel.hidden = panel.dataset.accountPanel !== view;
  $('#account-title').textContent = titles[view];
  $('#account-back').hidden = view === 'home';
  feedback($('#account-status'), '');
  $('#account-dialog').scrollTop = 0;
  if (view === 'devices') {
    deviceSessions = []; devicesCursor = '0'; $('#device-list').replaceChildren(); $('#devices-more').hidden = true;
    loadDevices();
  }
  if (view === 'email-edit') emailVerificationTimer = setTimeout(() => {
    showAccountView('email');
    feedback($('#account-status'), '身份验证已过期，请重新验证密码。');
  }, Math.max(0, emailVerifiedUntil - Date.now()));
  if (focus) focusAccountView(view);
}
function openAccount(view = 'home') {
  if (!currentUser) return;
  $('#account-summary').textContent = currentUser.username;
  $('#email-overview').textContent = currentUser.email || '尚未绑定';
  $('#email-summary').textContent = currentUser.email ? `已绑定 ${currentUser.email}` : '绑定后可用邮箱登录和找回密码。';
  $('#email-address-label').textContent = currentUser.email ? '新邮箱' : '邮箱地址';
  $('#email-unbind').hidden = !currentUser.email;
  showAccountView(view, false);
  if (!$('#account-dialog').open) $('#account-dialog').showModal();
  focusAccountView(view);
}
$('#open-account').addEventListener('click', () => openAccount());
for (const button of document.querySelectorAll('[data-account-view]')) {
  button.addEventListener('click', () => showAccountView(button.dataset.accountView));
}
$('#devices-more').addEventListener('click', () => loadDevices(true));
$('#account-back').addEventListener('click', () => showAccountView(accountView === 'logout-device' ? 'devices' : accountView === 'email-edit' ? 'email' : accountView === 'logout-all' ? logoutAllReturnView : 'home'));
$('#cancel-logout-all').addEventListener('click', () => showAccountView(logoutAllReturnView));
$('#cancel-logout-device').addEventListener('click', () => showAccountView('devices'));
$('#close-account').addEventListener('click', () => $('#account-dialog').close());
$('#account-dialog').addEventListener('cancel', (event) => { if ($('#account-dialog').dataset.busy) event.preventDefault(); });
$('#account-dialog').addEventListener('close', () => { showAccountView('home', false); deviceSessions = []; selectedDevice = null; $('#device-list').replaceChildren(); });
$('#logout').addEventListener('click', async () => {
  await withForm($('#logout-all-form'), $('#account-status'), async () => { await api('/api/auth/logout', {}); signedOut('已退出当前设备。'); });
});
$('#logout-all-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  await withForm(event.currentTarget, $('#account-status'), async () => {
    await api('/api/auth/logout/all', {});
    signedOut('已退出全部设备，请重新登录。');
  });
});
$('#logout-device-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const device = selectedDevice;
  if (!device) return;
  await withForm(event.currentTarget, $('#account-status'), async () => {
    await api('/api/auth/sessions/revoke', { session_id: device.id });
    if (device.current) signedOut('已退出当前设备。');
    else { showAccountView('devices'); feedback($('#account-status'), '已退出该设备。', true); }
  });
});
$('#password-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const form = event.currentTarget;
  const values = Object.fromEntries(new FormData(form));
  if (values.new_password !== values.confirm) { feedback($('#account-status'), '两次输入的密码不一致。'); return; }
  await withForm(form, $('#account-status'), async () => {
    await api('/api/auth/password', { old_password: values.old_password, new_password: values.new_password });
    signedOut('密码已更新，请重新登录。');
  });
});
$('#email-verify-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const form = event.currentTarget;
  const password = form.elements.password.value;
  let verified = false;
  await withForm(form, $('#account-status'), async () => {
    const result = await api('/api/auth/password/verify', { password });
    emailVerifiedUntil = Date.now() + result.expires_in * 1000;
    showAccountView('email-edit', false);
    verified = true;
  });
  if (verified && accountView === 'email-edit') focusAccountView('email-edit');
});
function requireEmailVerification() {
  if (Date.now() < emailVerifiedUntil) return true;
  showAccountView('email');
  feedback($('#account-status'), '身份验证已过期，请重新验证密码。');
  return false;
}
$('#email-send').addEventListener('click', async () => {
  const form = $('#email-form');
  if (!requireEmailVerification() || !form.elements.email.reportValidity()) return;
  await withForm(form, $('#account-status'), async () => {
    await api('/api/auth/email/code', { email: form.elements.email.value });
    feedback($('#account-status'), '验证码已发送，请查收邮件。', true);
  });
});
$('#email-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  if (!requireEmailVerification()) return;
  const form = event.currentTarget;
  const body = Object.fromEntries(new FormData(form));
  await withForm(form, $('#account-status'), async () => {
    const data = await api('/api/auth/email/bind', body);
    currentUser = data.user; form.reset(); openAccount(); feedback($('#account-status'), '邮箱已验证并绑定。', true);
  });
});
$('#email-unbind').addEventListener('click', async () => {
  if (!requireEmailVerification()) return;
  await withForm($('#email-form'), $('#account-status'), async () => {
    const data = await api('/api/auth/email/unbind', {});
    currentUser = data.user; openAccount(); feedback($('#account-status'), '邮箱已解绑。', true);
  });
});
$('#more-history').addEventListener('click', () => loadHistory(true));
$('#rename-conversation').addEventListener('click', async () => {
  const conversation = currentConversation();
  if (!conversation) return;
  const title = window.prompt('对话标题', conversation.title)?.trim();
  if (!title) return;
  try { await api('/api/conversation/rename', { conversation_id: conversation.id, title }); conversation.title = title; render(); }
  catch (error) { formError = error.message; updateComposer(); }
});

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

async function switchConversation(id) {
  drafts.set(activeId || 'new', prompt.value);
  activeId = id;
  formError = '';
  prompt.value = drafts.get(id || 'new') || '';
  history.replaceState(null, '', id ? `#conversation-${id}` : location.pathname);
  if (id && !pending.has(id)) {
    const conversation = currentConversation();
    if (conversation) {
      try { await refreshConversation(conversation); }
      catch (error) { historyError = error.message; }
    }
  }
  if (activeId !== id) return;
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
    remove.addEventListener('click', async () => {
      try { await api('/api/conversation/delete', { conversation_id: conversation.id }); }
      catch (error) { formError = error.message; updateComposer(); return; }
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
  const remote = Boolean(conversation?.active_message_id) || conversation?.messages.at(-1)?.state === 'generating';
  prompt.disabled = !currentUser || waiting || remote || Boolean(unanswered);
  prompt.placeholder = thinking ? '正在思考…' : waiting ? '正在等待回复…' : unanswered ? '重试上一条消息，或新建对话' : '有什么我可以帮你？';
  const chosen = selectedModel();
  sendButton.hidden = waiting;
  stopButton.hidden = !waiting;
  stopButton.disabled = pending.get(activeId)?.stopped === true;
  sendButton.disabled = prompt.disabled || !prompt.value.trim() || !chosen;
  modelSelect.disabled = waiting || !models.length;
  if (modelSelect.disabled) closeModelMenu();
  messagesElement.setAttribute('aria-busy', String(waiting));
  retryButton.hidden = remote || waiting || (!unanswered && !failures.has(activeId));
  const error = formError || failures.get(activeId) || modelError || historyError;
  status.classList.toggle('error', Boolean(error));
  status.textContent = error || notices.get(activeId) || (remote && !waiting ? '回复仍在生成，历史会自动更新。' : '') || (thinking ? '正在思考，可以随时停止。' : waiting ? '正在生成，可以随时停止。' : '') || (unanswered && !waiting ? '上一条消息还未收到回复，可以重试。' : '');
}

function render() {
  const conversation = currentConversation();
  workspace.classList.toggle('empty', !conversation);
  $('#conversation-title').textContent = conversation?.title || '新对话';
  $('#rename-conversation').hidden = !conversation;
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
      if (conversation) { conversation.provider = model.provider; conversation.model = model.model;  }
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
    const data = await api('/api/models');
    if (!Array.isArray(data.models) || !data.models.length || !data.models.every((item) =>
      typeof item.provider === 'string' && typeof item.model === 'string')) throw new Error('没有可用模型，请检查后端配置。');
    models = data.models;
    if (!defaultModel || !models.some((model) => modelKey(model) === modelKey(defaultModel))) defaultModel = models[0];
    renderModelOptions();
  } catch (error) {
    if (!currentUser || error.code === 'session_changed') return;
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
  const question = regenerate ? conversation.messages.at(-2) : conversation.messages.at(-1);
  if (!question || (regenerate && !question.id)) return;
  const flight = { controller: new AbortController(), text: '', reasoning: '', regenerate, stopped: false };
  pending.set(id, flight);
  failures.delete(id); notices.delete(id); formError = '';
  conversation.provider = chosen.provider; conversation.model = chosen.model;

  render(); scrollToEnd();
  try {
    const response = await fetch(`${apiBase}/api/chat`, {
      method: 'POST',
      credentials: 'include',
      headers: { 'Content-Type': 'application/json', 'X-Chat-Request': '1' },
      body: JSON.stringify({
        provider: chosen.provider, model: chosen.model, stream: true,
        conversation_id: id, request_id: makeId(),
        ...(regenerate ? { regenerate_message_id: question.id } : { content: question.content })
      }),
      signal: flight.controller.signal,
    });
    if (!response.ok) {
      const data = await response.json().catch(() => null);
      if (data?.code === 'unauthenticated') signedOut('登录已过期，请重新登录。');
      throw new Error(typeof data?.error === 'string' ? data.error : '请求失败，请重试。');
    }
    await readEvents(response, (event, data) => {
      if (event === 'meta') { question.id = data.user_message_id; flight.assistantId = data.assistant_message_id; }
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
        const reply = { id: data.assistant_message_id || flight.assistantId, state: 'completed', role: 'assistant', content: flight.text || '模型拒绝回答此问题。', reasoning: flight.reasoning, truncated: data.truncated === true };
        if (regenerate) conversation.messages[conversation.messages.length - 1] = reply;
        else conversation.messages.push(reply);
         return true;
      }
      return false;
    });
  } catch (error) {
    if (!conversations.some((item) => item.id === id)) return;
    if (!regenerate && (flight.text.trim() || flight.reasoning.trim())) {
      conversation.messages.push({ role: 'assistant', content: flight.text, reasoning: flight.reasoning, interrupted: true });
    }
    if (flight.stopped) notices.set(id, regenerate ? '已停止，原回复已保留。' : '已停止生成。');
    else failures.set(id, error instanceof TypeError ? '连接失败，请确认后端服务可用。' : error.message);
  } finally {
    if (pending.get(id) === flight) pending.delete(id);
    if (currentUser && conversations.includes(conversation)) {
      try { await refreshConversation(conversation); }
      catch (error) { historyError = error.message; }
    }
    render();
    if (activeId === id) { scrollToEnd(); if (!prompt.disabled) prompt.focus(); }
  }
}

$('#chat-form').addEventListener('submit', async (event) => {
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
    sendButton.disabled = true;
    try {
      const data = await api('/api/conversations', { title: text.replace(/\s+/g, ' ').slice(0, 28) });
      conversation = { ...data.conversation, messages: [], loaded: true };
    } catch (error) { formError = error.message; updateComposer(); return; }
    conversations.unshift(conversation);
    activeId = conversation.id;
    history.replaceState(null, '', `#conversation-${activeId}`);
    drafts.delete('new');
  }
  conversation.messages.push({ role: 'user', content: text });
  prompt.value = '';
  drafts.delete(activeId);
  resizePrompt();

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
render();
resizePrompt();
setAuthMode('login');
api('/api/auth/me').then((data) => startSession(data.user)).catch((error) => {
  if (error.code !== 'unauthenticated') feedback($('#auth-status'), error.message || '无法连接服务。');
});
