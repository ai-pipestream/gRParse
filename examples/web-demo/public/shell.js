// Demo-shell tab bar. The bridge only loads this script when DEMO_UIS is
// set (it injects <meta name="shell" content="on"> and this script tag into
// the entry page); without it the page is exactly the standalone demo.
//
// The first tab is this page's own gRParse demo; every other tab is a
// registered service whose frontend renders in an iframe below, proxied
// same-origin under /ui/<name>/ so no CSS or JS leaks between tabs.
"use strict";

const shellMeta = document.querySelector('meta[name="shell"]');

if (shellMeta && shellMeta.content === "on") {
  const header = document.querySelector("header");

  // Wrap the demo's own markup so tab switching is one hidden toggle. The
  // wrapper is built here (not in index.html) so the static page stays
  // byte-identical to the standalone demo for non-shell deployments.
  const grparseBody = document.createElement("div");
  grparseBody.id = "grparse-body";
  let node = header.nextSibling;
  while (node) {
    const next = node.nextSibling;
    if (node.nodeType !== Node.ELEMENT_NODE || node.tagName !== "SCRIPT") {
      grparseBody.appendChild(node);
    }
    node = next;
  }

  const tabStrip = document.createElement("nav");
  tabStrip.className = "tab-strip";
  tabStrip.setAttribute("role", "tablist");
  const paneHost = document.createElement("div");
  paneHost.className = "tab-panes";
  header.after(tabStrip, paneHost, grparseBody);

  const tabs = [];
  const panes = new Map();

  function makeTab(info) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "tab";
    button.setAttribute("role", "tab");
    button.title = info.description || "";
    const dot = document.createElement("span");
    dot.className = `tab-dot ${info.reachable ? "ok" : "bad"}`;
    dot.title = info.reachable ? "service reachable" : "service unreachable";
    const label = document.createElement("span");
    label.textContent = info.title || info.name;
    button.append(dot, label);
    return { info, button, dot };
  }

  const ownTab = makeTab({
    name: "grparse",
    title: "gRParse",
    description: "diskless page-streamed document parsing",
    reachable: true,
  });
  tabs.push(ownTab);
  tabStrip.appendChild(ownTab.button);

  function select(tab) {
    for (const other of tabs) other.button.classList.toggle("active", other === tab);
    if (tab === ownTab) {
      grparseBody.hidden = false;
      for (const pane of panes.values()) pane.hidden = true;
      return;
    }
    grparseBody.hidden = true;
    let pane = panes.get(tab.info.name);
    if (!pane) {
      // Lazy: the iframe only loads when the tab is first opened, and stays
      // alive afterwards so a running job in another tab is not lost.
      pane = document.createElement("iframe");
      pane.className = "tab-pane";
      pane.src = tab.info.path.endsWith("/") ? tab.info.path : `${tab.info.path}/`;
      pane.title = tab.info.title || tab.info.name;
      panes.set(tab.info.name, pane);
      paneHost.appendChild(pane);
    }
    for (const [name, other] of panes) other.hidden = name !== tab.info.name;
  }

  function applyUis(uis) {
    for (const info of uis || []) {
      let tab = tabs.find((candidate) => candidate.info.name === info.name);
      if (!tab) {
        tab = makeTab(info);
        tab.button.addEventListener("click", () => select(tab));
        tabs.push(tab);
        tabStrip.appendChild(tab.button);
        continue;
      }
      // Refresh in place: reachability, title, tooltip follow the service.
      Object.assign(tab.info, info);
      tab.dot.className = `tab-dot ${info.reachable ? "ok" : "bad"}`;
      tab.dot.title = info.reachable ? "service reachable" : "service unreachable";
      tab.button.lastChild.textContent = info.title || info.name;
      tab.button.title = info.description || "";
    }
  }

  function refresh() {
    return fetch("/api/uis")
      .then((response) => (response.ok ? response.json() : null))
      .then((data) => { if (data) applyUis(data.uis); })
      .catch(() => {});
  }

  ownTab.button.addEventListener("click", () => select(ownTab));
  refresh().finally(() => select(ownTab));
  // Status dots track the services without disturbing the open pane.
  setInterval(refresh, 5000);
}
