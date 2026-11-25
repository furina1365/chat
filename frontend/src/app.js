// Generated from TypeScript by npm run build; edit src/*.ts.
"use strict";
(() => {
  var __defProp = Object.defineProperty;
  var __defNormalProp = (obj, key, value) => key in obj ? __defProp(obj, key, { enumerable: true, configurable: true, writable: true, value }) : obj[key] = value;
  var __publicField = (obj, key, value) => __defNormalProp(obj, typeof key !== "symbol" ? key + "" : key, value);

  // src/dom.ts
  function $(selector) {
    const element = document.querySelector(selector);
    if (!(element instanceof HTMLElement)) throw new Error(`\u9875\u9762\u7F3A\u5C11\u5143\u7D20\uFF1A${selector}`);
    return element;
  }
  function formInput(form, name) {
    const input = form.elements.namedItem(name);
    if (!(input instanceof HTMLInputElement)) throw new Error(`\u8868\u5355\u7F3A\u5C11\u8F93\u5165\u6846\uFF1A${name}`);
    return input;
  }

  // src/app.ts
  var host = window.location.hostname || "localhost";
  var apiBase = location.protocol !== "file:" && location.port !== "8000" ? location.origin : `http://${host.includes(":") ? `[${host.replace(/^\[|\]$/g, "")}]` : host}:53411`;
  var shell = $(".app-shell");
  var workspace = $("#workspace");
  var list = $("#conversation-list");
  var messagesElement = $("#messages");
  var prompt = $("#prompt");
  var sendButton = $("#send");
  var status = $("#status");
  var retryButton = $("#retry");
  var modelSelect = $("#model-select");
  var modelMenu = $("#model-menu");
  var stopButton = $("#stop");
  var models = [];
  var modelError = "";
  var defaultModel = null;
  var streamFrame = null;
  var scrollArea = $("#conversation-scroll");
  var pending = /* @__PURE__ */ new Map();
  var selectingBranches = /* @__PURE__ */ new Set();
  var failures = /* @__PURE__ */ new Map();
  var notices = /* @__PURE__ */ new Map();
  var drafts = /* @__PURE__ */ new Map();
  var messageContentCache = /* @__PURE__ */ new WeakMap();
  var reasoningViews = /* @__PURE__ */ new WeakMap();
  var conversations = [];
  var activeId = null;
  var historyError = "";
  var currentUser = null;
  var authEpoch = 0;
  var authMode = "login";
  var nextHistory = null;
  var polling = /* @__PURE__ */ new Map();
  var formError = "";
  var accountView = "home";
  var accountViewVersion = 0;
  var logoutAllReturnView = "home";
  var selectedDevice = null;
  var deviceSessions = [];
  var devicesCursor = "0";
  var devicesLoading = false;
  var emailVerifiedUntil = 0;
  var emailVerificationTimer;
  function icon(name) {
    const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    svg.setAttribute("aria-hidden", "true");
    const use = document.createElementNS("http://www.w3.org/2000/svg", "use");
    use.setAttribute("href", `#icon-${name}`);
    svg.append(use);
    return svg;
  }
  var ApiError = class extends Error {
    constructor(message, code) {
      super(message);
      __publicField(this, "code", code);
    }
  };
  function errorCode(error) {
    return error instanceof ApiError ? error.code : void 0;
  }
  function errorMessage(error) {
    return error instanceof Error ? error.message : "\u8BF7\u6C42\u5931\u8D25\uFF0C\u8BF7\u7A0D\u540E\u91CD\u8BD5\u3002";
  }
  function errorPayload(data) {
    if (typeof data !== "object" || data === null) return {};
    return {
      code: "code" in data && typeof data.code === "string" ? data.code : void 0,
      error: "error" in data && typeof data.error === "string" ? data.error : void 0
    };
  }
  async function api(path, body) {
    const epoch = authEpoch;
    const response = await fetch(`${apiBase}${path}`, {
      method: body === void 0 ? "GET" : "POST",
      credentials: "include",
      headers: body === void 0 ? {} : { "Content-Type": "application/json", "X-Chat-Request": "1" },
      body: body === void 0 ? void 0 : JSON.stringify(body)
    });
    const data = await response.json().catch(() => ({}));
    if (epoch !== authEpoch) throw new ApiError("\u8D26\u53F7\u5DF2\u5207\u6362\u3002", "session_changed");
    if (!response.ok) {
      const failure = errorPayload(data);
      if (failure.code === "unauthenticated" && currentUser) signedOut("\u767B\u5F55\u5DF2\u8FC7\u671F\uFF0C\u8BF7\u91CD\u65B0\u767B\u5F55\u3002");
      throw new ApiError(failure.error || "\u8BF7\u6C42\u5931\u8D25\uFF0C\u8BF7\u7A0D\u540E\u91CD\u8BD5\u3002", failure.code || "request_failed");
    }
    return data;
  }
  function feedback(target, message, success = false) {
    target.textContent = message;
    target.classList.toggle("success", success);
  }
  function setAuthMode(mode, message = "") {
    authMode = mode;
    const labels = {
      username: mode === "register",
      identifier: mode === "login",
      email: ["register", "forgot", "reset"].includes(mode),
      code: mode === "reset",
      password: mode !== "forgot",
      confirm: ["register", "reset"].includes(mode)
    };
    for (const [name, visible] of Object.entries(labels)) {
      $(`#auth-${name}-label`).hidden = !visible;
      const input = $(`#auth-${name}`);
      input.disabled = !visible;
      input.required = visible && !(name === "email" && mode === "register");
    }
    $("#auth-email-hint").hidden = mode !== "register";
    $("#auth-password").value = "";
    $("#auth-confirm").value = "";
    $("#auth-password").autocomplete = mode === "login" ? "current-password" : "new-password";
    $("#auth-password").minLength = mode === "login" ? 1 : 8;
    $("#auth-password").placeholder = mode === "login" ? "" : "8\u2013128 \u4E2A\u5B57\u7B26";
    $("#auth-title").textContent = { login: "\u6B22\u8FCE\u56DE\u6765", register: "\u521B\u5EFA\u4F60\u7684\u8D26\u53F7", forgot: "\u627E\u56DE\u5BC6\u7801", reset: "\u8BBE\u7F6E\u65B0\u5BC6\u7801" }[mode];
    $("#auth-intro").textContent = {
      login: "\u767B\u5F55\u540E\uFF0C\u7EE7\u7EED\u4F60\u7684\u5BF9\u8BDD\u3002",
      register: "\u7528\u4E00\u4E2A\u8D26\u53F7\uFF0C\u4FDD\u5B58\u6BCF\u4E00\u6B21\u5BF9\u8BDD\u3002",
      forgot: "\u901A\u8FC7\u5DF2\u7ED1\u5B9A\u7684\u90AE\u7BB1\u63A5\u6536\u627E\u56DE\u9A8C\u8BC1\u7801\u3002",
      reset: "\u8F93\u5165\u90AE\u7BB1\u9A8C\u8BC1\u7801\uFF0C\u4E3A\u8D26\u53F7\u8BBE\u7F6E\u65B0\u5BC6\u7801\u3002"
    }[mode];
    $("#auth-submit").textContent = { login: "\u767B\u5F55", register: "\u521B\u5EFA\u8D26\u53F7", forgot: "\u53D1\u9001\u627E\u56DE\u9A8C\u8BC1\u7801", reset: "\u91CD\u7F6E\u5BC6\u7801" }[mode];
    $("#auth-switch").textContent = mode === "login" ? "\u521B\u5EFA\u8D26\u53F7" : "\u8FD4\u56DE\u767B\u5F55";
    $("#auth-forgot").hidden = mode !== "login";
    $("#auth-reset").hidden = mode !== "forgot";
    feedback($("#auth-status"), message);
  }
  function signedOut(message = "") {
    currentUser = null;
    ++authEpoch;
    for (const flight of pending.values()) flight.controller.abort();
    for (const timer of polling.values()) clearTimeout(timer);
    polling.clear();
    pending.clear();
    drafts.clear();
    failures.clear();
    notices.clear();
    selectingBranches.clear();
    conversations = [];
    activeId = null;
    models = [];
    defaultModel = null;
    prompt.value = "";
    formError = "";
    historyError = "";
    modelError = "";
    closeAccount(false);
    $("#password-form").reset();
    $("#email-form").reset();
    $("#email-verify-form").reset();
    clearTimeout(emailVerificationTimer);
    emailVerifiedUntil = 0;
    shell.hidden = true;
    $("#auth-screen").hidden = false;
    setAuthMode("login", message);
    render();
  }
  async function startSession(user) {
    currentUser = user;
    ++authEpoch;
    $("#auth-form").reset();
    $("#auth-screen").hidden = true;
    shell.hidden = false;
    updateAccountUser(user);
    render();
    await Promise.all([loadModels(), loadHistory()]);
  }
  async function loadHistory(more = false) {
    if (!currentUser) return;
    try {
      let query = "";
      if (more && nextHistory) query = `?before_updated_at=${nextHistory.before_updated_at}&before_id=${nextHistory.before_id}`;
      const data = await api(`/api/conversations${query}`);
      const existing = new Map(conversations.map((item) => [item.id, item]));
      const items = data.items.map((item) => ({ ...item, messages: existing.get(item.id)?.messages || [], loaded: existing.get(item.id)?.loaded || false, provider: existing.get(item.id)?.provider, model: existing.get(item.id)?.model }));
      conversations = more ? [...conversations, ...items.filter((item) => !existing.has(item.id))] : items;
      nextHistory = data.next;
      $("#more-history").hidden = !nextHistory;
      historyError = "";
      const id = activeId || conversationIdFromUrl();
      if (id) await switchConversation(id, true);
      else render();
    } catch (error) {
      if (currentUser && errorCode(error) !== "session_changed") {
        historyError = errorMessage(error);
        render();
      }
    }
  }
  async function refreshConversation(conversation) {
    const data = await api(`/api/conversation?id=${conversation.id}`);
    if (!conversations.includes(conversation)) return;
    Object.assign(conversation, data.conversation, { messages: data.messages, loaded: true });
    const last = conversation.messages.at(-1);
    if (last?.error) failures.set(conversation.id, last.error);
    else failures.delete(conversation.id);
    if ((conversation.active_message_id || last?.state === "generating") && !pending.has(conversation.id)) {
      clearTimeout(polling.get(conversation.id));
      polling.set(conversation.id, setTimeout(async () => {
        polling.delete(conversation.id);
        if (currentUser && conversations.includes(conversation) && !pending.has(conversation.id)) {
          try {
            await refreshConversation(conversation);
            render();
          } catch (error) {
            historyError = errorMessage(error);
            render();
          }
        }
      }, 1e3));
    }
  }
  async function withForm(form, target, action) {
    const owner = form.closest("#account-page") || form;
    const controls = [...owner.querySelectorAll("input, button")];
    if (form.id === "auth-form") controls.push(...document.querySelectorAll(".auth-links button"));
    if (owner.dataset.busy) return;
    owner.dataset.busy = "true";
    controls.forEach((element) => {
      element.disabled = true;
    });
    feedback(target, "\u6B63\u5728\u5904\u7406\u2026");
    let reauthenticate = false;
    try {
      await action();
    } catch (error) {
      if (errorCode(error) === "reauthentication_required" && owner.id === "account-page") {
        showAccountView("email", false);
        reauthenticate = true;
      }
      if (errorCode(error) !== "session_changed") feedback(target, error instanceof TypeError ? "\u65E0\u6CD5\u8FDE\u63A5\u670D\u52A1\uFF0C\u8BF7\u7A0D\u540E\u91CD\u8BD5\u3002" : errorMessage(error));
    } finally {
      delete owner.dataset.busy;
      controls.forEach((element) => {
        element.disabled = false;
      });
      if (reauthenticate) focusAccountView("email");
    }
  }
  $("#auth-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const mode = authMode;
    const password = $("#auth-password").value;
    if (["register", "reset"].includes(mode) && password !== $("#auth-confirm").value) {
      feedback($("#auth-status"), "\u4E24\u6B21\u8F93\u5165\u7684\u5BC6\u7801\u4E0D\u4E00\u81F4\u3002");
      return;
    }
    const identifier = $("#auth-identifier").value.trim();
    const username = $("#auth-username").value.trim();
    const email = $("#auth-email").value.trim();
    const code = $("#auth-code").value.trim();
    await withForm($("#auth-form"), $("#auth-status"), async () => {
      if (mode === "forgot") {
        const data2 = await api("/api/auth/password/forgot", { email });
        setAuthMode("reset", data2.message);
        return;
      }
      if (mode === "reset") {
        await api("/api/auth/password/reset", { email, code, new_password: password });
        setAuthMode("login", "\u5BC6\u7801\u5DF2\u91CD\u7F6E\uFF0C\u8BF7\u4F7F\u7528\u65B0\u5BC6\u7801\u767B\u5F55\u3002");
        return;
      }
      let registration;
      if (mode === "register") registration = await api("/api/auth/register", { username, password, ...email ? { email } : {} });
      const data = await api("/api/auth/login", { identifier: mode === "register" ? username : identifier, password });
      await startSession(data.user);
      if (registration && email) {
        openAccount("email");
        formInput($("#email-form"), "email").value = email;
        feedback($("#account-status"), registration.email_delivery === "sent" ? "\u8D26\u53F7\u5DF2\u521B\u5EFA\uFF0C\u9A8C\u8BC1\u7801\u5DF2\u53D1\u9001\u3002\u8BF7\u5148\u9A8C\u8BC1\u5F53\u524D\u5BC6\u7801\uFF0C\u518D\u586B\u5199\u90AE\u7BB1\u9A8C\u8BC1\u7801\u3002" : "\u8D26\u53F7\u5DF2\u521B\u5EFA\u3002\u8BF7\u5148\u9A8C\u8BC1\u5F53\u524D\u5BC6\u7801\uFF0C\u518D\u53D1\u9001\u90AE\u7BB1\u9A8C\u8BC1\u7801\u3002", registration.email_delivery === "sent");
      }
    });
    for (const label of $("#auth-form").querySelectorAll("label")) {
      const input = label.querySelector("input");
      if (label.hidden && input) input.disabled = true;
    }
  });
  $("#auth-switch").addEventListener("click", () => setAuthMode(authMode === "login" ? "register" : "login"));
  $("#auth-forgot").addEventListener("click", () => setAuthMode("forgot"));
  $("#auth-reset").addEventListener("click", () => setAuthMode("reset"));
  function deviceName(agent) {
    const browser = /Edg\//.test(agent) ? "Edge" : /OPR\//.test(agent) ? "Opera" : /Firefox\/|FxiOS\//.test(agent) ? "Firefox" : /Chrome\/|CriOS\//.test(agent) ? "Chrome" : /Safari\//.test(agent) ? "Safari" : "\u672A\u77E5\u6D4F\u89C8\u5668";
    const system = /iPhone|iPad|iPod/.test(agent) ? "iOS" : /Android/.test(agent) ? "Android" : /Windows/.test(agent) ? "Windows" : /Macintosh|Mac OS X/.test(agent) ? "macOS" : /Linux/.test(agent) ? "Linux" : "\u672A\u77E5\u7CFB\u7EDF";
    return `${browser} (${system})`;
  }
  function closeDeviceMenus() {
    for (const menu of document.querySelectorAll(".device-menu:popover-open")) menu.hidePopover();
  }
  function renderDevices() {
    closeDeviceMenus();
    const list2 = $("#device-list");
    list2.replaceChildren();
    const items = [...deviceSessions].sort((a, b) => Number(b.current) - Number(a.current) || b.last_active_at - a.last_active_at);
    for (const device of items) {
      const row = document.createElement("tr");
      row.className = `device-row${device.current ? " is-current" : ""}`;
      const details = document.createElement("td");
      details.className = "device-details";
      const name = document.createElement("div");
      name.className = "device-name";
      const label = document.createElement("span");
      label.className = "device-label";
      label.textContent = deviceName(device.user_agent);
      label.title = label.textContent;
      name.append(label);
      if (device.current) {
        const badge = document.createElement("span");
        badge.className = "current-device";
        badge.textContent = "\u5F53\u524D\u8BBE\u5907";
        name.append(badge);
      }
      details.append(name);
      const times = [["\u767B\u5F55\u65F6\u95F4", device.created_at], ["\u6700\u8FD1\u6D3B\u52A8", device.last_active_at]].map(([title, value]) => {
        const cell = document.createElement("td");
        cell.className = "device-time";
        cell.dataset.label = title;
        const time = document.createElement("time");
        time.dateTime = new Date(value).toISOString();
        time.textContent = new Date(value).toLocaleString("zh-CN", { year: "numeric", month: "2-digit", day: "2-digit", hour: "2-digit", minute: "2-digit", hour12: false });
        cell.append(time);
        return cell;
      });
      const actions = document.createElement("td");
      actions.className = "device-actions";
      const menu = document.createElement("div");
      menu.className = "device-menu";
      menu.popover = "auto";
      menu.id = `device-menu-${device.id}`;
      const trigger = document.createElement("button");
      trigger.type = "button";
      trigger.className = "icon-button device-menu-trigger";
      trigger.textContent = "\u22EF";
      trigger.setAttribute("aria-label", `\u7BA1\u7406${device.current ? "\u5F53\u524D\u8BBE\u5907" : deviceName(device.user_agent)}`);
      trigger.setAttribute("aria-expanded", "false");
      trigger.setAttribute("aria-controls", menu.id);
      trigger.popoverTargetElement = menu;
      menu.addEventListener("toggle", () => {
        trigger.setAttribute("aria-expanded", String(menu.matches(":popover-open")));
      });
      menu.addEventListener("beforetoggle", (event) => {
        if (event.newState !== "open") return;
        const rect = trigger.getBoundingClientRect();
        const above = innerHeight - rect.bottom < 80;
        menu.style.right = `${Math.max(8, innerWidth - rect.right)}px`;
        menu.style.top = above ? "auto" : `${rect.bottom + 6}px`;
        menu.style.bottom = above ? `${innerHeight - rect.top + 6}px` : "auto";
      });
      const button = document.createElement("button");
      button.type = "button";
      button.textContent = "\u9000\u51FA\u767B\u5F55";
      button.setAttribute("aria-label", `\u9000\u51FA${device.current ? "\u5F53\u524D\u8BBE\u5907" : deviceName(device.user_agent)}`);
      button.addEventListener("click", () => {
        menu.hidePopover();
        selectedDevice = device;
        $("#logout-device-description").textContent = device.current ? "\u9000\u51FA\u5F53\u524D\u8BBE\u5907\u540E\uFF0C\u9700\u8981\u91CD\u65B0\u767B\u5F55\u3002\u5176\u4ED6\u8BBE\u5907\u4FDD\u6301\u767B\u5F55\u3002" : `\u9000\u51FA ${deviceName(device.user_agent)} \u540E\uFF0C\u8BE5\u8BBE\u5907\u9700\u8981\u91CD\u65B0\u767B\u5F55\u3002`;
        showAccountView("logout-device");
      });
      menu.append(button);
      actions.append(trigger, menu);
      row.append(details, ...times, actions);
      list2.append(row);
    }
  }
  async function loadDevices(more = false) {
    if (!currentUser || accountView !== "devices" || devicesLoading) return;
    devicesLoading = true;
    const version = accountViewVersion;
    $("#devices-more").disabled = true;
    feedback($("#devices-status"), "\u6B63\u5728\u52A0\u8F7D\u5DF2\u767B\u5F55\u8BBE\u5907\u2026");
    try {
      const page = await api(`/api/auth/sessions?cursor=${more ? devicesCursor : "0"}`);
      if (version !== accountViewVersion) return;
      const existing = new Map((more ? deviceSessions : []).map((item) => [item.id, item]));
      for (const device of page.items) existing.set(device.id, device);
      deviceSessions = [...existing.values()];
      devicesCursor = page.next_cursor;
      renderDevices();
      feedback($("#devices-status"), deviceSessions.length ? "" : "\u6682\u65E0\u5DF2\u767B\u5F55\u8BBE\u5907\u3002");
      $("#devices-more").hidden = devicesCursor === "0";
      $("#devices-more").textContent = "\u52A0\u8F7D\u66F4\u591A";
    } catch (error) {
      if (version !== accountViewVersion) return;
      feedback($("#devices-status"), error instanceof TypeError ? "\u65E0\u6CD5\u52A0\u8F7D\u8BBE\u5907\uFF0C\u8BF7\u91CD\u8BD5\u3002" : errorMessage(error));
      $("#devices-more").hidden = false;
      $("#devices-more").textContent = "\u91CD\u8BD5";
    } finally {
      if (version === accountViewVersion) {
        devicesLoading = false;
        $("#devices-more").disabled = false;
      }
    }
  }
  function isAccountView(view) {
    return view !== void 0 && ["home", "username", "password", "email", "email-edit", "devices", "logout-all", "logout-device"].includes(view);
  }
  function focusAccountView(view) {
    const target = view === "username" ? $("#username-form input") : view === "password" ? $("#password-form input") : view === "email" ? $("#email-verify-form input") : view === "email-edit" ? $("#email-form input") : view === "logout-all" ? $("#cancel-logout-all") : view === "logout-device" ? $("#cancel-logout-device") : $("#account-title");
    target.focus({ preventScroll: true });
  }
  function showAccountView(view, focus = true) {
    closeDeviceMenus();
    const titles = { home: "\u8D26\u53F7\u8BBE\u7F6E", username: "\u4FEE\u6539\u7528\u6237\u540D", password: "\u4FEE\u6539\u5BC6\u7801", email: "\u9A8C\u8BC1\u5F53\u524D\u5BC6\u7801", "email-edit": "\u90AE\u7BB1", devices: "\u5DF2\u767B\u5F55\u8BBE\u5907", "logout-all": "\u9000\u51FA\u5168\u90E8\u8BBE\u5907", "logout-device": "\u9000\u51FA\u8BBE\u5907" };
    if (!titles[view]) return;
    if (view === "email-edit" && Date.now() >= emailVerifiedUntil) view = "email";
    const continuingEmail = ["email", "email-edit"].includes(accountView) && ["email", "email-edit"].includes(view);
    clearTimeout(emailVerificationTimer);
    if (view !== "email-edit") emailVerifiedUntil = 0;
    $("#email-verify-form").reset();
    if (view === "logout-all") logoutAllReturnView = accountView === "devices" ? "devices" : "home";
    accountView = view;
    $("#account-page").dataset.view = view;
    $("#devices-logout-all").hidden = view !== "devices";
    ++accountViewVersion;
    devicesLoading = false;
    $("#password-form").reset();
    $("#username-form").reset();
    if (currentUser) formInput($("#username-form"), "username").value = currentUser.username;
    if (!continuingEmail) $("#email-form").reset();
    for (const panel of document.querySelectorAll("[data-account-panel]")) panel.hidden = panel.dataset.accountPanel !== view;
    $("#account-title").textContent = titles[view];
    $("#account-back").hidden = view === "home";
    feedback($("#account-status"), "");
    $("#account-page").scrollTop = 0;
    if (view === "devices") {
      deviceSessions = [];
      devicesCursor = "0";
      $("#device-list").replaceChildren();
      $("#devices-more").hidden = true;
      loadDevices();
    }
    if (view === "email-edit") emailVerificationTimer = setTimeout(() => {
      showAccountView("email");
      feedback($("#account-status"), "\u8EAB\u4EFD\u9A8C\u8BC1\u5DF2\u8FC7\u671F\uFF0C\u8BF7\u91CD\u65B0\u9A8C\u8BC1\u5BC6\u7801\u3002");
    }, Math.max(0, emailVerifiedUntil - Date.now()));
    if (focus) focusAccountView(view);
  }
  function updateAccountUser(user) {
    currentUser = user;
    $("#account-name").textContent = user.username;
    $("#account-avatar").textContent = user.username.charAt(0).toUpperCase();
    $("#open-account").setAttribute("aria-label", `${user.username}\uFF0C\u8D26\u53F7\u83DC\u5355`);
    $("#username-overview").textContent = user.username;
    $("#email-overview").textContent = user.email || "\u5C1A\u672A\u7ED1\u5B9A";
    $("#email-overview").title = user.email || "";
    $("#email-summary").textContent = user.email ? `\u5DF2\u7ED1\u5B9A ${user.email}` : "\u7ED1\u5B9A\u540E\u53EF\u7528\u90AE\u7BB1\u767B\u5F55\u548C\u627E\u56DE\u5BC6\u7801\u3002";
    $("#email-address-label").textContent = user.email ? "\u65B0\u90AE\u7BB1" : "\u90AE\u7BB1\u5730\u5740";
    $("#email-unbind").hidden = !user.email;
  }
  function closeAccountMenu() {
    if ($("#account-menu").matches(":popover-open")) $("#account-menu").hidePopover();
  }
  function openAccount(view = "home") {
    if (!currentUser || $("#account-page").dataset.busy) return;
    closeAccountMenu();
    updateAccountUser(currentUser);
    closeSidebar();
    workspace.hidden = true;
    $("#account-page").hidden = false;
    showAccountView(view);
  }
  function closeAccount(focus = true) {
    closeAccountMenu();
    $("#account-page").hidden = true;
    workspace.hidden = false;
    showAccountView("home", false);
    deviceSessions = [];
    selectedDevice = null;
    $("#device-list").replaceChildren();
    if (focus) (prompt.disabled ? $("#open-sidebar") : prompt).focus({ preventScroll: true });
  }
  $("#account-menu").addEventListener("beforetoggle", (event) => {
    if (event.newState !== "open") return;
    const rect = $("#open-account").getBoundingClientRect();
    const menu = $("#account-menu");
    menu.style.left = `${Math.max(8, rect.left)}px`;
    menu.style.bottom = `${innerHeight - rect.top + 8}px`;
    menu.style.width = `${Math.min(280, innerWidth - 16)}px`;
    menu.style.maxHeight = `${Math.max(100, rect.top - 16)}px`;
  });
  $("#account-menu").addEventListener("toggle", () => {
    $("#open-account").setAttribute("aria-expanded", String($("#account-menu").matches(":popover-open")));
  });
  $("#account-menu").addEventListener("keydown", (event) => {
    if (!["ArrowDown", "ArrowUp", "Home", "End"].includes(event.key)) return;
    event.preventDefault();
    const buttons = [...$("#account-menu").querySelectorAll("button")];
    const index = buttons.findIndex((button) => button === document.activeElement);
    const next = event.key === "Home" ? 0 : event.key === "End" ? buttons.length - 1 : (index + (event.key === "ArrowDown" ? 1 : -1) + buttons.length) % buttons.length;
    buttons[next]?.focus();
  });
  for (const button of document.querySelectorAll("[data-account-view]")) {
    button.addEventListener("click", () => {
      if (!isAccountView(button.dataset.accountView)) return;
      if (button.closest("#account-menu")) openAccount(button.dataset.accountView);
      else showAccountView(button.dataset.accountView);
    });
  }
  $("#devices-more").addEventListener("click", () => loadDevices(true));
  $("#account-page").addEventListener("scroll", closeDeviceMenus);
  window.addEventListener("resize", () => {
    closeDeviceMenus();
    closeAccountMenu();
  });
  $("#account-back").addEventListener("click", () => showAccountView(accountView === "logout-device" ? "devices" : accountView === "email-edit" ? "email" : accountView === "logout-all" ? logoutAllReturnView : "home"));
  $("#cancel-logout-all").addEventListener("click", () => showAccountView(logoutAllReturnView));
  $("#cancel-logout-device").addEventListener("click", () => showAccountView("devices"));
  $("#close-account").addEventListener("click", () => closeAccount());
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && !$("#account-page").hidden && !$("#account-page").dataset.busy && !document.querySelector(":popover-open")) closeAccount();
  });
  async function logout() {
    openAccount();
    await withForm($("#logout-all-form"), $("#account-status"), async () => {
      await api("/api/auth/logout", {});
      signedOut("\u5DF2\u9000\u51FA\u5F53\u524D\u8BBE\u5907\u3002");
    });
  }
  $("#logout").addEventListener("click", logout);
  $("#menu-logout").addEventListener("click", logout);
  $("#username-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const form = $("#username-form");
    const username = formInput(form, "username").value.trim();
    await withForm(form, $("#account-status"), async () => {
      const data = await api("/api/auth/username", { username });
      updateAccountUser(data.user);
      formInput(form, "username").value = data.user.username;
      feedback($("#account-status"), "\u7528\u6237\u540D\u5DF2\u66F4\u65B0\uFF0C\u4E0B\u6B21\u767B\u5F55\u8BF7\u4F7F\u7528\u65B0\u7528\u6237\u540D\u3002", true);
    });
  });
  $("#logout-all-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    await withForm($("#logout-all-form"), $("#account-status"), async () => {
      await api("/api/auth/logout/all", {});
      signedOut("\u5DF2\u9000\u51FA\u5168\u90E8\u8BBE\u5907\uFF0C\u8BF7\u91CD\u65B0\u767B\u5F55\u3002");
    });
  });
  $("#logout-device-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const device = selectedDevice;
    if (!device) return;
    await withForm($("#logout-device-form"), $("#account-status"), async () => {
      await api("/api/auth/sessions/revoke", { session_id: device.id });
      if (device.current) signedOut("\u5DF2\u9000\u51FA\u5F53\u524D\u8BBE\u5907\u3002");
      else {
        showAccountView("devices");
        feedback($("#account-status"), "\u5DF2\u9000\u51FA\u8BE5\u8BBE\u5907\u3002", true);
      }
    });
  });
  $("#password-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const form = $("#password-form");
    const values = { old_password: formInput(form, "old_password").value, new_password: formInput(form, "new_password").value, confirm: formInput(form, "confirm").value };
    if (values.new_password !== values.confirm) {
      feedback($("#account-status"), "\u4E24\u6B21\u8F93\u5165\u7684\u5BC6\u7801\u4E0D\u4E00\u81F4\u3002");
      return;
    }
    await withForm(form, $("#account-status"), async () => {
      await api("/api/auth/password", { old_password: values.old_password, new_password: values.new_password });
      signedOut("\u5BC6\u7801\u5DF2\u66F4\u65B0\uFF0C\u8BF7\u91CD\u65B0\u767B\u5F55\u3002");
    });
  });
  $("#email-verify-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const form = $("#email-verify-form");
    const password = formInput(form, "password").value;
    let verified = false;
    await withForm(form, $("#account-status"), async () => {
      const result = await api("/api/auth/password/verify", { password });
      emailVerifiedUntil = Date.now() + result.expires_in * 1e3;
      showAccountView("email-edit", false);
      verified = true;
    });
    if (verified && accountView === "email-edit") focusAccountView("email-edit");
  });
  function requireEmailVerification() {
    if (Date.now() < emailVerifiedUntil) return true;
    showAccountView("email");
    feedback($("#account-status"), "\u8EAB\u4EFD\u9A8C\u8BC1\u5DF2\u8FC7\u671F\uFF0C\u8BF7\u91CD\u65B0\u9A8C\u8BC1\u5BC6\u7801\u3002");
    return false;
  }
  $("#email-send").addEventListener("click", async () => {
    const form = $("#email-form");
    if (!requireEmailVerification() || !formInput(form, "email").reportValidity()) return;
    await withForm(form, $("#account-status"), async () => {
      await api("/api/auth/email/code", { email: formInput(form, "email").value });
      feedback($("#account-status"), "\u9A8C\u8BC1\u7801\u5DF2\u53D1\u9001\uFF0C\u8BF7\u67E5\u6536\u90AE\u4EF6\u3002", true);
    });
  });
  $("#email-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!requireEmailVerification()) return;
    const form = $("#email-form");
    const body = { email: formInput(form, "email").value, code: formInput(form, "code").value };
    await withForm(form, $("#account-status"), async () => {
      const data = await api("/api/auth/email/bind", body);
      updateAccountUser(data.user);
      showAccountView("home", false);
      feedback($("#account-status"), "\u90AE\u7BB1\u5DF2\u9A8C\u8BC1\u5E76\u7ED1\u5B9A\u3002", true);
    });
  });
  $("#email-unbind").addEventListener("click", async () => {
    if (!requireEmailVerification()) return;
    await withForm($("#email-form"), $("#account-status"), async () => {
      const data = await api("/api/auth/email/unbind", {});
      updateAccountUser(data.user);
      showAccountView("home", false);
      feedback($("#account-status"), "\u90AE\u7BB1\u5DF2\u89E3\u7ED1\u3002", true);
    });
  });
  $("#more-history").addEventListener("click", () => loadHistory(true));
  $("#rename-conversation").addEventListener("click", async () => {
    const conversation = currentConversation();
    if (!conversation) return;
    const title = window.prompt("\u5BF9\u8BDD\u6807\u9898", conversation.title)?.trim();
    if (!title) return;
    try {
      await api("/api/conversation/rename", { conversation_id: conversation.id, title });
      conversation.title = title;
      render();
    } catch (error) {
      formError = errorMessage(error);
      updateComposer();
    }
  });
  function currentConversation() {
    return conversations.find((item) => item.id === activeId);
  }
  function makeId() {
    const bytes = new Uint32Array(2);
    crypto.getRandomValues(bytes);
    return `${Date.now().toString(36)}-${Array.from(bytes, (value) => value.toString(36)).join("")}`;
  }
  function closeSidebar() {
    closeAccountMenu();
    shell.classList.remove("sidebar-open");
    $("#sidebar-overlay").hidden = true;
    $("#sidebar").inert = window.innerWidth <= 720;
    $("#open-sidebar").setAttribute("aria-expanded", String(!shell.classList.contains("sidebar-collapsed") && window.innerWidth > 720));
  }
  function conversationIdFromUrl() {
    return /^\/c\/(\d+)$/.exec(location.pathname)?.[1] || /^#conversation-(\d+)$/.exec(location.hash)?.[1] || null;
  }
  function updateConversationUrl(id, replace = false) {
    const path = id ? `/c/${id}` : "/";
    if (`${location.pathname}${location.search}${location.hash}` === path) return;
    if (replace) history.replaceState(null, "", path);
    else history.pushState(null, "", path);
  }
  async function switchConversation(id, replace = false) {
    if (!$("#account-page").hidden) closeAccount(false);
    drafts.set(activeId || "new", prompt.value);
    activeId = id;
    formError = "";
    prompt.value = drafts.get(id || "new") || "";
    updateConversationUrl(id, replace);
    if (id && !pending.has(id)) {
      let conversation = currentConversation();
      try {
        if (!conversation) {
          const data = await api(`/api/conversation?id=${id}`);
          conversation = { ...data.conversation, messages: data.messages, loaded: true };
          conversations.push(conversation);
        }
        await refreshConversation(conversation);
      } catch (error) {
        historyError = errorMessage(error);
        if (!conversation && activeId === id) {
          activeId = null;
          id = null;
          updateConversationUrl(null, true);
        }
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
    drafts.delete("new");
    if (activeId === null) prompt.value = "";
    switchConversation(null);
  }
  function renderList() {
    list.replaceChildren();
    for (const conversation of conversations) {
      const row = document.createElement("div");
      row.className = `conversation-item${conversation.id === activeId ? " active" : ""}`;
      const link = document.createElement("button");
      link.className = "conversation-link";
      link.textContent = conversation.title;
      link.title = conversation.title;
      if (conversation.id === activeId) link.setAttribute("aria-current", "page");
      link.addEventListener("click", () => switchConversation(conversation.id));
      const remove = document.createElement("button");
      remove.className = "icon-button delete-conversation";
      remove.setAttribute("aria-label", `\u5220\u9664\u5BF9\u8BDD\uFF1A${conversation.title}`);
      remove.title = "\u5220\u9664\u5BF9\u8BDD";
      remove.append(icon("trash"));
      remove.addEventListener("click", async () => {
        try {
          await api("/api/conversation/delete", { conversation_id: conversation.id });
        } catch (error) {
          formError = errorMessage(error);
          updateComposer();
          return;
        }
        pending.get(conversation.id)?.controller.abort();
        pending.delete(conversation.id);
        failures.delete(conversation.id);
        notices.delete(conversation.id);
        drafts.delete(conversation.id);
        conversations = conversations.filter((item) => item.id !== conversation.id);
        if (activeId === conversation.id) {
          activeId = null;
          updateConversationUrl(null, true);
          prompt.value = drafts.get("new") || "";
          formError = "";
        }
        render();
        resizePrompt();
      });
      row.append(link, remove);
      list.append(row);
    }
    $("#history-empty").hidden = conversations.length > 0;
  }
  function renderContent(parent, text) {
    if (window.ChatMarkdown) window.ChatMarkdown.renderInto(parent, text);
    else parent.textContent = text;
  }
  function reasoningBlock(owner, text, state) {
    let view = reasoningViews.get(owner);
    if (!view) {
      const details = document.createElement("details");
      details.className = "thinking";
      const summary = document.createElement("summary");
      const title = document.createElement("span");
      title.className = "thinking-title";
      summary.append(title, icon("chevron-down"));
      const content = document.createElement("div");
      content.className = "message-content thinking-content";
      details.append(summary, content);
      view = { details, title, content, text: "" };
      reasoningViews.set(owner, view);
    }
    view.details.dataset.state = state;
    view.title.textContent = state === "thinking" ? "\u6B63\u5728\u601D\u8003\u2026" : state === "interrupted" ? "\u601D\u8003\u5DF2\u4E2D\u65AD" : "\u601D\u8003\u8FC7\u7A0B";
    if (view.text !== text) {
      renderContent(view.content, text);
      view.text = text;
    }
    return view.details;
  }
  function modelHistory(messages) {
    const history2 = [];
    for (const message of messages) {
      if (message.role === "assistant" && !message.content.trim()) {
        history2.pop();
        continue;
      }
      history2.push({ role: message.role, content: message.content });
    }
    return history2;
  }
  function conversationBusy(conversation) {
    return pending.has(conversation.id) || selectingBranches.has(conversation.id) || Boolean(conversation.active_message_id) || conversation.messages.at(-1)?.state === "generating";
  }
  async function selectReply(conversation, messageId, previousMessageId) {
    if (conversationBusy(conversation)) return;
    const article = [...messagesElement.querySelectorAll("[data-message-id]")].find((element) => element.dataset.messageId === previousMessageId);
    const top = article?.getBoundingClientRect().top;
    selectingBranches.add(conversation.id);
    formError = "";
    renderMessages();
    updateComposer();
    try {
      const data = await api("/api/conversation/select", { conversation_id: conversation.id, message_id: messageId });
      if (!conversations.includes(conversation)) return;
      Object.assign(conversation, data.conversation, { messages: data.messages, loaded: true });
      failures.delete(conversation.id);
      notices.delete(conversation.id);
      const last = conversation.messages.at(-1);
      if (last?.error) failures.set(conversation.id, last.error);
    } catch (error) {
      if (currentUser && conversations.includes(conversation) && errorCode(error) !== "session_changed") {
        formError = error instanceof TypeError ? "\u65E0\u6CD5\u5207\u6362\u56DE\u590D\u7248\u672C\uFF0C\u8BF7\u91CD\u8BD5\u3002" : errorMessage(error);
        if (errorCode(error) === "conflict") {
          try {
            await refreshConversation(conversation);
          } catch {
          }
        }
      }
    } finally {
      selectingBranches.delete(conversation.id);
      if (currentUser && conversations.includes(conversation) && activeId === conversation.id) {
        render();
        const selected = [...messagesElement.querySelectorAll("[data-message-id]")].find((element) => element.dataset.messageId === messageId);
        if (selected && top !== void 0) scrollArea.scrollTop += selected.getBoundingClientRect().top - top;
        selected?.querySelector(".reply-version-count")?.focus({ preventScroll: true });
      }
    }
  }
  function replyVersionPicker(conversation, message) {
    const versions = message.versions;
    if (!versions || versions.total < 2) return null;
    const picker = document.createElement("div");
    picker.className = "reply-versions";
    picker.setAttribute("role", "group");
    picker.setAttribute("aria-label", "\u56DE\u590D\u7248\u672C");
    const count = document.createElement("span");
    count.className = "reply-version-count";
    count.tabIndex = -1;
    count.textContent = `${versions.index}/${versions.total}`;
    count.setAttribute("aria-label", `\u7B2C ${versions.index} \u4E2A\u56DE\u590D\uFF0C\u5171 ${versions.total} \u4E2A`);
    for (const direction of ["previous", "next"]) {
      const targetId = direction === "previous" ? versions.previous_id : versions.next_id;
      const button = document.createElement("button");
      button.type = "button";
      button.className = `icon-button reply-version-${direction}`;
      button.title = direction === "previous" ? "\u4E0A\u4E00\u7248\u56DE\u590D" : "\u4E0B\u4E00\u7248\u56DE\u590D";
      button.setAttribute("aria-label", button.title);
      button.disabled = !targetId || conversationBusy(conversation);
      button.append(icon("chevron-down"));
      button.addEventListener("click", () => {
        if (targetId) selectReply(conversation, targetId, message.id);
      });
      if (direction === "previous") picker.append(button, count);
      else picker.append(button);
    }
    return picker;
  }
  function renderMessages() {
    messagesElement.replaceChildren();
    const conversation = currentConversation();
    if (!conversation) return;
    for (const message of conversation.messages) {
      const flight2 = pending.get(conversation.id);
      if (flight2?.regenerate && (flight2.text || flight2.reasoning) && message === conversation.messages.at(-1)) continue;
      const article = document.createElement("article");
      article.className = `message ${message.role}`;
      if (message.id) article.dataset.messageId = message.id;
      article.setAttribute("aria-label", message.role === "user" ? "\u4F60" : "AI \u7684\u56DE\u590D");
      if (message.role === "user") article.textContent = message.content;
      else {
        let content = messageContentCache.get(message);
        if (!content) {
          content = document.createElement("div");
          content.className = "message-content";
          renderContent(content, message.content);
          messageContentCache.set(message, content);
        }
        const actions = document.createElement("div");
        actions.className = "message-actions";
        const copy = document.createElement("button");
        copy.className = "icon-button";
        copy.title = "\u590D\u5236\u56DE\u590D";
        copy.setAttribute("aria-label", "\u590D\u5236\u56DE\u590D");
        copy.append(icon("copy"));
        copy.hidden = !message.content.trim();
        copy.addEventListener("click", async () => {
          try {
            if (navigator.clipboard?.writeText) await navigator.clipboard.writeText(message.content);
            else {
              const field = document.createElement("textarea");
              field.value = message.content;
              field.style.position = "fixed";
              field.style.opacity = "0";
              document.body.append(field);
              field.select();
              const copied = document.execCommand("copy");
              field.remove();
              if (!copied) throw new Error("copy failed");
            }
            copy.title = "\u5DF2\u590D\u5236";
            copy.setAttribute("aria-label", "\u5DF2\u590D\u5236");
          } catch {
            copy.title = "\u590D\u5236\u5931\u8D25\uFF0C\u8BF7\u9009\u62E9\u6587\u5B57\u590D\u5236";
            copy.setAttribute("aria-label", copy.title);
          }
        });
        actions.append(copy);
        if (message === conversation.messages.at(-1)) {
          const regenerate = document.createElement("button");
          regenerate.className = "regenerate-button";
          regenerate.title = "\u91CD\u65B0\u751F\u6210\u8FD9\u6761\u56DE\u590D";
          regenerate.setAttribute("aria-label", "\u91CD\u8BD5\u56DE\u590D");
          regenerate.disabled = conversationBusy(conversation);
          regenerate.append(icon("retry"));
          regenerate.addEventListener("click", () => requestReply(conversation, { regenerate: true }));
          actions.append(regenerate);
        }
        const versions = replyVersionPicker(conversation, message);
        if (versions) actions.append(versions);
        if (message.reasoning) article.append(reasoningBlock(message, message.reasoning, message.interrupted && !message.content ? "interrupted" : "done"));
        if (message.content) article.append(content);
        if (message.truncated || message.interrupted) {
          const note = document.createElement("p");
          note.className = "truncated-note";
          note.textContent = message.interrupted ? "\u751F\u6210\u5DF2\u4E2D\u65AD\uFF0C\u4EE5\u4E0A\u662F\u5DF2\u6536\u5230\u7684\u5185\u5BB9\u3002" : "\u56DE\u590D\u8FBE\u5230\u957F\u5EA6\u9650\u5236\uFF0C\u53EF\u4EE5\u53D1\u9001\u201C\u8BF7\u7EE7\u7EED\u201D\u3002";
          article.append(note);
        }
        article.append(actions);
      }
      messagesElement.append(article);
    }
    const flight = pending.get(activeId);
    if (flight?.text || flight?.reasoning) {
      const article = document.createElement("article");
      article.className = "message assistant streaming";
      article.setAttribute("aria-label", "AI \u6B63\u5728\u56DE\u590D");
      const content = document.createElement("div");
      content.className = "message-content";
      renderContent(content, flight.text);
      if (flight.reasoning) article.append(reasoningBlock(flight, flight.reasoning, flight.text ? "done" : "thinking"));
      if (flight.text) article.append(content);
      messagesElement.append(article);
    } else if (flight) {
      const waiting = document.createElement("div");
      waiting.className = "pending";
      const dot = document.createElement("span");
      dot.className = "pending-dot";
      waiting.append(dot, document.createTextNode("\u6B63\u5728\u7B49\u5F85\u56DE\u590D\u2026"));
      messagesElement.append(waiting);
    }
  }
  function updateComposer() {
    const conversation = currentConversation();
    const waiting = pending.has(activeId);
    const flight = pending.get(activeId);
    const thinking = flight?.reasoning && !flight.text;
    const unanswered = conversation?.messages.at(-1)?.role === "user";
    const remote = Boolean(conversation?.active_message_id) || conversation?.messages.at(-1)?.state === "generating";
    const selecting = Boolean(conversation && selectingBranches.has(conversation.id));
    prompt.disabled = !currentUser || waiting || remote || selecting || Boolean(unanswered);
    prompt.placeholder = thinking ? "\u6B63\u5728\u601D\u8003\u2026" : waiting ? "\u6B63\u5728\u7B49\u5F85\u56DE\u590D\u2026" : unanswered ? "\u91CD\u8BD5\u4E0A\u4E00\u6761\u6D88\u606F\uFF0C\u6216\u65B0\u5EFA\u5BF9\u8BDD" : "\u6709\u4EC0\u4E48\u6211\u53EF\u4EE5\u5E2E\u4F60\uFF1F";
    const chosen = selectedModel();
    sendButton.hidden = waiting;
    stopButton.hidden = !waiting;
    stopButton.disabled = pending.get(activeId)?.stopped === true;
    sendButton.disabled = prompt.disabled || !prompt.value.trim() || !chosen;
    modelSelect.disabled = waiting || selecting || !models.length;
    if (modelSelect.disabled) closeModelMenu();
    messagesElement.setAttribute("aria-busy", String(waiting));
    retryButton.hidden = remote || waiting || selecting || !unanswered && !failures.has(activeId);
    const error = formError || failures.get(activeId) || modelError || historyError;
    status.classList.toggle("error", Boolean(error));
    status.textContent = error || notices.get(activeId) || (remote && !waiting ? "\u56DE\u590D\u4ECD\u5728\u751F\u6210\uFF0C\u5386\u53F2\u4F1A\u81EA\u52A8\u66F4\u65B0\u3002" : "") || (thinking ? "\u6B63\u5728\u601D\u8003\uFF0C\u53EF\u4EE5\u968F\u65F6\u505C\u6B62\u3002" : waiting ? "\u6B63\u5728\u751F\u6210\uFF0C\u53EF\u4EE5\u968F\u65F6\u505C\u6B62\u3002" : "") || (unanswered && !waiting ? "\u4E0A\u4E00\u6761\u6D88\u606F\u8FD8\u672A\u6536\u5230\u56DE\u590D\uFF0C\u53EF\u4EE5\u91CD\u8BD5\u3002" : "");
  }
  function render() {
    const conversation = currentConversation();
    workspace.classList.toggle("empty", !conversation);
    $("#conversation-title").textContent = conversation?.title || "\u65B0\u5BF9\u8BDD";
    $("#rename-conversation").hidden = !conversation;
    document.title = `Chat \xB7 ${conversation?.title || "\u65B0\u5BF9\u8BDD"}`;
    syncModelSelect();
    renderList();
    renderMessages();
    updateComposer();
  }
  function resizePrompt() {
    prompt.style.height = "auto";
    prompt.style.height = `${Math.min(prompt.scrollHeight, 220)}px`;
  }
  function scrollToEnd() {
    requestAnimationFrame(() => {
      scrollArea.scrollTop = scrollArea.scrollHeight;
    });
  }
  function modelKey(model) {
    return JSON.stringify([model.provider, model.model]);
  }
  function selectedModel() {
    const conversation = currentConversation();
    const choice = conversation?.provider ? conversation : defaultModel;
    return models.find((model) => choice && model.provider === choice.provider && model.model === choice.model) || null;
  }
  function syncModelSelect() {
    closeModelMenu();
    const chosen = selectedModel();
    const label = chosen ? `${chosen.provider} / ${chosen.model}` : models.length ? "\u6A21\u578B\u4E0D\u53EF\u7528" : modelError ? "\u6A21\u578B\u52A0\u8F7D\u5931\u8D25" : "\u6B63\u5728\u52A0\u8F7D\u6A21\u578B\u2026";
    $("#model-label").textContent = label;
    modelSelect.title = label;
    modelSelect.setAttribute("aria-label", `\u9009\u62E9\u6A21\u578B\uFF1A${label}`);
    for (const option of modelMenu.children) {
      option.setAttribute("aria-selected", String(chosen !== null && option.getAttribute("data-key") === modelKey(chosen)));
    }
  }
  function closeModelMenu() {
    modelMenu.hidePopover();
    modelSelect.setAttribute("aria-expanded", "false");
  }
  function openModelMenu() {
    if (modelSelect.disabled) return;
    modelMenu.showPopover({ source: modelSelect });
    modelSelect.setAttribute("aria-expanded", "true");
    const trigger = modelSelect.getBoundingClientRect();
    const gap = 8;
    const above = trigger.top - gap - 12;
    const below = window.innerHeight - trigger.bottom - gap - 12;
    const openAbove = above >= Math.min(modelMenu.scrollHeight, 360) || above > below;
    modelMenu.style.maxHeight = `${Math.max(0, Math.min(360, openAbove ? above : below))}px`;
    const menu = modelMenu.getBoundingClientRect();
    modelMenu.style.left = `${Math.max(12, Math.min(trigger.right - menu.width, window.innerWidth - menu.width - 12))}px`;
    modelMenu.style.top = `${openAbove ? trigger.top - gap - menu.height : trigger.bottom + gap}px`;
    (modelMenu.querySelector('[aria-selected="true"]') || modelMenu.querySelector(":scope > :first-child"))?.focus();
  }
  function renderModelOptions() {
    modelMenu.replaceChildren();
    for (const model of models) {
      const option = document.createElement("button");
      option.type = "button";
      option.className = "model-option";
      option.setAttribute("role", "option");
      option.setAttribute("aria-label", `${model.provider} / ${model.model}`);
      option.title = `${model.provider} / ${model.model}`;
      option.tabIndex = -1;
      option.dataset.key = modelKey(model);
      const name = document.createElement("span");
      name.className = "model-option-name";
      name.textContent = model.model;
      option.append(name, icon("check"));
      option.addEventListener("click", () => {
        const conversation = currentConversation();
        if (conversation) {
          conversation.provider = model.provider;
          conversation.model = model.model;
        } else defaultModel = model;
        syncModelSelect();
        updateComposer();
        modelSelect.focus();
      });
      modelMenu.append(option);
    }
  }
  async function loadModels() {
    modelError = "";
    try {
      const data = await api("/api/models");
      if (!Array.isArray(data.models) || !data.models.length || !data.models.every((item) => typeof item.provider === "string" && typeof item.model === "string")) throw new Error("\u6CA1\u6709\u53EF\u7528\u6A21\u578B\uFF0C\u8BF7\u68C0\u67E5\u540E\u7AEF\u914D\u7F6E\u3002");
      models = data.models;
      const defaultKey = defaultModel ? modelKey(defaultModel) : null;
      if (!models.some((model) => modelKey(model) === defaultKey)) defaultModel = models[0] ?? null;
      renderModelOptions();
    } catch (error) {
      if (!currentUser || errorCode(error) === "session_changed") return;
      modelError = error instanceof TypeError ? "\u65E0\u6CD5\u8FDE\u63A5\u540E\u7AEF\uFF0C\u8BF7\u786E\u8BA4\u670D\u52A1\u5DF2\u542F\u52A8\u3002" : errorMessage(error);
    }
    $("#reload-models").hidden = !modelError;
    render();
  }
  modelSelect.addEventListener("click", (event) => {
    event.preventDefault();
    if (modelMenu.matches(":popover-open")) closeModelMenu();
    else openModelMenu();
  });
  modelSelect.addEventListener("keydown", (event) => {
    if (event.key === "ArrowDown" || event.key === "ArrowUp") {
      event.preventDefault();
      openModelMenu();
    }
  });
  modelMenu.addEventListener("toggle", (event) => {
    modelSelect.setAttribute("aria-expanded", String(event.newState === "open"));
  });
  modelMenu.addEventListener("keydown", (event) => {
    const options = [...modelMenu.querySelectorAll(":scope > button")];
    const index = options.findIndex((option) => option === document.activeElement);
    let next;
    if (event.key === "ArrowDown") next = (index + 1) % options.length;
    else if (event.key === "ArrowUp") next = (index - 1 + options.length) % options.length;
    else if (event.key === "Home") next = 0;
    else if (event.key === "End") next = options.length - 1;
    else if (event.key === "Escape" || event.key === "Tab") {
      closeModelMenu();
      modelSelect.focus();
    }
    if (next !== void 0) {
      event.preventDefault();
      options[next]?.focus();
    }
  });
  $("#reload-models").addEventListener("click", loadModels);
  stopButton.addEventListener("click", () => {
    const flight = pending.get(activeId);
    if (flight) {
      flight.stopped = true;
      flight.controller.abort();
      updateComposer();
    }
  });
  function isRecord(value) {
    return typeof value === "object" && value !== null && !Array.isArray(value);
  }
  async function readEvents(response, onEvent) {
    if (!response.headers.get("content-type")?.includes("text/event-stream") || !response.body)
      throw new Error("\u540E\u7AEF\u6CA1\u6709\u8FD4\u56DE\u6D41\u5F0F\u56DE\u590D\u3002");
    const reader = response.body.getReader();
    const decoder = new TextDecoder("utf-8", { fatal: true });
    let buffer = "", event = "", eventSize = 0;
    let data = [];
    function line(text) {
      if (!text) {
        if (data.length) {
          const payload = JSON.parse(data.join("\n"));
          if (!isRecord(payload)) throw new Error("\u56DE\u590D\u4E8B\u4EF6\u683C\u5F0F\u5F02\u5E38\u3002");
          const stop = onEvent(event || "message", payload);
          event = "";
          data = [];
          eventSize = 0;
          return stop;
        }
        event = "";
        return false;
      }
      if (text.startsWith(":")) return false;
      const colon = text.indexOf(":");
      const field = colon < 0 ? text : text.slice(0, colon);
      let value = colon < 0 ? "" : text.slice(colon + 1);
      if (value.startsWith(" ")) value = value.slice(1);
      if (field === "event") event = value;
      if (field === "data") {
        data.push(value);
        eventSize += value.length + 1;
      }
      if (eventSize > 1024 * 1024) throw new Error("\u56DE\u590D\u4E8B\u4EF6\u8FC7\u5927\uFF0C\u5DF2\u505C\u6B62\u63A5\u6536\u3002");
      return false;
    }
    try {
      for (; ; ) {
        const { value, done } = await reader.read();
        buffer += decoder.decode(value, { stream: !done });
        let match;
        while (match = /\r\n|\r|\n/.exec(buffer)) {
          if (!done && match[0] === "\r" && match.index === buffer.length - 1) break;
          const text = buffer.slice(0, match.index);
          buffer = buffer.slice(match.index + match[0].length);
          if (line(text)) return;
        }
        if (buffer.length + eventSize > 1024 * 1024) throw new Error("\u56DE\u590D\u4E8B\u4EF6\u8FC7\u5927\uFF0C\u5DF2\u505C\u6B62\u63A5\u6536\u3002");
        if (done) throw new Error("\u8FDE\u63A5\u610F\u5916\u4E2D\u65AD\uFF0C\u56DE\u590D\u672A\u5B8C\u6574\u751F\u6210\u3002");
      }
    } finally {
      await reader.cancel().catch(() => {
      });
      reader.releaseLock();
    }
  }
  function paintStream(id) {
    if (activeId !== id || streamFrame !== null) return;
    streamFrame = requestAnimationFrame(() => {
      streamFrame = null;
      const follow = scrollArea.scrollHeight - scrollArea.scrollTop - scrollArea.clientHeight < 120;
      renderMessages();
      updateComposer();
      if (follow) scrollArea.scrollTop = scrollArea.scrollHeight;
    });
  }
  async function requestReply(conversation, { regenerate = false } = {}) {
    const id = conversation.id;
    if (conversationBusy(conversation)) return;
    const chosen = selectedModel();
    if (!chosen) {
      formError = "\u5F53\u524D\u6A21\u578B\u4E0D\u53EF\u7528\uFF0C\u8BF7\u5207\u6362\u6A21\u578B\u540E\u91CD\u8BD5\u3002";
      updateComposer();
      return;
    }
    const lastRole = conversation.messages.at(-1)?.role;
    if (lastRole !== (regenerate ? "assistant" : "user")) return;
    const question = regenerate ? conversation.messages.at(-2) : conversation.messages.at(-1);
    if (!question || regenerate && !question.id) return;
    const flight = { controller: new AbortController(), text: "", reasoning: "", regenerate, stopped: false };
    pending.set(id, flight);
    failures.delete(id);
    notices.delete(id);
    formError = "";
    conversation.provider = chosen.provider;
    conversation.model = chosen.model;
    render();
    scrollToEnd();
    try {
      const response = await fetch(`${apiBase}/api/chat`, {
        method: "POST",
        credentials: "include",
        headers: { "Content-Type": "application/json", "X-Chat-Request": "1" },
        body: JSON.stringify({
          provider: chosen.provider,
          model: chosen.model,
          stream: true,
          conversation_id: id,
          request_id: makeId(),
          ...regenerate && question.id ? { regenerate_message_id: question.id } : { content: question.content }
        }),
        signal: flight.controller.signal
      });
      if (!response.ok) {
        const data = errorPayload(await response.json().catch(() => null));
        if (data?.code === "unauthenticated") signedOut("\u767B\u5F55\u5DF2\u8FC7\u671F\uFF0C\u8BF7\u91CD\u65B0\u767B\u5F55\u3002");
        throw new Error(typeof data?.error === "string" ? data.error : "\u8BF7\u6C42\u5931\u8D25\uFF0C\u8BF7\u91CD\u8BD5\u3002");
      }
      await readEvents(response, (event, data) => {
        if (event === "meta") {
          if (typeof data.user_message_id !== "string" || typeof data.assistant_message_id !== "string") throw new Error("\u56DE\u590D\u6D88\u606F ID \u5F02\u5E38\u3002");
          question.id = data.user_message_id;
          flight.assistantId = data.assistant_message_id;
        }
        if (event === "error") throw new Error(typeof data.error === "string" ? data.error : "\u751F\u6210\u5931\u8D25\uFF0C\u8BF7\u91CD\u8BD5\u3002");
        if (event === "delta") {
          if (typeof data.text !== "string") throw new Error("\u56DE\u590D\u683C\u5F0F\u5F02\u5E38\u3002");
          flight.text += data.text;
          paintStream(id);
        }
        if (event === "reasoning_delta") {
          if (typeof data.text !== "string") throw new Error("\u601D\u8003\u5185\u5BB9\u683C\u5F0F\u5F02\u5E38\u3002");
          flight.reasoning += data.text;
          paintStream(id);
        }
        if (event === "done") {
          if (typeof data.finish_reason !== "string" || !["stop", "length", "refusal", "other"].includes(data.finish_reason)) throw new Error("\u56DE\u590D\u7ED3\u675F\u72B6\u6001\u5F02\u5E38\u3002");
          if (!conversations.some((item) => item.id === id)) return true;
          if (!flight.text.trim() && data.finish_reason !== "refusal") throw new Error("\u6CA1\u6709\u6536\u5230\u6709\u6548\u56DE\u590D\uFF0C\u8BF7\u91CD\u8BD5\u3002");
          const reply = { id: typeof data.assistant_message_id === "string" ? data.assistant_message_id : flight.assistantId, state: "completed", role: "assistant", content: flight.text || "\u6A21\u578B\u62D2\u7EDD\u56DE\u7B54\u6B64\u95EE\u9898\u3002", reasoning: flight.reasoning, truncated: data.truncated === true };
          if (regenerate) conversation.messages[conversation.messages.length - 1] = reply;
          else conversation.messages.push(reply);
          return true;
        }
        return false;
      });
    } catch (error) {
      if (!conversations.some((item) => item.id === id)) return;
      if (!regenerate && (flight.text.trim() || flight.reasoning.trim())) {
        conversation.messages.push({ role: "assistant", content: flight.text, reasoning: flight.reasoning, interrupted: true });
      }
      if (flight.stopped) notices.set(id, regenerate ? "\u5DF2\u505C\u6B62\uFF0C\u539F\u56DE\u590D\u5DF2\u4FDD\u7559\u3002" : "\u5DF2\u505C\u6B62\u751F\u6210\u3002");
      else failures.set(id, error instanceof TypeError ? "\u8FDE\u63A5\u5931\u8D25\uFF0C\u8BF7\u786E\u8BA4\u540E\u7AEF\u670D\u52A1\u53EF\u7528\u3002" : errorMessage(error));
    } finally {
      if (pending.get(id) === flight) pending.delete(id);
      if (currentUser && conversations.includes(conversation)) {
        try {
          await refreshConversation(conversation);
        } catch (error) {
          historyError = errorMessage(error);
        }
      }
      render();
      if (activeId === id) {
        scrollToEnd();
        if (!prompt.disabled) prompt.focus();
      }
    }
  }
  $("#chat-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const text = prompt.value.trim();
    if (!text || prompt.disabled || !selectedModel()) return;
    let conversation = currentConversation();
    const nextMessages = modelHistory([...conversation?.messages || [], { role: "user", content: text }]);
    if (nextMessages.length > 200 || new TextEncoder().encode(JSON.stringify({ messages: nextMessages })).length > 240 * 1024) {
      formError = "\u5BF9\u8BDD\u5185\u5BB9\u8FC7\u957F\uFF0C\u8BF7\u65B0\u5EFA\u5BF9\u8BDD\u7EE7\u7EED\u3002";
      updateComposer();
      return;
    }
    if (!conversation) {
      sendButton.disabled = true;
      try {
        const data = await api("/api/conversations", { title: text.replace(/\s+/g, " ").slice(0, 28) });
        conversation = { ...data.conversation, messages: [], loaded: true };
      } catch (error) {
        formError = errorMessage(error);
        updateComposer();
        return;
      }
      conversations.unshift(conversation);
      activeId = conversation.id;
      updateConversationUrl(activeId);
      drafts.delete("new");
    }
    conversation.messages.push({ role: "user", content: text });
    prompt.value = "";
    drafts.delete(activeId);
    resizePrompt();
    requestReply(conversation);
  });
  prompt.addEventListener("input", () => {
    formError = "";
    resizePrompt();
    updateComposer();
  });
  prompt.addEventListener("keydown", (event) => {
    if (event.key === "Enter" && !event.shiftKey && !event.isComposing) {
      event.preventDefault();
      if (!sendButton.disabled) $("#chat-form").requestSubmit();
    }
  });
  retryButton.addEventListener("click", () => {
    const conversation = currentConversation();
    if (conversation) requestReply(conversation, { regenerate: conversation.messages.at(-1)?.role === "assistant" });
  });
  $("#new-chat").addEventListener("click", newConversation);
  window.addEventListener("popstate", () => {
    if (currentUser) switchConversation(conversationIdFromUrl(), true);
  });
  $("#brand").addEventListener("click", (event) => {
    event.preventDefault();
    newConversation();
  });
  $("#open-sidebar").addEventListener("click", () => {
    if (window.innerWidth <= 720) {
      $("#sidebar").inert = false;
      shell.classList.add("sidebar-open");
      $("#sidebar-overlay").hidden = false;
      $("#open-sidebar").setAttribute("aria-expanded", "true");
      $("#close-sidebar").focus();
    } else {
      shell.classList.remove("sidebar-collapsed");
      $("#open-sidebar").setAttribute("aria-expanded", "true");
    }
  });
  $("#close-sidebar").addEventListener("click", () => {
    closeSidebar();
    if (window.innerWidth > 720) shell.classList.add("sidebar-collapsed");
    $("#open-sidebar").setAttribute("aria-expanded", "false");
    $("#open-sidebar").focus();
  });
  $("#sidebar-overlay").addEventListener("click", closeSidebar);
  document.addEventListener("keydown", (event) => {
    if (event.key !== "Escape") return;
    if ($("#account-menu").matches(":popover-open")) {
      event.preventDefault();
      closeAccountMenu();
      $("#open-account").focus();
    } else closeSidebar();
  });
  window.addEventListener("resize", () => {
    closeModelMenu();
    closeSidebar();
    resizePrompt();
  });
  closeSidebar();
  render();
  resizePrompt();
  setAuthMode("login");
  api("/api/auth/me").then((data) => startSession(data.user)).catch((error) => {
    if (errorCode(error) !== "unauthenticated") feedback($("#auth-status"), errorMessage(error) || "\u65E0\u6CD5\u8FDE\u63A5\u670D\u52A1\u3002");
  });
})();
