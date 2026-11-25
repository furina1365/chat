import { $, formInput } from './dom';

interface AdminUser {
  id: string;
  username: string;
  email: string | null;
  disabled: boolean;
  created_at: number;
  updated_at: number;
}
interface UserPage { items: AdminUser[]; next_before_id: string | null }
type Action = 'rename' | 'disable' | 'enable' | 'logout';
const loginForm = $<HTMLFormElement>('#admin-login-form');
const searchForm = $<HTMLFormElement>('#admin-search');
const actionForm = $<HTMLFormElement>('#admin-action-form');
const dialog = $<HTMLDialogElement>('#admin-dialog');
const searchInput = $<HTMLInputElement>('#user-search');
let users: AdminUser[] = [];
let nextId: string | null = null;
let search = '';
let loading = false;
let selected: { user: AdminUser; action: Action } | null = null;
let actionBusy = false;
let epoch = 0;

class RequestError extends Error {
  constructor(message: string, readonly status: number) { super(message); }
}
function message(error: unknown) { return error instanceof Error ? error.message : '请求失败，请稍后重试。'; }
function status(text = '', success = false) {
  $('#admin-status').textContent = text;
  $('#admin-status').classList.toggle('success', success);
}
async function request<T>(path: string, body?: object): Promise<T> {
  const response = await fetch(path, {
    method: body === undefined ? 'GET' : 'POST', credentials: 'same-origin',
    headers: body === undefined ? {} : { 'Content-Type': 'application/json', 'X-Chat-Request': '1' },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const data = await response.json();
  if (!response.ok) {
    if (response.status === 401 && path !== '/api/admin/login') showLogin();
    throw new RequestError(typeof data.error === 'string' ? data.error : '请求失败，请稍后重试。', response.status);
  }
  return data as T;
}
function busy(form: HTMLFormElement, value: boolean) {
  form.querySelectorAll<HTMLInputElement | HTMLButtonElement>('input, button').forEach(control => { control.disabled = value; });
}
function showLogin() {
  ++epoch;
  users = []; nextId = null; selected = null;
  $('#admin-users').replaceChildren();
  $('#admin-panel').hidden = true;
  $('#admin-initial').hidden = true;
  $('#admin-login').hidden = false;
  dialog.close();
  formInput(loginForm, 'password').focus();
}
async function showPanel() {
  ++epoch;
  $('#admin-initial').hidden = true;
  $('#admin-login').hidden = true;
  $('#admin-panel').hidden = false;
  loginForm.reset();
  search = ''; searchInput.value = '';
  await loadUsers();
}
function renderUsers() {
  const fragment = document.createDocumentFragment();
  for (const user of users) {
    const row = document.createElement('tr');
    const identity = row.insertCell();
    const name = document.createElement('span'); name.className = 'admin-user-name'; name.textContent = user.username;
    const id = document.createElement('span'); id.className = 'admin-user-id'; id.textContent = `ID ${user.id}`;
    identity.append(name, id);
    const email = row.insertCell(); email.className = 'admin-email'; email.textContent = user.email || '未绑定邮箱';
    const state = row.insertCell(); state.className = 'admin-state';
    const badge = document.createElement('span'); badge.className = `admin-badge${user.disabled ? ' disabled' : ''}`; badge.textContent = user.disabled ? '已停用' : '正常'; state.append(badge);
    const date = row.insertCell(); date.className = 'admin-date';
    date.textContent = new Date(user.created_at).toLocaleDateString('zh-CN'); date.title = new Date(user.created_at).toLocaleString('zh-CN');
    const actions = document.createElement('div'); actions.className = 'admin-row-actions';
    for (const [action, label] of [['rename', '改名'], ['logout', '强制退出'], [user.disabled ? 'enable' : 'disable', user.disabled ? '启用' : '停用']] as [Action, string][]) {
      const button = document.createElement('button'); button.type = 'button'; button.className = `text-button${action === 'disable' ? ' danger' : ''}`;
      button.textContent = label; button.setAttribute('aria-label', `${label} ${user.username}`);
      button.addEventListener('click', () => openAction(user, action)); actions.append(button);
    }
    row.insertCell().append(actions); fragment.append(row);
  }
  $('#admin-users').replaceChildren(fragment);
  $('#admin-empty').hidden = users.length > 0 || loading;
  $('#admin-count').textContent = `已显示 ${users.length} 位用户`;
  $('#admin-more').hidden = !nextId;
}
async function loadUsers(more = false) {
  if (loading) return;
  const version = epoch;
  loading = true; busy(searchForm, true); $<HTMLButtonElement>('#admin-more').disabled = true;
  status('正在加载…');
  if (!more) { users = []; nextId = null; renderUsers(); }
  try {
    const params = new URLSearchParams({ search });
    if (more && nextId) params.set('before_id', nextId);
    const data = await request<UserPage>(`/api/admin/users?${params}`);
    if (version !== epoch) return;
    users = more ? [...users, ...data.items] : data.items;
    nextId = data.next_before_id;
    status();
  } catch (error) {
    if (version === epoch) status(message(error));
    else if (error instanceof RequestError && error.status === 401) $('#login-status').textContent = error.message;
  } finally {
    loading = false; busy(searchForm, false); $<HTMLButtonElement>('#admin-more').disabled = false;
    if (version === epoch) renderUsers();
  }
}
function openAction(user: AdminUser, action: Action) {
  selected = { user, action };
  const labels = { rename: '修改用户名', disable: '停用账号', enable: '启用账号', logout: '强制退出' };
  $('#dialog-title').textContent = labels[action];
  $('#dialog-description').textContent = {
    rename: `修改 ${user.username} 的用户名。`,
    disable: `停用 ${user.username} 后，该用户将退出全部设备，无法登录。对话记录会保留。`,
    enable: `启用 ${user.username} 后，该用户可重新登录。`,
    logout: `将 ${user.username} 退出全部设备，该用户可重新登录。`,
  }[action];
  $('#dialog-username-label').hidden = action !== 'rename';
  const input = formInput(actionForm, 'username'); input.disabled = action !== 'rename'; input.required = action === 'rename'; input.value = user.username;
  $('#dialog-status').textContent = '';
  $('#dialog-submit').textContent = action === 'rename' ? '保存' : labels[action];
  dialog.showModal();
  if (action === 'rename') input.select(); else $('#dialog-cancel').focus();
}
loginForm.addEventListener('submit', async event => {
  event.preventDefault(); busy(loginForm, true); $('#login-status').textContent = '正在登录…';
  try {
    await request('/api/admin/login', { password: formInput(loginForm, 'password').value });
    $('#login-status').textContent = ''; await showPanel();
  } catch (error) { $('#login-status').textContent = message(error); }
  finally { busy(loginForm, false); }
});
searchForm.addEventListener('submit', event => { event.preventDefault(); search = searchInput.value.trim(); void loadUsers(); });
$('#admin-refresh').addEventListener('click', () => { void loadUsers(); });
$('#admin-more').addEventListener('click', () => { void loadUsers(true); });
$('#dialog-cancel').addEventListener('click', () => dialog.close());
dialog.addEventListener('cancel', event => { if (actionBusy) event.preventDefault(); });
actionForm.addEventListener('submit', async event => {
  event.preventDefault();
  if (!selected || actionBusy) return;
  const { user, action } = selected;
  const username = formInput(actionForm, 'username').value;
  const version = epoch;
  actionBusy = true; busy(actionForm, true); $('#dialog-status').textContent = '正在处理…';
  try {
    if (action === 'logout') await request('/api/admin/users/logout', { user_id: user.id });
    else {
      const data = await request<{ user: AdminUser }>('/api/admin/users/update', {
        user_id: user.id, ...(action === 'rename' ? { username } : { disabled: action === 'disable' }),
      });
      if (version !== epoch) return;
      users = users.map(item => item.id === user.id ? data.user : item);
      renderUsers();
    }
    if (version !== epoch) return;
    dialog.close(); status({ rename: '用户名已修改。', disable: '账号已停用。', enable: '账号已启用。', logout: '已退出该用户的全部设备。' }[action], true);
  } catch (error) {
    if (version === epoch) $('#dialog-status').textContent = message(error);
    else $('#login-status').textContent = message(error);
  } finally {
    actionBusy = false; busy(actionForm, false);
    formInput(actionForm, 'username').disabled = action !== 'rename';
  }
});
$('#admin-logout').addEventListener('click', async () => {
  const button = $<HTMLButtonElement>('#admin-logout'); button.disabled = true;
  try { await request('/api/admin/logout', {}); showLogin(); }
  catch (error) { status(message(error)); }
  finally { button.disabled = false; }
});
request('/api/admin/me').then(showPanel).catch(error => {
  showLogin();
  if (!(error instanceof RequestError && error.status === 401)) $('#login-status').textContent = message(error);
});
