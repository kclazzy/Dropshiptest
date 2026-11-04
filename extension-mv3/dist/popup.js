// src/shared/orders.ts
function needsAction(order) {
  return ["QUEUED_FOR_EXT", "MANUAL_REVIEW", "ERROR", "ORDERED"].includes(order.status);
}
function addressLines(address) {
  return Object.entries(address).filter(([, v]) => v !== null && v !== "" && typeof v !== "object").map(([k, v]) => `${k}: ${String(v)}`).join("\n");
}

// src/popup/popup.ts
var $ = (id) => document.getElementById(id);
function say(text, error = false) {
  const el2 = $("message");
  el2.textContent = text;
  el2.className = error ? "error" : "";
}
async function send(request) {
  const reply = await chrome.runtime.sendMessage(request);
  if (!reply) throw new Error("\u0420\u0430\u0441\u0448\u0438\u0440\u0435\u043D\u0438\u0435 \u043D\u0435 \u043E\u0442\u0432\u0435\u0447\u0430\u0435\u0442");
  if (!reply.ok) throw new Error(reply.error);
  return reply.data;
}
async function activeTab() {
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  if (!tab?.id || !tab.url || !/^https?:/.test(tab.url)) throw new Error("\u041E\u0442\u043A\u0440\u043E\u0439\u0442\u0435 \u0441\u0442\u0440\u0430\u043D\u0438\u0446\u0443 \u0442\u043E\u0432\u0430\u0440\u0430 (http/https)");
  return tab;
}
async function refreshStatus() {
  const status = await send({ type: "status" });
  const el2 = $("status");
  el2.className = status.connected ? "status ok" : "status";
  el2.textContent = status.connected ? "\u0421\u0432\u044F\u0437\u044C \u0441 \u044F\u0434\u0440\u043E\u043C \u0443\u0441\u0442\u0430\u043D\u043E\u0432\u043B\u0435\u043D\u0430" : !status.hasToken ? "\u0412\u0441\u0442\u0430\u0432\u044C\u0442\u0435 \u0442\u043E\u043A\u0435\u043D \u0434\u043E\u0441\u0442\u0443\u043F\u0430 \u0432 \u043D\u0430\u0441\u0442\u0440\u043E\u0439\u043A\u0430\u0445" : `\u041D\u0435\u0442 \u0441\u0432\u044F\u0437\u0438 \u0441 \u044F\u0434\u0440\u043E\u043C. ${status.error ?? "\u0417\u0430\u043F\u0443\u0449\u0435\u043D\u043E \u043B\u0438 \u043F\u0440\u0438\u043B\u043E\u0436\u0435\u043D\u0438\u0435?"}`;
  if (!status.hasToken) $("settings").open = true;
  if (status.storeId !== null) $("store").value = String(status.storeId);
}
async function importCurrentPage() {
  const tab = await activeTab();
  await chrome.scripting.executeScript({ target: { tabId: tab.id }, files: ["extractor.js"] });
  const [injected] = await chrome.scripting.executeScript({
    target: { tabId: tab.id },
    func: () => globalThis.__dropshipExtract()
  });
  const result = injected?.result;
  if (!result) throw new Error("\u041D\u0435 \u0443\u0434\u0430\u043B\u043E\u0441\u044C \u043F\u0440\u043E\u0447\u0438\u0442\u0430\u0442\u044C \u0441\u0442\u0440\u0430\u043D\u0438\u0446\u0443");
  if (!result.ok) throw new Error(result.error);
  const saved = await send({ type: "import", product: result.product });
  say(
    saved.duplicate ? `\u042D\u0442\u043E\u0442 \u0442\u043E\u0432\u0430\u0440 \u0443\u0436\u0435 \u0434\u043E\u0431\u0430\u0432\u043B\u0435\u043D (\u2116${saved.product_id}). \u0414\u0430\u043D\u043D\u044B\u0435 \u043E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u044B.` : `\u0414\u043E\u0431\u0430\u0432\u043B\u0435\u043D\u043E: \xAB${result.product.title}\xBB
\u0412\u0430\u0440\u0438\u0430\u043D\u0442\u043E\u0432: ${saved.variants}. \u0418\u0418 \u0433\u043E\u0442\u043E\u0432\u0438\u0442 \u043E\u043F\u0438\u0441\u0430\u043D\u0438\u0435.`
  );
}
async function syncCookies() {
  const tab = await activeTab();
  const origin = new URL(tab.url).origin + "/*";
  const granted = await chrome.permissions.request({ origins: [origin], permissions: ["cookies"] });
  if (!granted) throw new Error("\u0411\u0435\u0437 \u0440\u0430\u0437\u0440\u0435\u0448\u0435\u043D\u0438\u044F \u043D\u0430 \u043A\u0443\u043A\u0438 \u0441\u0435\u0441\u0441\u0438\u044E \u043F\u0435\u0440\u0435\u0434\u0430\u0442\u044C \u043D\u0435\u043B\u044C\u0437\u044F");
  await send({ type: "sync-cookies", pageUrl: tab.url });
  say("\u0421\u0435\u0441\u0441\u0438\u044F \u0441\u0430\u0439\u0442\u0430 \u043F\u0435\u0440\u0435\u0434\u0430\u043D\u0430 \u0432 \u044F\u0434\u0440\u043E.");
}
async function checkUpdate() {
  const info = await send({ type: "update-check" });
  $("update").hidden = !info.update_available;
  if (!info.update_available) return;
  $("update-text").textContent = `\u0414\u043E\u0441\u0442\u0443\u043F\u043D\u0430 \u0432\u0435\u0440\u0441\u0438\u044F ${info.latest} (\u0443 \u0432\u0430\u0441 ${info.current}).`;
  const apply = $("update-apply");
  apply.hidden = !info.can_apply;
  if (!info.can_apply) $("update-text").textContent += " \u041E\u0431\u043D\u043E\u0432\u0438\u0442\u0435 \u043F\u0440\u0438\u043B\u043E\u0436\u0435\u043D\u0438\u0435: \u041D\u0430\u0441\u0442\u0440\u043E\u0439\u043A\u0438 \u2192 \u041E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u0438\u0435.";
}
async function applyUpdate() {
  await send({ type: "update-apply" });
  say("\u041E\u0431\u043D\u043E\u0432\u043B\u044F\u044E\u2026 \u042F\u0434\u0440\u043E \u043F\u0435\u0440\u0435\u0437\u0430\u043F\u0443\u0441\u0442\u0438\u0442\u0441\u044F \u0441\u0430\u043C\u043E, \u0440\u0430\u0441\u0448\u0438\u0440\u0435\u043D\u0438\u0435 \u043F\u0435\u0440\u0435\u0437\u0430\u0433\u0440\u0443\u0437\u0438\u0442\u0441\u044F \u0447\u0435\u0440\u0435\u0437 \u043C\u0438\u043D\u0443\u0442\u0443.");
  setTimeout(() => chrome.runtime.reload(), 6e4);
}
var STATUS_TEXT = {
  NEW: "\u041E\u0436\u0438\u0434\u0430\u0435\u0442 \u043E\u0442\u043F\u0440\u0430\u0432\u043A\u0438",
  QUEUED_FOR_EXT: "\u041D\u0443\u0436\u043D\u043E \u0432\u044B\u043A\u0443\u043F\u0438\u0442\u044C \u0443 \u043F\u043E\u0441\u0442\u0430\u0432\u0449\u0438\u043A\u0430",
  ORDERED: "\u0412\u044B\u043A\u0443\u043F\u043B\u0435\u043D \u2014 \u0443\u043A\u0430\u0436\u0438\u0442\u0435 \u0442\u0440\u0435\u043A",
  SHIPPED: "\u0422\u0440\u0435\u043A \u043F\u0435\u0440\u0435\u0434\u0430\u043D",
  MANUAL_REVIEW: "\u0422\u0440\u0435\u0431\u0443\u0435\u0442 \u0432\u0430\u0448\u0435\u0433\u043E \u0440\u0435\u0448\u0435\u043D\u0438\u044F",
  ERROR: "\u041E\u0448\u0438\u0431\u043A\u0430 \u0432\u044B\u043A\u0443\u043F\u0430"
};
function el(tag, text = "", className = "") {
  const node = document.createElement(tag);
  node.textContent = text;
  if (className) node.className = className;
  return node;
}
function actionRow(placeholder, button, onClick) {
  const row = el("div", "", "row");
  const input = el("input");
  input.placeholder = placeholder;
  const go = el("button", button);
  go.addEventListener("click", guarded(() => onClick(input.value.trim())));
  row.append(input, go);
  return row;
}
function renderOrder(order) {
  const card = el("div", "", "order");
  card.append(el("div", `\u2116${order.external_order_id} \xB7 ${order.title ?? "\u0442\u043E\u0432\u0430\u0440 \u043D\u0435 \u043F\u0440\u0438\u0432\u044F\u0437\u0430\u043D"} \xD7 ${order.quantity}`));
  card.append(el("div", STATUS_TEXT[order.status] ?? order.status, "st"));
  if (order.error_message) card.append(el("div", order.error_message, "note"));
  const address = addressLines(order.address);
  if (address && ["QUEUED_FOR_EXT", "MANUAL_REVIEW", "ERROR", "NEW"].includes(order.status)) {
    card.append(el("pre", address));
    const row = el("div", "", "row");
    const copy = el("button", "\u0421\u043A\u043E\u043F\u0438\u0440\u043E\u0432\u0430\u0442\u044C \u0430\u0434\u0440\u0435\u0441");
    copy.addEventListener("click", () => void navigator.clipboard.writeText(address).then(() => say("\u0410\u0434\u0440\u0435\u0441 \u0441\u043A\u043E\u043F\u0438\u0440\u043E\u0432\u0430\u043D.")));
    row.append(copy);
    if (order.supplier_url && /^https?:/.test(order.supplier_url)) {
      const open = el("button", `\u041E\u0442\u043A\u0440\u044B\u0442\u044C \u043F\u043E\u0441\u0442\u0430\u0432\u0449\u0438\u043A\u0430 (\u0430\u0440\u0442. ${order.sku ?? "?"})`);
      open.addEventListener("click", () => void chrome.tabs.create({ url: order.supplier_url }));
      row.append(open);
    }
    card.append(row);
  }
  if (["QUEUED_FOR_EXT", "MANUAL_REVIEW", "ERROR", "NEW"].includes(order.status)) {
    card.append(actionRow("\u2116 \u0437\u0430\u043A\u0430\u0437\u0430 \u0443 \u043F\u043E\u0441\u0442\u0430\u0432\u0449\u0438\u043A\u0430", "\u0412\u044B\u043A\u0443\u043F\u043B\u0435\u043D\u043E", async (value) => {
      if (!value) throw new Error("\u0412\u0432\u0435\u0434\u0438\u0442\u0435 \u043D\u043E\u043C\u0435\u0440 \u0437\u0430\u043A\u0430\u0437\u0430 \u0443 \u043F\u043E\u0441\u0442\u0430\u0432\u0449\u0438\u043A\u0430");
      await send({ type: "order-result", orderId: order.id, status: "ordered", supplierOrderId: value });
      say("\u0417\u0430\u043A\u0430\u0437 \u043E\u0442\u043C\u0435\u0447\u0435\u043D \u043A\u0430\u043A \u0432\u044B\u043A\u0443\u043F\u043B\u0435\u043D\u043D\u044B\u0439.");
    }));
    if (order.status === "QUEUED_FOR_EXT") {
      const fail = el("button", "\u041D\u0435 \u0443\u0434\u0430\u043B\u043E\u0441\u044C \u0432\u044B\u043A\u0443\u043F\u0438\u0442\u044C");
      fail.addEventListener("click", guarded(async () => {
        await send({ type: "order-result", orderId: order.id, status: "failed", message: "\u041E\u0442\u043C\u0435\u0447\u0435\u043D\u043E \u0432\u0440\u0443\u0447\u043D\u0443\u044E: \u0432\u044B\u043A\u0443\u043F \u043D\u0435 \u0443\u0434\u0430\u043B\u0441\u044F" });
        say("\u0417\u0430\u043A\u0430\u0437 \u043E\u0442\u043C\u0435\u0447\u0435\u043D \u043A\u0430\u043A \u043D\u0435\u0443\u0434\u0430\u0432\u0448\u0438\u0439\u0441\u044F.");
      }));
      card.append(fail);
    }
  }
  if (["ORDERED", "SHIPPED"].includes(order.status)) {
    card.append(actionRow(order.tracking_number ?? "\u0422\u0440\u0435\u043A-\u043D\u043E\u043C\u0435\u0440", "\u041E\u0442\u043F\u0440\u0430\u0432\u0438\u0442\u044C \u0442\u0440\u0435\u043A", async (value) => {
      if (!value) throw new Error("\u0412\u0432\u0435\u0434\u0438\u0442\u0435 \u0442\u0440\u0435\u043A-\u043D\u043E\u043C\u0435\u0440");
      await send({ type: "order-track", orderId: order.id, trackingNumber: value });
      say("\u0422\u0440\u0435\u043A \u0441\u043E\u0445\u0440\u0430\u043D\u0451\u043D, \u044F\u0434\u0440\u043E \u043F\u0435\u0440\u0435\u0434\u0430\u0441\u0442 \u0435\u0433\u043E \u0432 \u043C\u0430\u0433\u0430\u0437\u0438\u043D.");
    }));
  }
  return card;
}
async function loadOrders() {
  const reply = await send({ type: "orders-list" });
  const box = $("orders");
  box.replaceChildren();
  const active = reply.orders.filter((o) => needsAction(o));
  if (active.length === 0) box.append(el("div", "\u0417\u0430\u043A\u0430\u0437\u043E\u0432, \u0442\u0440\u0435\u0431\u0443\u044E\u0449\u0438\u0445 \u0434\u0435\u0439\u0441\u0442\u0432\u0438\u0439, \u043D\u0435\u0442."));
  for (const order of active) box.append(renderOrder(order));
  const waiting = reply.orders.filter((o) => o.status === "QUEUED_FOR_EXT").length;
  await chrome.action.setBadgeText({ text: waiting > 0 ? String(waiting) : "" });
}
function guarded(action) {
  return () => {
    action().then(refreshStatus).then(() => loadOrders().catch(() => void 0)).catch((e) => say(e instanceof Error ? e.message : String(e), true));
  };
}
$("orders-refresh").addEventListener("click", guarded(loadOrders));
$("update-apply").addEventListener("click", guarded(applyUpdate));
$("import").addEventListener("click", guarded(importCurrentPage));
$("cookies").addEventListener("click", guarded(syncCookies));
$("save").addEventListener("click", guarded(async () => {
  const storeText = $("store").value.trim();
  await send({
    type: "save-settings",
    storeId: storeText === "" ? null : Number(storeText)
  });
  say("\u0421\u043E\u0445\u0440\u0430\u043D\u0435\u043D\u043E.");
}));
$("pair").addEventListener("click", guarded(async () => {
  await send({ type: "pair", code: $("code").value });
  $("code").value = "";
  say("\u0420\u0430\u0441\u0448\u0438\u0440\u0435\u043D\u0438\u0435 \u043F\u0440\u0438\u0432\u044F\u0437\u0430\u043D\u043E.");
  await refreshStatus();
}));
refreshStatus().then(() => loadOrders().catch(() => void 0)).then(() => checkUpdate().catch(() => void 0)).catch((e) => say(e instanceof Error ? e.message : String(e), true));
