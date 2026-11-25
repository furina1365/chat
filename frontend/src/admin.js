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

  // src/admin.ts
  var loginForm = $("#admin-login-form");
  var searchForm = $("#admin-search");
  var actionForm = $("#admin-action-form");
  var dialog = $("#admin-dialog");
  var searchInput = $("#user-search");
  var users = [];
  var nextId = null;
  var search = "";
  var loading = false;
  var selected = null;
  var actionBusy = false;
  var epoch = 0;
  var RequestError = class extends Error {
    constructor(message2, status2) {
      super(message2);
      __publicField(this, "status", status2);
    }
  };
  function message(error) {
    return error instanceof Error ? error.message : "\u8BF7\u6C42\u5931\u8D25\uFF0C\u8BF7\u7A0D\u540E\u91CD\u8BD5\u3002";
  }
  function status(text = "", success = false) {
    $("#admin-status").textContent = text;
    $("#admin-status").classList.toggle("success", success);
  }
  async function request(path, body) {
    const response = await fetch(path, {
      method: body === void 0 ? "GET" : "POST",
      credentials: "same-origin",
      headers: body === void 0 ? {} : { "Content-Type": "application/json", "X-Chat-Request": "1" },
      body: body === void 0 ? void 0 : JSON.stringify(body)
    });
    const data = await response.json();
    if (!response.ok) {
      if (response.status === 401 && path !== "/api/admin/login") showLogin();
      throw new RequestError(typeof data.error === "string" ? data.error : "\u8BF7\u6C42\u5931\u8D25\uFF0C\u8BF7\u7A0D\u540E\u91CD\u8BD5\u3002", response.status);
    }
    return data;
  }
  function busy(form, value) {
    form.querySelectorAll("input, button").forEach((control) => {
      control.disabled = value;
    });
  }
  function showLogin() {
    ++epoch;
    users = [];
    nextId = null;
    selected = null;
    $("#admin-users").replaceChildren();
    $("#admin-panel").hidden = true;
    $("#admin-initial").hidden = true;
    $("#admin-login").hidden = false;
    dialog.close();
    formInput(loginForm, "password").focus();
  }
  async function showPanel() {
    ++epoch;
    $("#admin-initial").hidden = true;
    $("#admin-login").hidden = true;
    $("#admin-panel").hidden = false;
    loginForm.reset();
    search = "";
    searchInput.value = "";
    await loadUsers();
  }
  function renderUsers() {
    const fragment = document.createDocumentFragment();
    for (const user of users) {
      const row = document.createElement("tr");
      const identity = row.insertCell();
      const name = document.createElement("span");
      name.className = "admin-user-name";
      name.textContent = user.username;
      const id = document.createElement("span");
      id.className = "admin-user-id";
      id.textContent = `ID ${user.id}`;
      identity.append(name, id);
      const email = row.insertCell();
      email.className = "admin-email";
      email.textContent = user.email || "\u672A\u7ED1\u5B9A\u90AE\u7BB1";
      const state = row.insertCell();
      state.className = "admin-state";
      const badge = document.createElement("span");
      badge.className = `admin-badge${user.disabled ? " disabled" : ""}`;
      badge.textContent = user.disabled ? "\u5DF2\u505C\u7528" : "\u6B63\u5E38";
      state.append(badge);
      const date = row.insertCell();
      date.className = "admin-date";
      date.textContent = new Date(user.created_at).toLocaleDateString("zh-CN");
      date.title = new Date(user.created_at).toLocaleString("zh-CN");
      const actions = document.createElement("div");
      actions.className = "admin-row-actions";
      for (const [action, label] of [["rename", "\u6539\u540D"], ["logout", "\u5F3A\u5236\u9000\u51FA"], [user.disabled ? "enable" : "disable", user.disabled ? "\u542F\u7528" : "\u505C\u7528"]]) {
        const button = document.createElement("button");
        button.type = "button";
        button.className = `text-button${action === "disable" ? " danger" : ""}`;
        button.textContent = label;
        button.setAttribute("aria-label", `${label} ${user.username}`);
        button.addEventListener("click", () => openAction(user, action));
        actions.append(button);
      }
      row.insertCell().append(actions);
      fragment.append(row);
    }
    $("#admin-users").replaceChildren(fragment);
    $("#admin-empty").hidden = users.length > 0 || loading;
    $("#admin-count").textContent = `\u5DF2\u663E\u793A ${users.length} \u4F4D\u7528\u6237`;
    $("#admin-more").hidden = !nextId;
  }
  async function loadUsers(more = false) {
    if (loading) return;
    const version = epoch;
    loading = true;
    busy(searchForm, true);
    $("#admin-more").disabled = true;
    status("\u6B63\u5728\u52A0\u8F7D\u2026");
    if (!more) {
      users = [];
      nextId = null;
      renderUsers();
    }
    try {
      const params = new URLSearchParams({ search });
      if (more && nextId) params.set("before_id", nextId);
      const data = await request(`/api/admin/users?${params}`);
      if (version !== epoch) return;
      users = more ? [...users, ...data.items] : data.items;
      nextId = data.next_before_id;
      status();
    } catch (error) {
      if (version === epoch) status(message(error));
      else if (error instanceof RequestError && error.status === 401) $("#login-status").textContent = error.message;
    } finally {
      loading = false;
      busy(searchForm, false);
      $("#admin-more").disabled = false;
      if (version === epoch) renderUsers();
    }
  }
  function openAction(user, action) {
    selected = { user, action };
    const labels = { rename: "\u4FEE\u6539\u7528\u6237\u540D", disable: "\u505C\u7528\u8D26\u53F7", enable: "\u542F\u7528\u8D26\u53F7", logout: "\u5F3A\u5236\u9000\u51FA" };
    $("#dialog-title").textContent = labels[action];
    $("#dialog-description").textContent = {
      rename: `\u4FEE\u6539 ${user.username} \u7684\u7528\u6237\u540D\u3002`,
      disable: `\u505C\u7528 ${user.username} \u540E\uFF0C\u8BE5\u7528\u6237\u5C06\u9000\u51FA\u5168\u90E8\u8BBE\u5907\uFF0C\u65E0\u6CD5\u767B\u5F55\u3002\u5BF9\u8BDD\u8BB0\u5F55\u4F1A\u4FDD\u7559\u3002`,
      enable: `\u542F\u7528 ${user.username} \u540E\uFF0C\u8BE5\u7528\u6237\u53EF\u91CD\u65B0\u767B\u5F55\u3002`,
      logout: `\u5C06 ${user.username} \u9000\u51FA\u5168\u90E8\u8BBE\u5907\uFF0C\u8BE5\u7528\u6237\u53EF\u91CD\u65B0\u767B\u5F55\u3002`
    }[action];
    $("#dialog-username-label").hidden = action !== "rename";
    const input = formInput(actionForm, "username");
    input.disabled = action !== "rename";
    input.required = action === "rename";
    input.value = user.username;
    $("#dialog-status").textContent = "";
    $("#dialog-submit").textContent = action === "rename" ? "\u4FDD\u5B58" : labels[action];
    dialog.showModal();
    if (action === "rename") input.select();
    else $("#dialog-cancel").focus();
  }
  loginForm.addEventListener("submit", async (event) => {
    event.preventDefault();
    busy(loginForm, true);
    $("#login-status").textContent = "\u6B63\u5728\u767B\u5F55\u2026";
    try {
      await request("/api/admin/login", { password: formInput(loginForm, "password").value });
      $("#login-status").textContent = "";
      await showPanel();
    } catch (error) {
      $("#login-status").textContent = message(error);
    } finally {
      busy(loginForm, false);
    }
  });
  searchForm.addEventListener("submit", (event) => {
    event.preventDefault();
    search = searchInput.value.trim();
    void loadUsers();
  });
  $("#admin-refresh").addEventListener("click", () => {
    void loadUsers();
  });
  $("#admin-more").addEventListener("click", () => {
    void loadUsers(true);
  });
  $("#dialog-cancel").addEventListener("click", () => dialog.close());
  dialog.addEventListener("cancel", (event) => {
    if (actionBusy) event.preventDefault();
  });
  actionForm.addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!selected || actionBusy) return;
    const { user, action } = selected;
    const username = formInput(actionForm, "username").value;
    const version = epoch;
    actionBusy = true;
    busy(actionForm, true);
    $("#dialog-status").textContent = "\u6B63\u5728\u5904\u7406\u2026";
    try {
      if (action === "logout") await request("/api/admin/users/logout", { user_id: user.id });
      else {
        const data = await request("/api/admin/users/update", {
          user_id: user.id,
          ...action === "rename" ? { username } : { disabled: action === "disable" }
        });
        if (version !== epoch) return;
        users = users.map((item) => item.id === user.id ? data.user : item);
        renderUsers();
      }
      if (version !== epoch) return;
      dialog.close();
      status({ rename: "\u7528\u6237\u540D\u5DF2\u4FEE\u6539\u3002", disable: "\u8D26\u53F7\u5DF2\u505C\u7528\u3002", enable: "\u8D26\u53F7\u5DF2\u542F\u7528\u3002", logout: "\u5DF2\u9000\u51FA\u8BE5\u7528\u6237\u7684\u5168\u90E8\u8BBE\u5907\u3002" }[action], true);
    } catch (error) {
      if (version === epoch) $("#dialog-status").textContent = message(error);
      else $("#login-status").textContent = message(error);
    } finally {
      actionBusy = false;
      busy(actionForm, false);
      formInput(actionForm, "username").disabled = action !== "rename";
    }
  });
  $("#admin-logout").addEventListener("click", async () => {
    const button = $("#admin-logout");
    button.disabled = true;
    try {
      await request("/api/admin/logout", {});
      showLogin();
    } catch (error) {
      status(message(error));
    } finally {
      button.disabled = false;
    }
  });
  request("/api/admin/me").then(showPanel).catch((error) => {
    showLogin();
    if (!(error instanceof RequestError && error.status === 401)) $("#login-status").textContent = message(error);
  });
})();
