import { $, formInput } from './dom';
import type { AccountView, AuthMode, ChatRequest, Conversation, DeviceSession, GetResponses, HistoryCursor, Message, Model, PendingRegenerate, PendingReply, PendingSend, PostContracts, ReasoningEffort, ReasoningView, Registration, User } from './types';
import type * as MarkdownRenderer from './markdown';

declare global {
  interface Window { ChatMarkdown?: typeof MarkdownRenderer }
}

const host = window.location.hostname || 'localhost';
const apiBase = location.protocol !== 'file:' && location.port !== '8000'
  ? location.origin : `http://${host.includes(':') ? `[${host.replace(/^\[|\]$/g, '')}]` : host}:53411`;
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
const modelOptions = $('#model-options');
const effortOptions = $('#effort-options');
const effortInputs = effortOptions.querySelectorAll<HTMLInputElement>('input');
const stopButton = $('#stop');
let models: Model[] = [];
let modelError = '';
let defaultModel: Model | null = null;
let defaultReasoningEffort: ReasoningEffort | undefined;
let streamFrame: number | null = null;
let streamElement: HTMLElement | null = null;
const scrollArea = $('#conversation-scroll');
const pending = new Map<string | null, PendingReply>();
const pendingSends = new Map<string, PendingSend>();
const pendingRegenerates = new Map<string, PendingRegenerate>();
const selectingBranches = new Set<string>();
const failures = new Map<string | null, string>();
const notices = new Map<string | null, string>();
const drafts = new Map<string | null, string>();
const messageViews = new WeakMap<Message, HTMLElement>();
const reasoningViews = new WeakMap<Message | PendingReply, ReasoningView>();
let conversations: Conversation[] = [];
let activeId: string | null = null;
let historyError = '';
let currentUser: User | null = null;
let authEpoch = 0;
let authMode: AuthMode = 'login';
let nextHistory: HistoryCursor | null = null;
const polling = new Map<string, number>();
const conversationErrors = new Map<string, string>();
const latestRefresh = new WeakMap<Conversation, object>();
let conversationLoad: { id: string } | null = null;
let formError = '';
let creatingConversation: { draftVersion: number } | null = null;
let newDraftVersion = 0;
let createError = '';
let accountView: AccountView = 'home';
let accountViewVersion = 0;
let logoutAllReturnView: AccountView = 'home';
let selectedDevice: DeviceSession | null = null;
let deviceSessions: DeviceSession[] = [];
let devicesCursor = '0';
let devicesLoading = false;
let emailVerifiedUntil = 0;
let emailVerificationTimer: number | undefined;

function icon(name: string) {
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.setAttribute('aria-hidden', 'true');
  const use = document.createElementNS('http://www.w3.org/2000/svg', 'use');
  use.setAttribute('href', `#icon-${name}`);
  svg.append(use);
  return svg;
}

class ApiError extends Error {
  constructor(message: string, readonly code: string, readonly status?: number) { super(message); }
}
function errorCode(error: unknown): string | undefined {
  return error instanceof ApiError ? error.code : undefined;
}
function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : '请求失败，请稍后重试。';
}
function errorPayload(data: unknown): { code?: string; error?: string } {
  if (typeof data !== 'object' || data === null) return {};
  return {
    code: 'code' in data && typeof data.code === 'string' ? data.code : undefined,
    error: 'error' in data && typeof data.error === 'string' ? data.error : undefined,
  };
}
function matchingPendingSend(id: string | null, text = prompt.value.trim()): PendingSend | undefined {
  if (!id) return undefined;
  const send = pendingSends.get(id);
  return send && send.content === text ? send : undefined;
}
function clearConfirmedSend(id: string, send: PendingSend) {
  if (pendingSends.get(id) === send) pendingSends.delete(id);
  if (drafts.get(id) === send.raw) drafts.delete(id);
  if (activeId === id && prompt.value === send.raw) {
    prompt.value = '';
    resizePrompt();
  }
}
function unsavedFailureMessage(error: unknown): string {
  if (error instanceof TypeError || errorCode(error) === 'stream_incomplete') {
    return '连接已断开，保存结果尚未确认，可用原请求重试。';
  }
  return errorMessage(error);
}
function api<P extends keyof GetResponses>(path: P): Promise<GetResponses[P]>;
function api<P extends keyof PostContracts>(path: P, body: PostContracts[P]['body']): Promise<PostContracts[P]['response']>;
async function api(path: string, body?: object): Promise<unknown> {
  const epoch = authEpoch;
  const response = await fetch(`${apiBase}${path}`, {
    method: body === undefined ? 'GET' : 'POST', credentials: 'include',
    headers: body === undefined ? {} : { 'Content-Type': 'application/json', 'X-Chat-Request': '1' },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const data: unknown = await response.json().catch(() => ({}));
  if (epoch !== authEpoch) throw new ApiError('账号已切换。', 'session_changed', response.status);
  if (!response.ok) {
    const failure = errorPayload(data);
    if (failure.code === 'unauthenticated' && currentUser) signedOut('登录已过期，请重新登录。');
    throw new ApiError(failure.error || '请求失败，请稍后重试。', failure.code || 'request_failed', response.status);
  }
  return data;
}
function feedback(target: HTMLElement, message: string, success = false) {
  target.textContent = message;
  target.classList.toggle('success', success);
}
function setAuthMode(mode: AuthMode, message = '') {
  authMode = mode;
  const labels = {
    username: mode === 'register', identifier: mode === 'login',
    email: ['register', 'forgot', 'reset'].includes(mode), code: mode === 'reset',
    password: mode !== 'forgot', confirm: ['register', 'reset'].includes(mode),
  };
  for (const [name, visible] of Object.entries(labels)) {
    $(`#auth-${name}-label`).hidden = !visible;
    const input = $<HTMLInputElement>(`#auth-${name}`);
    input.disabled = !visible;
    input.required = visible && !(name === 'email' && mode === 'register');
  }
  $('#auth-email-hint').hidden = mode !== 'register';
  $('#auth-password').value = '';
  $('#auth-confirm').value = '';
  $('#auth-password').autocomplete = mode === 'login' ? 'current-password' : 'new-password';
  $('#auth-password').minLength = mode === 'login' ? 1 : 8;
  $('#auth-password').placeholder = mode === 'login' ? '' : '8–128 个字符';
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
  polling.clear(); pending.clear(); pendingSends.clear(); pendingRegenerates.clear(); drafts.clear(); failures.clear(); notices.clear();
  selectingBranches.clear(); conversationErrors.clear(); conversationLoad = null;
  creatingConversation = null; createError = '';
  conversations = []; activeId = null; models = []; defaultModel = null; defaultReasoningEffort = undefined;
  prompt.value = ''; formError = ''; historyError = ''; modelError = '';
  closeAccount(false);
  $('#password-form').reset(); $('#email-form').reset(); $('#email-verify-form').reset();
  clearTimeout(emailVerificationTimer); emailVerifiedUntil = 0;
  shell.hidden = true; $('#auth-screen').hidden = false;
  setAuthMode('login', message);
  render();
}
async function startSession(user: User) {
  currentUser = user;
  const epoch = ++authEpoch;
  const initialId = conversationIdFromUrl();
  activeId = initialId;
  $('#auth-form').reset();
  $('#auth-screen').hidden = true; shell.hidden = false;
  updateAccountUser(user);
  render();
  await Promise.all([loadModels(), loadHistory().then(() => {
    if (epoch === authEpoch && initialId && activeId === initialId && !currentConversation()?.loaded && !conversationLoad) {
      return switchConversation(initialId, true);
    }
  })]);
}
async function loadHistory(more = false) {
  if (!currentUser) return;
  try {
    let query: '' | `?${string}` = '';
    if (more && nextHistory) query = `?before_updated_at=${nextHistory.before_updated_at}&before_id=${nextHistory.before_id}`;
    const data = await api(`/api/conversations${query}`);
    const existing = new Set(conversations.map((item) => item.id));
    conversations.push(...data.items.filter((item) => !existing.has(item.id))
      .map((item) => ({ ...item, messages: [], loaded: false })));
    nextHistory = data.next;
    $('#more-history').hidden = !nextHistory;
    historyError = '';
    render();
  } catch (error) { if (currentUser && errorCode(error) !== 'session_changed') { historyError = error instanceof TypeError ? '无法连接服务，请稍后重试。' : errorMessage(error); render(); } }
}
async function refreshConversation(conversation: Conversation) {
  const refresh = {};
  latestRefresh.set(conversation, refresh);
  try {
    const data = await api(`/api/conversation?id=${conversation.id}`);
    if (latestRefresh.get(conversation) === refresh && conversations.includes(conversation)) {
      applyConversation(conversation, data);
      return true;
    }
  } catch (error) {
    if (latestRefresh.get(conversation) === refresh && conversations.includes(conversation)) throw error;
  }
}

function updateConversationMessages(conversation: Conversation, messages: Message[]) {
  const previous = new Map(conversation.messages.map((message) => [message.id, message]));
  conversation.messages = messages.map((message) => {
    const existing = message.id && previous.get(message.id);
    const keys = Object.keys(message) as (keyof Message)[];
    if (!existing || Object.keys(existing).length !== keys.length) return message;
    // 刷新历史时保留内容相同的节点，避免清除用户选中的文字。
    const unchanged = keys.every((key) =>
      key === 'versions' ? JSON.stringify(message.versions) === JSON.stringify(existing.versions)
        : message[key] === existing[key]);
    return unchanged ? existing : message;
  });
}

function applyConversation(conversation: Conversation, data: GetResponses[`/api/conversation?${string}`]) {
  conversationErrors.delete(conversation.id);
  Object.assign(conversation, data.conversation, { loaded: true });
  updateConversationMessages(conversation, data.messages);
  failures.delete(conversation.id);
  scheduleConversationPoll(conversation);
}

function isGenerating(conversation: Conversation | undefined) {
  return Boolean(conversation?.active_message_id) || conversation?.messages.at(-1)?.state === 'generating';
}

function recordConversationError(conversationId: string, error: unknown) {
  conversationErrors.set(conversationId, error instanceof TypeError ? '无法连接服务，请稍后重试。' : errorMessage(error));
}

function isRetryableConversationError(error: unknown) {
  return error instanceof TypeError || (error instanceof ApiError
    && (!error.status || error.status >= 500 || error.status === 429));
}

function scheduleConversationPoll(conversation: Conversation, delay = 1000) {
  const { id } = conversation;
  if (polling.has(id) || pending.has(id) || !isGenerating(conversation)) return;
  polling.set(id, setTimeout(async () => {
    polling.delete(id);
    if (!currentUser || !conversations.includes(conversation) || pending.has(id) || !isGenerating(conversation)) return;
    if (conversationLoad?.id === id) return;
    try {
      await refreshConversation(conversation);
    } catch (error) {
      if (!currentUser || !conversations.includes(conversation) || errorCode(error) === 'session_changed') return;
      recordConversationError(id, error);
      if (isRetryableConversationError(error)) scheduleConversationPoll(conversation, 2000);
    }
    if (activeId === id && conversations.includes(conversation)) renderReplyProgress();
  }, delay));
}
async function withForm(form: HTMLFormElement, target: HTMLElement, action: () => Promise<void>) {
  const owner = form.closest<HTMLElement>('#account-page') || form;
  const controls = [...owner.querySelectorAll<HTMLInputElement | HTMLButtonElement>('input, button')];
  if (form.id === 'auth-form') controls.push(...document.querySelectorAll<HTMLButtonElement>('.auth-links button'));
  if (owner.dataset.busy) return;
  owner.dataset.busy = 'true';
  controls.forEach((element) => { element.disabled = true; });
  feedback(target, '正在处理…');
  let reauthenticate = false;
  try { await action(); }
  catch (error) {
    if (errorCode(error) === 'reauthentication_required' && owner.id === 'account-page') {
      showAccountView('email', false); reauthenticate = true;
    }
    if (errorCode(error) !== 'session_changed') feedback(target, error instanceof TypeError ? '无法连接服务，请稍后重试。' : errorMessage(error));
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
  await withForm($('#auth-form'), $('#auth-status'), async () => {
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
    let registration: Registration | undefined;
    if (mode === 'register') registration = await api('/api/auth/register', { username, password, ...(email ? { email } : {}) });
    const data = await api('/api/auth/login', { identifier: mode === 'register' ? username : identifier, password });
    await startSession(data.user);
    if (registration && email) {
      openAccount('email');
      formInput($('#email-form'), 'email').value = email;
      feedback($('#account-status'), registration.email_delivery === 'sent' ? '账号已创建，验证码已发送。请先验证当前密码，再填写邮箱验证码。' : '账号已创建。请先验证当前密码，再发送邮箱验证码。', registration.email_delivery === 'sent');
    }
  });
  // 模式切换后重新应用隐藏字段的 disabled / required。
  for (const label of $('#auth-form').querySelectorAll('label')) {
    const input = label.querySelector('input');
    if (label.hidden && input) input.disabled = true;
  }
});
$('#auth-switch').addEventListener('click', () => setAuthMode(authMode === 'login' ? 'register' : 'login'));
$('#auth-forgot').addEventListener('click', () => setAuthMode('forgot'));
$('#auth-reset').addEventListener('click', () => setAuthMode('reset'));
function deviceName(agent: string) {
  const browser = /Edg\//.test(agent) ? 'Edge' : /OPR\//.test(agent) ? 'Opera' : /Firefox\/|FxiOS\//.test(agent) ? 'Firefox'
    : /Chrome\/|CriOS\//.test(agent) ? 'Chrome' : /Safari\//.test(agent) ? 'Safari' : '未知浏览器';
  const system = /iPhone|iPad|iPod/.test(agent) ? 'iOS' : /Android/.test(agent) ? 'Android'
    : /Windows/.test(agent) ? 'Windows' : /Macintosh|Mac OS X/.test(agent) ? 'macOS' : /Linux/.test(agent) ? 'Linux' : '未知系统';
  return `${browser} (${system})`;
}
function closeDeviceMenus() {
  for (const menu of document.querySelectorAll<HTMLElement>('.device-menu:popover-open')) menu.hidePopover();
}
function renderDevices() {
  closeDeviceMenus();
  const list = $('#device-list');
  list.replaceChildren();
  const items = [...deviceSessions].sort((a, b) => Number(b.current) - Number(a.current) || b.last_active_at - a.last_active_at);
  for (const device of items) {
    const row = document.createElement('tr'); row.className = `device-row${device.current ? ' is-current' : ''}`;
    const details = document.createElement('td'); details.className = 'device-details';
    const name = document.createElement('div'); name.className = 'device-name';
    const label = document.createElement('span'); label.className = 'device-label'; label.textContent = deviceName(device.user_agent); label.title = label.textContent;
    name.append(label);
    if (device.current) {
      const badge = document.createElement('span'); badge.className = 'current-device'; badge.textContent = '当前设备'; name.append(badge);
    }
    details.append(name);
    const times = ([['登录时间', device.created_at], ['最近活动', device.last_active_at]] as const).map(([title, value]) => {
      const cell = document.createElement('td'); cell.className = 'device-time'; cell.dataset.label = title;
      const time = document.createElement('time'); time.dateTime = new Date(value).toISOString();
      time.textContent = new Date(value).toLocaleString('zh-CN', { year: 'numeric', month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit', hour12: false });
      cell.append(time); return cell;
    });
    const actions = document.createElement('td'); actions.className = 'device-actions';
    const menu = document.createElement('div'); menu.className = 'device-menu'; menu.popover = 'auto'; menu.id = `device-menu-${device.id}`;
    const trigger = document.createElement('button'); trigger.type = 'button'; trigger.className = 'icon-button device-menu-trigger'; trigger.textContent = '⋯';
    trigger.setAttribute('aria-label', `管理${device.current ? '当前设备' : deviceName(device.user_agent)}`);
    trigger.setAttribute('aria-expanded', 'false'); trigger.setAttribute('aria-controls', menu.id); trigger.popoverTargetElement = menu;
    menu.addEventListener('toggle', () => {
      trigger.setAttribute('aria-expanded', String(menu.matches(':popover-open')));
    });
    menu.addEventListener('beforetoggle', (event) => {
      if ((event as ToggleEvent).newState !== 'open') return;
      const rect = trigger.getBoundingClientRect();
      const above = innerHeight - rect.bottom < 80;
      menu.style.right = `${Math.max(8, innerWidth - rect.right)}px`;
      menu.style.top = above ? 'auto' : `${rect.bottom + 6}px`;
      menu.style.bottom = above ? `${innerHeight - rect.top + 6}px` : 'auto';
    });
    const button = document.createElement('button'); button.type = 'button'; button.textContent = '退出登录';
    button.setAttribute('aria-label', `退出${device.current ? '当前设备' : deviceName(device.user_agent)}`);
    button.addEventListener('click', () => {
      menu.hidePopover();
      selectedDevice = device;
      $('#logout-device-description').textContent = device.current ? '退出当前设备后，需要重新登录。其他设备保持登录。' : `退出 ${deviceName(device.user_agent)} 后，该设备需要重新登录。`;
      showAccountView('logout-device');
    });
    menu.append(button); actions.append(trigger, menu);
    row.append(details, ...times, actions); list.append(row);
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
    feedback($('#devices-status'), error instanceof TypeError ? '无法加载设备，请重试。' : errorMessage(error));
    $('#devices-more').hidden = false;
    $('#devices-more').textContent = '重试';
  } finally {
    if (version === accountViewVersion) { devicesLoading = false; $('#devices-more').disabled = false; }
  }
}
function isAccountView(view: string | undefined): view is AccountView {
  return view !== undefined && ['home', 'username', 'password', 'email', 'email-edit', 'devices', 'logout-all', 'logout-device'].includes(view);
}
function focusAccountView(view: AccountView) {
  const target = view === 'username' ? $('#username-form input') : view === 'password' ? $('#password-form input') : view === 'email' ? $('#email-verify-form input') : view === 'email-edit' ? $('#email-form input')
    : view === 'logout-all' ? $('#cancel-logout-all') : view === 'logout-device' ? $('#cancel-logout-device') : $('#account-title');
  target.focus({ preventScroll: true });
}
function showAccountView(view: AccountView, focus = true) {
  closeDeviceMenus();
  const titles = { home: '账号设置', username: '修改用户名', password: '修改密码', email: '验证当前密码', 'email-edit': '邮箱', devices: '已登录设备', 'logout-all': '退出全部设备', 'logout-device': '退出设备' };
  if (!titles[view]) return;
  if (view === 'email-edit' && Date.now() >= emailVerifiedUntil) view = 'email';
  const continuingEmail = ['email', 'email-edit'].includes(accountView) && ['email', 'email-edit'].includes(view);
  clearTimeout(emailVerificationTimer);
  if (view !== 'email-edit') emailVerifiedUntil = 0;
  $('#email-verify-form').reset();
  if (view === 'logout-all') logoutAllReturnView = accountView === 'devices' ? 'devices' : 'home';
  accountView = view;
  $('#account-page').dataset.view = view;
  $('#devices-logout-all').hidden = view !== 'devices';
  ++accountViewVersion; devicesLoading = false;
  $('#password-form').reset();
  $('#username-form').reset();
  if (currentUser) formInput($('#username-form'), 'username').value = currentUser.username;
  if (!continuingEmail) $('#email-form').reset();
  for (const panel of document.querySelectorAll<HTMLElement>('[data-account-panel]')) panel.hidden = panel.dataset.accountPanel !== view;
  $('#account-title').textContent = titles[view];
  $('#account-back').hidden = view === 'home';
  feedback($('#account-status'), '');
  $('#account-page').scrollTop = 0;
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
function updateAccountUser(user: User) {
  currentUser = user;
  $('#account-name').textContent = user.username;
  $('#account-avatar').textContent = user.username.charAt(0).toUpperCase();
  $('#open-account').setAttribute('aria-label', `${user.username}，账号菜单`);
  $('#username-overview').textContent = user.username;
  $('#email-overview').textContent = user.email || '尚未绑定';
  $('#email-overview').title = user.email || '';
  $('#email-summary').textContent = user.email ? `已绑定 ${user.email}` : '绑定后可用邮箱登录和找回密码。';
  $('#email-address-label').textContent = user.email ? '新邮箱' : '邮箱地址';
  $('#email-unbind').hidden = !user.email;
}
function closeAccountMenu() {
  if ($('#account-menu').matches(':popover-open')) $('#account-menu').hidePopover();
}
function openAccount(view: AccountView = 'home') {
  if (!currentUser || $('#account-page').dataset.busy) return;
  closeAccountMenu();
  updateAccountUser(currentUser);
  closeSidebar();
  workspace.hidden = true;
  $('#account-page').hidden = false;
  showAccountView(view);
}
function closeAccount(focus = true) {
  closeAccountMenu();
  $('#account-page').hidden = true;
  workspace.hidden = false;
  showAccountView('home', false);
  deviceSessions = []; selectedDevice = null; $('#device-list').replaceChildren();
  if (focus) (prompt.disabled ? $('#open-sidebar') : prompt).focus({ preventScroll: true });
}
$('#account-menu').addEventListener('beforetoggle', (event) => {
  if ((event as ToggleEvent).newState !== 'open') return;
  const rect = $('#open-account').getBoundingClientRect();
  const menu = $('#account-menu');
  menu.style.left = `${Math.max(8, rect.left)}px`;
  menu.style.bottom = `${innerHeight - rect.top + 8}px`;
  menu.style.width = `${Math.min(280, innerWidth - 16)}px`;
  menu.style.maxHeight = `${Math.max(100, rect.top - 16)}px`;
});
$('#account-menu').addEventListener('toggle', () => {
  $('#open-account').setAttribute('aria-expanded', String($('#account-menu').matches(':popover-open')));
});
$('#account-menu').addEventListener('keydown', (event) => {
  if (!['ArrowDown', 'ArrowUp', 'Home', 'End'].includes(event.key)) return;
  event.preventDefault();
  const buttons = [...$('#account-menu').querySelectorAll('button')];
  const index = buttons.findIndex(button => button === document.activeElement);
  const next = event.key === 'Home' ? 0 : event.key === 'End' ? buttons.length - 1
    : (index + (event.key === 'ArrowDown' ? 1 : -1) + buttons.length) % buttons.length;
  buttons[next]?.focus();
});
for (const button of document.querySelectorAll<HTMLButtonElement>('[data-account-view]')) {
  button.addEventListener('click', () => {
    if (!isAccountView(button.dataset.accountView)) return;
    if (button.closest('#account-menu')) openAccount(button.dataset.accountView);
    else showAccountView(button.dataset.accountView);
  });
}
$('#devices-more').addEventListener('click', () => loadDevices(true));
$('#account-page').addEventListener('scroll', closeDeviceMenus);
window.addEventListener('resize', () => { closeDeviceMenus(); closeAccountMenu(); });
$('#account-back').addEventListener('click', () => showAccountView(accountView === 'logout-device' ? 'devices' : accountView === 'email-edit' ? 'email' : accountView === 'logout-all' ? logoutAllReturnView : 'home'));
$('#cancel-logout-all').addEventListener('click', () => showAccountView(logoutAllReturnView));
$('#cancel-logout-device').addEventListener('click', () => showAccountView('devices'));
$('#close-account').addEventListener('click', () => closeAccount());
document.addEventListener('keydown', (event) => {
  if (event.key === 'Escape' && !$('#account-page').hidden && !$('#account-page').dataset.busy && !document.querySelector(':popover-open')) closeAccount();
});
async function logout() {
  openAccount();
  await withForm($('#logout-all-form'), $('#account-status'), async () => { await api('/api/auth/logout', {}); signedOut('已退出当前设备。'); });
}
$('#logout').addEventListener('click', logout);
$('#menu-logout').addEventListener('click', logout);
$('#username-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const form = $('#username-form');
  const username = formInput(form, 'username').value.trim();
  await withForm(form, $('#account-status'), async () => {
    const data = await api('/api/auth/username', { username });
    updateAccountUser(data.user);
    formInput(form, 'username').value = data.user.username;
    feedback($('#account-status'), '用户名已更新，下次登录请使用新用户名。', true);
  });
});
$('#logout-all-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  await withForm($('#logout-all-form'), $('#account-status'), async () => {
    await api('/api/auth/logout/all', {});
    signedOut('已退出全部设备，请重新登录。');
  });
});
$('#logout-device-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const device = selectedDevice;
  if (!device) return;
  await withForm($('#logout-device-form'), $('#account-status'), async () => {
    await api('/api/auth/sessions/revoke', { session_id: device.id });
    if (device.current) signedOut('已退出当前设备。');
    else { showAccountView('devices'); feedback($('#account-status'), '已退出该设备。', true); }
  });
});
$('#password-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const form = $('#password-form');
  const values = { old_password: formInput(form, 'old_password').value, new_password: formInput(form, 'new_password').value, confirm: formInput(form, 'confirm').value };
  if (values.new_password !== values.confirm) { feedback($('#account-status'), '两次输入的密码不一致。'); return; }
  await withForm(form, $('#account-status'), async () => {
    await api('/api/auth/password', { old_password: values.old_password, new_password: values.new_password });
    signedOut('密码已更新，请重新登录。');
  });
});
$('#email-verify-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const form = $('#email-verify-form');
  const password = formInput(form, 'password').value;
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
  if (!requireEmailVerification() || !formInput(form, 'email').reportValidity()) return;
  await withForm(form, $('#account-status'), async () => {
    await api('/api/auth/email/code', { email: formInput(form, 'email').value });
    feedback($('#account-status'), '验证码已发送，请查收邮件。', true);
  });
});
$('#email-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  if (!requireEmailVerification()) return;
  const form = $('#email-form');
  const body = { email: formInput(form, 'email').value, code: formInput(form, 'code').value };
  await withForm(form, $('#account-status'), async () => {
    const data = await api('/api/auth/email/bind', body);
    updateAccountUser(data.user); showAccountView('home', false); feedback($('#account-status'), '邮箱已验证并绑定。', true);
  });
});
$('#email-unbind').addEventListener('click', async () => {
  if (!requireEmailVerification()) return;
  await withForm($('#email-form'), $('#account-status'), async () => {
    const data = await api('/api/auth/email/unbind', {});
    updateAccountUser(data.user); showAccountView('home', false); feedback($('#account-status'), '邮箱已解绑。', true);
  });
});
$('#more-history').addEventListener('click', () => loadHistory(true));
$('#rename-conversation').addEventListener('click', async () => {
  const conversation = currentConversation();
  if (!conversation) return;
  const title = window.prompt('对话标题', conversation.title)?.trim();
  if (!title) return;
  try { await api('/api/conversation/rename', { conversation_id: conversation.id, title }); conversation.title = title; render(); }
  catch (error) { formError = errorMessage(error); updateComposer(); }
});

function currentConversation() {
  return conversations.find((item) => item.id === activeId);
}

function makeId() {
  const bytes = new Uint32Array(2);
  crypto.getRandomValues(bytes);
  return `${Date.now().toString(36)}-${Array.from(bytes, value => value.toString(36)).join('')}`;
}

function closeSidebar() {
  closeAccountMenu();
  shell.classList.remove('sidebar-open');
  $('#sidebar-overlay').hidden = true;
  $('#sidebar').inert = window.innerWidth <= 720;
  $('#open-sidebar').setAttribute('aria-expanded', String(!shell.classList.contains('sidebar-collapsed') && window.innerWidth > 720));
}

function conversationIdFromUrl() {
  return /^\/c\/(\d+)$/.exec(location.pathname)?.[1]
    || /^#conversation-(\d+)$/.exec(location.hash)?.[1] || null;
}

function updateConversationUrl(id: string | null, replace = false) {
  const path = id ? `/c/${id}` : '/';
  if (`${location.pathname}${location.search}${location.hash}` === path) return;
  if (replace) history.replaceState(null, '', path);
  else history.pushState(null, '', path);
}

function forgetConversation(id: string, message = '') {
  pending.get(id)?.controller.abort();
  pending.delete(id);
  pendingSends.delete(id);
  pendingRegenerates.delete(id);
  failures.delete(id);
  notices.delete(id);
  drafts.delete(id);
  conversationErrors.delete(id);
  clearTimeout(polling.get(id));
  polling.delete(id);
  conversations = conversations.filter((conversation) => conversation.id !== id);

  if (activeId === id) {
    activeId = null;
    conversationLoad = null;
    updateConversationUrl(null, true);
    prompt.value = drafts.get('new') || '';
    formError = message;
  }
  render();
  resizePrompt();
}

async function switchConversation(id: string | null, replace = false) {
  if (!$('#account-page').hidden) closeAccount(false);
  drafts.set(activeId || 'new', prompt.value);
  activeId = id;
  const load = id && !pending.has(id) ? { id } : null;
  conversationLoad = load;
  formError = '';
  prompt.value = drafts.get(id || 'new') || '';
  updateConversationUrl(id, replace);
  closeSidebar();
  render();
  resizePrompt();
  scrollToEnd();
  if (load) {
    const conversation = currentConversation();
    try {
      if (conversation) await refreshConversation(conversation);
      else {
        const data = await api(`/api/conversation?id=${load.id}`);
        if (conversationLoad !== load) return;
        const existing = currentConversation();
        const target: Conversation = existing ?? { ...data.conversation, messages: [], loaded: false };
        if (!existing) conversations.push(target);
        applyConversation(target, data);
      }
    } catch (error) {
      if (conversationLoad !== load) return;
      if (error instanceof ApiError && error.status === 404) {
        forgetConversation(load.id, '对话不存在或已删除。');
        return;
      }
      if (currentUser && errorCode(error) !== 'session_changed') {
        recordConversationError(load.id, error);
        if (conversation && isRetryableConversationError(error)) scheduleConversationPoll(conversation, 2000);
      }
    }
    if (conversationLoad !== load) return;
    conversationLoad = null;
    const follow = isNearEnd();
    render();
    if (follow) scrollToEnd();
  }
  if (!prompt.disabled && (document.activeElement === document.body || document.activeElement === prompt)) {
    prompt.focus({ preventScroll: true });
  }
}

function newConversation() {
  ++newDraftVersion;
  drafts.delete('new');
  createError = '';
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
      catch (error) { formError = errorMessage(error); updateComposer(); return; }
      forgetConversation(conversation.id);
    });
    row.append(link, remove);
    list.append(row);
  }
  $('#history-empty').hidden = conversations.length > 0;
}

function renderContent(parent: HTMLElement, text: string) {
  if (window.ChatMarkdown) window.ChatMarkdown.renderInto(parent, text);
  else parent.textContent = text;
}

function reasoningBlock(owner: Message | PendingReply, text: string, state: 'thinking' | 'interrupted' | 'done') {
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

function latestQuestionId(conversation: Conversation): string | undefined {
  const last = conversation.messages.at(-1);
  const question = last?.role === 'assistant' ? conversation.messages.at(-2) : last;
  return question?.role === 'user' ? question.id : undefined;
}

function matchingPendingRegenerate(conversation: Conversation): PendingRegenerate | undefined {
  const request = pendingRegenerates.get(conversation.id);
  return request && request.questionId === latestQuestionId(conversation) ? request : undefined;
}

function conversationBusy(conversation: Conversation) {
  return !conversation.loaded || conversationLoad?.id === conversation.id
    || pending.has(conversation.id) || selectingBranches.has(conversation.id) || isGenerating(conversation);
}

async function selectReply(conversation: Conversation, messageId: string, previousMessageId?: string) {
  if (conversationBusy(conversation)) return;
  const article = [...messagesElement.querySelectorAll<HTMLElement>('[data-message-id]')]
    .find(element => element.dataset.messageId === previousMessageId);
  const top = article?.getBoundingClientRect().top;
  selectingBranches.add(conversation.id);
  formError = '';
  renderMessages(); updateComposer();
  try {
    const data = await api('/api/conversation/select', { conversation_id: conversation.id, message_id: messageId });
    if (!conversations.includes(conversation)) return;
    applyConversation(conversation, data);
    notices.delete(conversation.id);
  } catch (error) {
    if (currentUser && conversations.includes(conversation) && errorCode(error) !== 'session_changed') {
      formError = error instanceof TypeError ? '无法切换回复版本，请重试。' : errorMessage(error);
      if (errorCode(error) === 'conflict') {
        try { await refreshConversation(conversation); } catch { /* 保留当前显示，等待下次刷新。 */ }
      }
    }
  } finally {
    selectingBranches.delete(conversation.id);
    if (currentUser && conversations.includes(conversation) && activeId === conversation.id) {
      render();
      const selected = [...messagesElement.querySelectorAll<HTMLElement>('[data-message-id]')]
        .find(element => element.dataset.messageId === messageId);
      if (selected && top !== undefined) scrollArea.scrollTop += selected.getBoundingClientRect().top - top;
      selected?.querySelector<HTMLElement>('.reply-version-count')?.focus({ preventScroll: true });
    }
  }
}

function replyVersionPicker(conversation: Conversation, message: Message) {
  const versions = message.versions;
  if (!versions || versions.total < 2) return null;
  const picker = document.createElement('div');
  picker.className = 'reply-versions';
  picker.setAttribute('role', 'group');
  picker.setAttribute('aria-label', '回复版本');
  const count = document.createElement('span');
  count.className = 'reply-version-count'; count.tabIndex = -1;
  count.textContent = `${versions.index}/${versions.total}`;
  count.setAttribute('aria-label', `第 ${versions.index} 个回复，共 ${versions.total} 个`);
  for (const direction of ['previous', 'next'] as const) {
    const targetId = direction === 'previous' ? versions.previous_id : versions.next_id;
    const button = document.createElement('button');
    button.type = 'button'; button.className = `icon-button reply-version-${direction}`;
    button.title = direction === 'previous' ? '上一版回复' : '下一版回复';
    button.setAttribute('aria-label', button.title);
    button.disabled = !targetId || conversationBusy(conversation);
    button.append(icon('chevron-down'));
    button.addEventListener('click', () => { if (targetId) selectReply(conversation, targetId, message.id); });
    if (direction === 'previous') picker.append(button, count);
    else picker.append(button);
  }
  return picker;
}

function createMessageView(conversation: Conversation, message: Message) {
  const article = document.createElement('article');
  article.className = `message ${message.role}`;
  if (message.id) article.dataset.messageId = message.id;
  article.setAttribute('aria-label', message.role === 'user' ? '你' : 'AI 的回复');
  if (message.role === 'user') article.textContent = message.content;
  else {
    const content = document.createElement('div');
    content.className = 'message-content';
    renderContent(content, message.content);
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
    const regenerate = document.createElement('button');
    regenerate.className = 'regenerate-button';
    const emptyFailure = message.state === 'failed' && !message.content && !message.reasoning;
    regenerate.title = emptyFailure ? '重新生成' : '重新生成这条回复';
    regenerate.setAttribute('aria-label', regenerate.title);
    regenerate.append(icon('retry'));
    if (emptyFailure) {
      regenerate.classList.add('regenerate-empty');
      regenerate.append('重新生成');
    }
    regenerate.addEventListener('click', () => regenerateReply(conversation));
    actions.append(regenerate);
    const versions = replyVersionPicker(conversation, message);
    if (versions) actions.append(versions);
    const interrupted = message.state !== 'generating' && message.interrupted;
    if (message.reasoning) {
      const state = message.state === 'generating' && !message.content ? 'thinking'
        : interrupted && !message.content ? 'interrupted' : 'done';
      article.append(reasoningBlock(message, message.reasoning, state));
    }
    if (message.content) article.append(content);
    if (message.error || message.truncated || interrupted) {
      const note = document.createElement('p');
      note.className = 'truncated-note';
      note.textContent = message.error || (interrupted
        ? message.state ? '生成已中断，以上是已收到的内容。' : '接收已中断，以上是已收到的内容。'
        : '回复达到长度限制，可以发送“请继续”。');
      article.append(note);
    }
    article.append(actions);
  }
  return article;
}

function renderMessages() {
  const conversation = currentConversation();
  const articles: HTMLElement[] = [];
  if (conversation) {
    const busy = conversationBusy(conversation);
    const last = conversation.messages.at(-1);
    for (const message of conversation.messages) {
      let article = messageViews.get(message);
      if (!article) {
        article = createMessageView(conversation, message);
        messageViews.set(message, article);
      }
      if (message.role === 'assistant') {
        const regenerate = article.querySelector<HTMLButtonElement>('.regenerate-button')!;
        regenerate.hidden = message !== last;
        regenerate.disabled = busy;
        if (message.versions && message.versions.total > 1) {
          article.querySelector<HTMLButtonElement>('.reply-version-previous')!.disabled = busy || !message.versions.previous_id;
          article.querySelector<HTMLButtonElement>('.reply-version-next')!.disabled = busy || !message.versions.next_id;
        }
      }
      articles.push(article);
    }
  }
  const visible = new Set(articles);
  for (const child of Array.from(messagesElement.children)) {
    if (child instanceof HTMLElement && child !== streamElement && !visible.has(child)) child.remove();
  }
  let next = messagesElement.firstElementChild;
  for (const article of articles) {
    if (article !== next) messagesElement.insertBefore(article, next);
    next = article.nextElementSibling;
  }
  renderPendingReply();
}

function renderPendingReply() {
  streamElement?.remove();
  streamElement = null;
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
    streamElement = article;
  } else if (flight) {
    const waiting = document.createElement('div');
    waiting.className = 'pending';
    const dot = document.createElement('span');
    dot.className = 'pending-dot';
    waiting.append(dot, document.createTextNode(!flight.assistantId && !flight.regenerate ? '正在发送并等待回复…' : '正在等待回复…'));
    streamElement = waiting;
  }
  if (streamElement) messagesElement.append(streamElement);
}

function updateComposer() {
  const conversation = currentConversation();
  const creating = activeId === null && creatingConversation !== null;
  const loading = conversationLoad !== null;
  const detailError = activeId ? conversationErrors.get(activeId) : undefined;
  const unloaded = activeId !== null && !conversation?.loaded;
  const waiting = pending.has(activeId);
  const flight = pending.get(activeId);
  const thinking = flight?.reasoning && !flight.text;
  const saving = Boolean(flight && !flight.assistantId && !flight.regenerate);
  const unanswered = conversation?.messages.at(-1)?.role === 'user';
  const remote = isGenerating(conversation);
  const selecting = Boolean(conversation && selectingBranches.has(conversation.id));
  const activeUnsent = matchingPendingSend(activeId);
  const uncertainSend = activeUnsent?.rejected ? undefined : activeUnsent;
  const activeRegenerate = !activeUnsent && conversation ? matchingPendingRegenerate(conversation) : undefined;
  prompt.disabled = !currentUser || loading || unloaded || creating || waiting || remote || selecting || Boolean(unanswered);
  prompt.placeholder = loading || unloaded ? '等待对话加载' : thinking ? '正在思考…' : creating ? '正在创建对话…' : saving ? '正在发送…' : waiting ? '正在等待回复…' : unanswered ? '重试上一条消息，或新建对话' : '有什么我可以帮你？';
  const chosen = selectedModel();
  sendButton.hidden = waiting;
  stopButton.hidden = !waiting;
  stopButton.disabled = Boolean(flight?.stopped || flight?.finished);
  sendButton.disabled = prompt.disabled || !prompt.value.trim() || !chosen || Boolean(uncertainSend);
  modelSelect.disabled = loading || unloaded || creating || selecting || !models.length;
  if (modelSelect.disabled) closeModelMenu();
  messagesElement.setAttribute('aria-busy', String(waiting || remote || loading || (unloaded && !detailError)));
  const canRetryCreate = activeId === null && Boolean(createError && prompt.value.trim());
  const canRetryLoad = Boolean(detailError);
  const canRetrySaved = !activeUnsent && (unanswered || (failures.has(activeId) && Boolean(conversation?.messages.at(-1))));
  const canResendDraft = activeUnsent?.rejected && chosen && !remote;
  const canRetrySend = !remote && (uncertainSend || canResendDraft || activeRegenerate || canRetryCreate || canRetrySaved);
  retryButton.hidden = loading || creating || waiting || selecting || !(canRetryLoad || canRetrySend);
  const retryModel = canRetrySend ? (uncertainSend ?? activeRegenerate)?.model : undefined;
  retryButton.textContent = retryModel ? '重试原请求' : canRetryLoad && !canResendDraft ? '刷新对话' : '重试';
  retryButton.title = retryModel ? `使用原请求重试（${retryModel.provider} / ${retryModel.model}）` : '';
  const error = formError || activeUnsent?.error || activeRegenerate?.error || failures.get(activeId)
    || (activeId === null ? createError : detailError) || modelError || historyError;
  const notice = notices.get(activeId) || '';
  status.classList.toggle('error', Boolean(error));
  status.textContent = error || notice || (loading || unloaded ? '正在加载对话…' : '')
    || (remote && !waiting ? '回复仍在生成，历史会自动更新。' : '')
    || (flight?.stopped || flight?.finished ? '正在更新对话…' : '')
    || (creating ? '正在创建对话…' : thinking ? '正在思考，可以随时停止。' : saving ? '正在发送消息…' : waiting ? '正在生成，可以随时停止。' : '')
    || (unanswered && !waiting ? '上一条消息还未收到回复，可以重试。' : '');
}

function render() {
  const conversation = currentConversation();
  const title = conversation?.title || (activeId === null ? '新对话' : '对话');
  workspace.classList.toggle('empty', activeId === null);
  $('#conversation-title').textContent = title;
  $('#rename-conversation').hidden = !conversation?.loaded;
  document.title = `Chat · ${title}`;
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
  scrollArea.scrollTo({ top: scrollArea.scrollHeight, behavior: 'instant' });
}

function isNearEnd() {
  return scrollArea.scrollHeight - scrollArea.scrollTop - scrollArea.clientHeight < 120;
}

function renderReplyProgress() {
  const follow = isNearEnd();
  renderMessages();
  updateComposer();
  if (follow) scrollToEnd();
}

function modelKey(model: Model) { return JSON.stringify([model.provider, model.model]); }
function selectedModel(conversation = currentConversation()) {
  const choice = conversation?.provider ? conversation : defaultModel;
  return models.find((model) => choice && model.provider === choice.provider && model.model === choice.model) || null;
}
function selectedReasoningEffort(model: Model, conversation = currentConversation()): ReasoningEffort | undefined {
  const effort = conversation?.provider ? conversation.reasoningEffort : defaultReasoningEffort;
  return effort && model.reasoning_efforts?.includes(effort) ? effort : model.reasoning_efforts?.[0];
}
function syncModelSelect() {
  const chosen = selectedModel();
  const effort = chosen ? selectedReasoningEffort(chosen) : undefined;
  const label = chosen ? `${chosen.provider} / ${chosen.model}${effort ? ` · ${effort === 'max' ? 'Max' : 'High'}` : ''}` :
    models.length ? '模型不可用' : modelError ? '模型加载失败' : '正在加载模型…';
  $('#model-label').textContent = label;
  modelSelect.title = label;
  modelSelect.setAttribute('aria-label', `选择模型和思考强度：${label}`);
  const key = chosen ? modelKey(chosen) : null;
  for (const input of modelOptions.querySelectorAll<HTMLInputElement>('input')) {
    input.checked = input.value === key;
  }
  effortOptions.hidden = !effort;
  for (const input of effortInputs) {
    input.disabled = !chosen?.reasoning_efforts?.some((option) => option === input.value);
    input.checked = input.value === effort;
    input.closest('label')?.toggleAttribute('hidden', input.disabled);
  }
  positionModelMenu();
}

function closeModelMenu() {
  modelMenu.hidePopover();
  modelSelect.setAttribute('aria-expanded', 'false');
}

function openModelMenu() {
  if (modelSelect.disabled) return;
  modelMenu.showPopover({ source: modelSelect });
  modelSelect.setAttribute('aria-expanded', 'true');
  positionModelMenu();
  (modelOptions.querySelector<HTMLInputElement>('input:checked') || modelOptions.querySelector<HTMLInputElement>('input'))?.focus();
}

function positionModelMenu() {
  if (!modelMenu.matches(':popover-open')) return;
  const trigger = modelSelect.getBoundingClientRect();
  const gap = 8;
  const above = trigger.top - gap - 12;
  const below = window.innerHeight - trigger.bottom - gap - 12;
  const openAbove = above >= Math.min(modelMenu.scrollHeight, 360) || above > below;
  modelMenu.style.maxHeight = `${Math.max(0, Math.min(360, openAbove ? above : below))}px`;
  const menu = modelMenu.getBoundingClientRect();
  modelMenu.style.left = `${Math.max(12, Math.min(trigger.right - menu.width, window.innerWidth - menu.width - 12))}px`;
  modelMenu.style.top = `${openAbove ? trigger.top - gap - menu.height : trigger.bottom + gap}px`;
}

function renderModelOptions() {
  modelOptions.querySelectorAll('label').forEach((label) => label.remove());
  for (const model of models) {
    const option = document.createElement('label');
    option.className = 'model-option';
    option.title = `${model.provider} / ${model.model}`;
    const input = document.createElement('input');
    input.type = 'radio';
    input.name = 'chat-model';
    input.value = modelKey(model);
    input.setAttribute('aria-label', option.title);
    const name = document.createElement('span');
    name.className = 'model-option-name';
    name.textContent = option.title;
    option.append(input, name, icon('check'));
    input.addEventListener('change', () => {
      const conversation = currentConversation();
      if (conversation) {
        conversation.provider = model.provider;
        conversation.model = model.model;
        conversation.reasoningEffort = undefined;
      } else {
        defaultModel = model;
        defaultReasoningEffort = undefined;
      }
      syncModelSelect();
      updateComposer();
    });
    modelOptions.append(option);
  }
}
async function loadModels() {
  modelError = '';
  try {
    const data = await api('/api/models');
    if (!Array.isArray(data.models) || !data.models.length || !data.models.every((item) =>
      typeof item.provider === 'string' && typeof item.model === 'string' &&
      (item.reasoning_efforts === undefined || (Array.isArray(item.reasoning_efforts) &&
        item.reasoning_efforts.every((effort) => effort === 'high' || effort === 'max'))))) {
      throw new Error('没有可用模型，请检查后端配置。');
    }
    models = data.models;
    const defaultKey = defaultModel ? modelKey(defaultModel) : null;
    if (!models.some((model) => modelKey(model) === defaultKey)) {
      defaultModel = models[0] ?? null;
      defaultReasoningEffort = undefined;
    }
    renderModelOptions();
  } catch (error) {
    if (!currentUser || errorCode(error) === 'session_changed') return;
    modelError = error instanceof TypeError ? '无法连接后端，请确认服务已启动。' : errorMessage(error);
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
effortOptions.addEventListener('change', (event) => {
  if (!(event.target instanceof HTMLInputElement)) return;
  const effort = event.target.value;
  if (effort !== 'high' && effort !== 'max') return;
  const conversation = currentConversation();
  if (conversation) {
    const model = selectedModel(conversation);
    if (!model) return;
    conversation.provider = model.provider;
    conversation.model = model.model;
    conversation.reasoningEffort = effort;
  } else defaultReasoningEffort = effort;
  syncModelSelect();
});
modelMenu.addEventListener('keydown', (event) => {
  if (event.key === 'Escape') {
    event.preventDefault();
    closeModelMenu();
    modelSelect.focus();
  } else if (event.key === 'Enter' && event.target instanceof HTMLInputElement) {
    event.preventDefault();
    event.target.click();
  }
});
modelMenu.addEventListener('focusout', (event) => {
  if (event.relatedTarget instanceof Node && !modelMenu.contains(event.relatedTarget)) closeModelMenu();
});
window.addEventListener('resize', positionModelMenu);
$('#reload-models').addEventListener('click', loadModels);
stopButton.addEventListener('click', () => {
  const flight = pending.get(activeId);
  if (flight && !flight.stopped && !flight.finished) {
    flight.stopped = true;
    flight.controller.abort();
    updateComposer();
  }
});

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

// POST fetch 的响应体按 SSE 行解析；网络读取块不等于完整事件。
async function readEvents(response: Response, onEvent: (event: string, data: Record<string, unknown>) => boolean) {
  if (!response.headers.get('content-type')?.includes('text/event-stream') || !response.body)
    throw new Error('后端没有返回流式回复。');
  const reader = response.body.getReader();
  const decoder = new TextDecoder('utf-8', { fatal: true });
  let buffer = '', event = '', eventSize = 0;
  let data: string[] = [];
  function line(text: string): boolean {
    if (!text) {
      if (data.length) {
        const payload: unknown = JSON.parse(data.join('\n'));
        if (!isRecord(payload)) throw new Error('回复事件格式异常。');
        const stop = onEvent(event || 'message', payload);
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
      if (done) throw new ApiError('连接意外中断，回复未完整接收。', 'stream_incomplete');
    }
  } finally { await reader.cancel().catch(() => { }); reader.releaseLock(); }
}
function paintStream(id: string) {
  if (activeId !== id || streamFrame !== null) return;
  streamFrame = requestAnimationFrame(() => {
    streamFrame = null;
    const follow = isNearEnd();
    renderPendingReply();
    updateComposer();
    if (follow) scrollToEnd();
  });
}

function validateChatContext(messages: readonly Pick<Message, 'content'>[]) {
  const encoder = new TextEncoder();
  const bytes = messages.reduce((size, message) => size + encoder.encode(message.content).byteLength, 0);
  if (messages.length <= 200 && bytes <= 240 * 1024) return true;
  formError = '对话内容过长，请新建对话继续。';
  updateComposer();
  return false;
}

function regenerateReply(conversation: Conversation) {
  const questionId = latestQuestionId(conversation);
  if (!questionId) return;
  const messages = conversation.messages;
  const last = messages.at(-1);
  const previousAnswer = last?.role === 'assistant' ? last : undefined;
  const context = previousAnswer ? messages.slice(0, -1) : messages;
  if (!validateChatContext(context)) return;
  const model = selectedModel(conversation);
  if (!model) {
    formError = '当前模型不可用，请切换模型后重试。';
    updateComposer();
    return;
  }
  return requestReply(conversation, {
    questionId, previousAssistantId: previousAnswer?.id, model,
    reasoningEffort: selectedReasoningEffort(model, conversation), requestId: makeId(), error: '',
  });
}

function reconcileUnconfirmedReply(
  conversation: Conversation, request: PendingSend | PendingRegenerate, error: ApiError,
  refreshed: boolean, retry: boolean,
) {
  const { id } = conversation;
  // 现有接口共用 unavailable 和 conflict，只有这两种响应指向已登记的请求。
  const queueRejected = error.status === 503 && error.code === 'unavailable'
    && error.message === '模型任务队列已满，请稍后重试。';
  const existingRequest = retry && error.status === 409 && error.code === 'conflict'
    && error.message === '该请求已处理或正在生成，请刷新历史后重试。';
  const recorded = queueRejected || existingRequest;
  const rejected = !retry && error.status !== undefined && error.status >= 400 && error.status < 500;
  if (!recorded && !rejected) return;
  if (recorded && (!refreshed || isGenerating(conversation))) return;

  const answer = conversation.messages.at(-1);
  if ('content' in request) {
    const question = conversation.messages.at(-2);
    const completedDuringRetry = existingRequest && answer?.state === 'completed';
    const saved = refreshed && question?.role === 'user' && question.id && question.content === request.content
      && question.parent_id === request.parentId && answer?.role === 'assistant' && answer.parent_id === question.id
      && (completedDuringRetry || answer.state === 'failed' || answer.state === 'interrupted');
    if (saved) {
      clearConfirmedSend(id, request);
      if (!completedDuringRetry) {
        failures.set(id, existingRequest ? answer.error || '生成已结束，请重新生成。' : request.error);
      }
    } else if (recorded) {
      pendingSends.delete(id);
      notices.set(id, '原请求已结束，请在历史中查看。');
    } else {
      request.rejected = true;
    }
  } else {
    pendingRegenerates.delete(id);
    const completedDuringRetry = existingRequest && answer?.role === 'assistant' && answer.state === 'completed'
      && answer.id !== request.previousAssistantId;
    if (!completedDuringRetry && latestQuestionId(conversation) === request.questionId) {
      failures.set(id, existingRequest ? '生成已结束，请重新生成。' : request.error);
    }
  }
}

async function requestReply(conversation: Conversation, request: PendingSend | PendingRegenerate, retry = false) {
  const id = conversation.id;
  if (conversationBusy(conversation)) return;
  request.error = '';
  if ('content' in request) pendingSends.set(id, request);
  else pendingRegenerates.set(id, request);
  const { provider, model } = request.model;
  const requestBody: ChatRequest = {
    provider, model, stream: true, conversation_id: id, request_id: request.requestId,
    reasoning_effort: request.reasoningEffort,
    ...('content' in request ? { content: request.content } : { regenerate_message_id: request.questionId }),
  };

  const epoch = authEpoch;
  const flight: PendingReply = {
    controller: new AbortController(), text: '', reasoning: '',
    regenerate: 'questionId' in request, stopped: false, finished: false,
  };
  pending.set(id, flight);
  failures.delete(id); notices.delete(id); conversationErrors.delete(id);
  if (activeId === id) formError = '';
  if (!retry) {
    conversation.provider = provider;
    conversation.model = model;
    conversation.reasoningEffort = request.reasoningEffort;
  }

  if (activeId === id) {
    render();
    scrollToEnd();
  } else renderList();
  let savedFlightError = '';
  let responseError: ApiError | undefined;
  try {
    const response = await fetch(`${apiBase}/api/chat`, {
      method: 'POST',
      credentials: 'include',
      headers: { 'Content-Type': 'application/json', 'X-Chat-Request': '1' },
      body: JSON.stringify(requestBody),
      signal: flight.controller.signal,
    });
    if (epoch !== authEpoch) throw new ApiError('账号已切换。', 'session_changed', response.status);
    if (!response.ok) {
      const data = errorPayload(await response.json().catch(() => null));
      if (epoch !== authEpoch) throw new ApiError('账号已切换。', 'session_changed', response.status);
      if (data.code === 'unauthenticated') signedOut('登录已过期，请重新登录。');
      responseError = new ApiError(data.error || '请求失败，请重试。', data.code || 'request_failed', response.status);
      throw responseError;
    }
    await readEvents(response, (event, data) => {
      if (epoch !== authEpoch || !conversations.includes(conversation)) return true;
      if (event === 'meta') {
        if (typeof data.user_message_id !== 'string' || typeof data.assistant_message_id !== 'string') throw new Error('回复消息 ID 异常。');
        flight.assistantId = data.assistant_message_id;
        if ('content' in request) {
          clearConfirmedSend(id, request);
          if (!retry && !conversation.messages.some((message) => message.id === data.user_message_id)) {
            conversation.messages.push({ id: data.user_message_id, role: 'user', state: 'completed', content: request.content });
          }
        } else if (pendingRegenerates.get(id) === request) {
          pendingRegenerates.delete(id);
        }
        if (activeId === id) renderReplyProgress();
      }
      if (event === 'error') {
        throw new ApiError(
          typeof data.error === 'string' ? data.error : '生成失败，请重试。',
          typeof data.code === 'string' ? data.code : 'generation_failed',
        );
      }
      if (event === 'delta') {
        if (typeof data.text !== 'string') throw new Error('回复格式异常。');
        flight.text += data.text; paintStream(id);
      }
      if (event === 'reasoning_delta') {
        if (typeof data.text !== 'string') throw new Error('思考内容格式异常。');
        flight.reasoning += data.text; paintStream(id);
      }
      if (event === 'done') {
        if (typeof data.finish_reason !== 'string' || !['stop', 'length', 'refusal', 'other'].includes(data.finish_reason)) throw new Error('回复结束状态异常。');
        if (!flight.assistantId || data.assistant_message_id !== flight.assistantId) throw new Error('回复消息 ID 异常。');
        if (!flight.text.trim() && data.finish_reason !== 'refusal') throw new Error('没有收到有效回复，请重试。');
        flight.finished = true;
        // 同一请求的回放可能来自其他分支，当前分支由历史接口更新。
        if (!retry) {
          const reply: Message = { id: flight.assistantId, state: 'completed', role: 'assistant', content: flight.text || '模型拒绝回答此问题。', reasoning: flight.reasoning, truncated: data.truncated === true };
          if (flight.regenerate && conversation.messages.at(-1)?.role === 'assistant') conversation.messages[conversation.messages.length - 1] = reply;
          else conversation.messages.push(reply);
        }
        if (activeId === id) updateComposer();
        return true;
      }
      return false;
    });
  } catch (error) {
    if (epoch !== authEpoch || !currentUser || !conversations.includes(conversation)) return;
    if (!flight.stopped) {
      if (flight.assistantId) savedFlightError = error instanceof TypeError ? '连接已断开，请刷新对话确认回复状态。' : errorMessage(error);
      else request.error = unsavedFailureMessage(error);
    }
  } finally {
    if (epoch !== authEpoch || !currentUser || !conversations.includes(conversation)) return;
    let refreshed = false;
    try { refreshed = (await refreshConversation(conversation)) === true; }
    catch (error) {
      if (epoch !== authEpoch || !currentUser || !conversations.includes(conversation)) return;
      recordConversationError(id, error);
      if (!retry && !flight.regenerate && !flight.finished && flight.assistantId && (flight.text || flight.reasoning)) {
        conversation.messages.push({ role: 'assistant', content: flight.text, reasoning: flight.reasoning, interrupted: true });
      }
    }
    if (epoch !== authEpoch || !currentUser || !conversations.includes(conversation)) return;
    if (pending.get(id) === flight) pending.delete(id);
    if (responseError && !flight.stopped) reconcileUnconfirmedReply(conversation, request, responseError, refreshed, retry);
    scheduleConversationPoll(conversation);
    const reply = flight.assistantId ? conversation.messages.find((message) => message.id === flight.assistantId) : undefined;
    if (flight.stopped && reply?.state !== 'completed') {
      notices.set(id, flight.assistantId ? '已停止接收回复。' : '已停止接收，保存状态待确认，可使用原请求重试。');
    }
    if (savedFlightError && reply?.state !== 'completed' && !isGenerating(conversation) && !conversation.messages.at(-1)?.error) {
      failures.set(id, savedFlightError);
    }
    if (activeId === id) {
      const follow = isNearEnd();
      renderReplyProgress();
      if (follow && !prompt.disabled && document.activeElement === document.body
        && window.getSelection()?.isCollapsed && $('#account-page').hidden) {
        prompt.focus({ preventScroll: true });
      }
    }
  }
}

$('#chat-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const raw = prompt.value;
  const text = raw.trim();
  let conversation = currentConversation();
  const existing = matchingPendingSend(conversation?.id ?? null, text);
  const model = selectedModel(conversation);
  if (!text || prompt.disabled || !model || (existing && !existing.rejected) || (conversation && conversationBusy(conversation))
    || (!conversation && (activeId !== null || creatingConversation))) return;
  if (!validateChatContext([...(conversation?.messages || []), { content: text }])) return;
  const reasoningEffort = selectedReasoningEffort(model, conversation);
  const epoch = authEpoch;
  if (!conversation) {
    const creation = { draftVersion: newDraftVersion };
    creatingConversation = creation;
    formError = '';
    createError = '';
    updateComposer();
    try {
      const data = await api('/api/conversations', { title: text.replace(/\s+/g, ' ').slice(0, 28) });
      if (epoch !== authEpoch || !currentUser) return;
      conversation = { ...data.conversation, messages: [], loaded: true };
    } catch (error) {
      if (epoch === authEpoch && creation.draftVersion === newDraftVersion && errorCode(error) !== 'session_changed') {
        createError = error instanceof TypeError ? '连接失败，请确认后端服务可用。' : errorMessage(error);
      }
      return;
    } finally {
      if (creatingConversation === creation) {
        creatingConversation = null;
        updateComposer();
      }
    }
    conversations.unshift(conversation);
    if (creation.draftVersion === newDraftVersion) {
      drafts.delete('new');
      if (activeId === null) {
        activeId = conversation.id;
        updateConversationUrl(activeId);
      }
    }
  }
  const send: PendingSend = {
    content: text, raw, model, reasoningEffort, requestId: makeId(), error: '',
    parentId: conversation.messages.at(-1)?.id ?? conversation.current_message_id,
  };
  drafts.set(conversation.id, raw);
  requestReply(conversation, send);
});

prompt.addEventListener('input', () => {
  formError = '';
  if (activeId === null) createError = '';
  drafts.set(activeId || 'new', prompt.value);
  resizePrompt();
  updateComposer();
});
prompt.addEventListener('keydown', (event) => {
  if (event.key === 'Enter' && !event.shiftKey && !event.isComposing) {
    event.preventDefault();
    if (!sendButton.disabled) $('#chat-form').requestSubmit();
  }
});
retryButton.addEventListener('click', () => {
  const conversation = currentConversation();
  if (activeId !== null && !conversation?.loaded && conversationErrors.has(activeId)) {
    void switchConversation(activeId, true);
    return;
  }
  if (activeId === null || !conversation) {
    if (activeId === null && createError && prompt.value.trim()) $('#chat-form').requestSubmit();
    return;
  }
  if (!conversationBusy(conversation)) {
    const unsent = matchingPendingSend(conversation.id);
    if (unsent) {
      if (unsent.rejected) {
        $('#chat-form').requestSubmit();
        return;
      }
      unsent.raw = prompt.value;
      drafts.set(conversation.id, prompt.value);
      void requestReply(conversation, unsent, true);
      return;
    }
    const regenerate = matchingPendingRegenerate(conversation);
    if (regenerate) {
      void requestReply(conversation, regenerate, true);
      return;
    }
  }
  if (conversationErrors.has(conversation.id)) {
    void switchConversation(conversation.id, true);
    return;
  }
  void regenerateReply(conversation);
});
$('#new-chat').addEventListener('click', newConversation);
window.addEventListener('popstate', () => {
  if (currentUser) switchConversation(conversationIdFromUrl(), true);
});
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
document.addEventListener('keydown', (event) => {
  if (event.key !== 'Escape') return;
  if ($('#account-menu').matches(':popover-open')) {
    event.preventDefault();
    closeAccountMenu();
    $('#open-account').focus();
  } else closeSidebar();
});
window.addEventListener('resize', () => { closeModelMenu(); closeSidebar(); resizePrompt(); });
closeSidebar();
render();
resizePrompt();
setAuthMode('login');
api('/api/auth/me').then((data) => startSession(data.user)).catch((error) => {
  if (errorCode(error) !== 'unauthenticated') feedback($('#auth-status'), errorMessage(error) || '无法连接服务。');
});
