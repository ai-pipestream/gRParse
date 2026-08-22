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

  // Native tabs: services with no web UI of their own, carried by the
  // bridge itself. Each renders a page the bridge serves (/<name>.html)
  // talking to the service through /api/<name> endpoints. They show up
  // whether or not the server is reachable; each page carries its own
  // status badge, and the dot follows /api/<name>/status. A status payload
  // with vlmConfigured === false marks the tab amber: the service is up
  // but depends on an external VLM endpoint nobody configured.
  const NATIVE_TABS = [
    {
      name: "fastwarc",
      title: "FastWARC",
      path: "/fastwarc.html",
      description: "streaming WARC archive parsing via fastwarc-grpc",
      statusUrl: "/api/fastwarc/status",
    },
    {
      name: "poic",
      title: "POI",
      path: "/poic.html",
      description: "office document parsing via grPOIc (Apache POI)",
      statusUrl: "/api/poic/status",
    },
    {
      name: "asr",
      title: "ASR",
      path: "/asr.html",
      description: "streaming speech-to-text via grpc-asr (whisper.cpp)",
      statusUrl: "/api/asr/status",
    },
    {
      name: "enrich",
      title: "Enrich",
      path: "/enrich.html",
      description: "VLM annotations on parsed document items via grpc-enrich",
      statusUrl: "/api/enrich/status",
    },
    {
      name: "vlm-convert",
      title: "VLM Convert",
      path: "/vlm-convert.html",
      description: "vision-language-model page parsing via grpc-vlm-convert",
      statusUrl: "/api/vlm-convert/status",
    },
  ];

  const nativeTabs = NATIVE_TABS.map((spec) => {
    const tab = makeTab({
      name: spec.name,
      title: spec.title,
      path: spec.path,
      description: spec.description,
      reachable: false,
    });
    tab.native = true;
    tab.statusUrl = spec.statusUrl;
    tabs.push(tab);
    tabStrip.appendChild(tab.button);
    return tab;
  });

  function refreshNativeTab(tab) {
    return fetch(tab.statusUrl)
      .then((response) => (response.ok ? response.json() : null))
      .then((status) => {
        if (!status) return;
        tab.info.reachable = Boolean(status.reachable);
        const degraded = status.reachable && status.vlmConfigured === false;
        const state = status.reachable ? (degraded ? "warn" : "ok") : "bad";
        tab.dot.className = `tab-dot ${state}`;
        tab.dot.title = status.reachable
          ? (degraded ? "service reachable, no VLM endpoint configured" : "service reachable")
          : "service unreachable";
      })
      .catch(() => {});
  }

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
      // Native pages carry a full file path; proxied frontends mount under
      // their /ui/<name>/ directory and need the trailing slash.
      pane.src = tab.info.path.endsWith("/") || tab.info.path.endsWith(".html")
        ? tab.info.path
        : `${tab.info.path}/`;
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
      // A native tab of the same name wins over a proxy registry entry.
      if (tab.native) continue;
      // Refresh in place: reachability, title, tooltip follow the service.
      Object.assign(tab.info, info);
      tab.dot.className = `tab-dot ${info.reachable ? "ok" : "bad"}`;
      tab.dot.title = info.reachable ? "service reachable" : "service unreachable";
      tab.button.lastChild.textContent = info.title || info.name;
      tab.button.title = info.description || "";
    }
  }

  function refresh() {
    for (const tab of nativeTabs) refreshNativeTab(tab);
    return fetch("/api/uis")
      .then((response) => (response.ok ? response.json() : null))
      .then((data) => { if (data) applyUis(data.uis); })
      .catch(() => {});
  }

  ownTab.button.addEventListener("click", () => select(ownTab));
  for (const tab of nativeTabs) tab.button.addEventListener("click", () => select(tab));
  refresh().finally(() => select(ownTab));
  // Status dots track the services without disturbing the open pane.
  setInterval(refresh, 5000);
}
