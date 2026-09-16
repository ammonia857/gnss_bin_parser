/* GNSS 解析界面前端逻辑：无框架、无构建，直接由 /static/app.js 提供给浏览器。
 * 数据来源：GET /api/jobs 全量快照 + GET /api/events（SSE）增量事件；
 * 每次收到 done/failed 事件会重新拉取快照做一次校准，避免事件丢失导致界面失真。
 */
"use strict";

var API = {
  settings: "/api/settings",
  jobs: "/api/jobs",
  jobsClear: "/api/jobs/clear",
  jobsStart: "/api/jobs/start",
  events: "/api/events",
  pickFiles: "/api/pick/files",
  pickFolder: "/api/pick/folder",
  pickOutdir: "/api/pick/outdir",
  upload: "/api/upload",
  open: "/api/open",
  summary: function (id) { return "/api/results/" + encodeURIComponent(id) + "/summary"; },
  action: function (id, act) { return "/api/jobs/" + encodeURIComponent(id) + "/" + act; }
};

var STATUS_TEXT = {
  queued: "排队中", running: "解析中", done: "完成",
  failed: "失败", cancelled: "已取消", interrupted: "已中断"
};

var state = {
  jobs: [],
  settings: {},
  selectedId: null,
  summary: null,          // 当前选中任务的概览
  summaryFor: null,       // 概览对应的任务 id
  summaryLoading: false,
  uploads: {}             // 上传中的文件：name -> 0..1
};

/* ---------- 小工具 ---------- */

function $(id) { return document.getElementById(id); }

function fmtSize(bytes) {
  if (!bytes && bytes !== 0) return "-";
  var units = ["B", "KB", "MB", "GB", "TB"], n = bytes, i = 0;
  while (n >= 1024 && i < units.length - 1) { n /= 1024; i++; }
  return (i === 0 ? n : n.toFixed(1)) + " " + units[i];
}

function fmtInt(n) { return (n === null || n === undefined) ? "-" : Number(n).toLocaleString("zh-CN"); }

function fmtDuration(sec) {
  if (!sec) return "-";
  if (sec < 60) return sec.toFixed(1) + " s";
  var m = Math.floor(sec / 60), s = Math.round(sec - m * 60);
  return m + " min " + s + " s";
}

function jobById(id) {
  for (var i = 0; i < state.jobs.length; i++) { if (state.jobs[i].id === id) return state.jobs[i]; }
  return null;
}

function el(tag, cls, text) {
  var node = document.createElement(tag);
  if (cls) node.className = cls;
  if (text !== undefined && text !== null) node.textContent = text;
  return node;
}

/* ---------- 提示条 ---------- */

var bannerTimer = null;
function showBanner(text, warn) {
  var box = $("banner");
  box.textContent = text;
  box.className = "banner" + (warn ? " warn" : "");
  if (bannerTimer) clearTimeout(bannerTimer);
  bannerTimer = setTimeout(function () { box.className = "banner hidden"; }, warn ? 8000 : 12000);
}

/* ---------- HTTP ---------- */

function request(method, url, body) {
  var opts = { method: method, headers: {} };
  if (body !== undefined) {
    opts.headers["Content-Type"] = "application/json";
    opts.body = JSON.stringify(body);
  }
  return fetch(url, opts).then(function (resp) {
    return resp.text().then(function (text) {
      var data = {};
      try { data = text ? JSON.parse(text) : {}; } catch (e) { data = { error: text }; }
      if (!resp.ok) throw new Error(data.error || (resp.status + " " + resp.statusText));
      return data;
    });
  });
}

/* ---------- 设置 ---------- */

function loadSettings() {
  return request("GET", API.settings).then(function (s) {
    state.settings = s || {};
    $("engine-path").value = s.enginePath || "";
    $("output-dir").value = s.outputDir || "";
    $("auto-start").checked = !!s.autoStart;
    if (s.lastError) showBanner("上次运行的内部提示：" + s.lastError, true);
    updateStartButton();
  }).catch(function (e) { showBanner("读取设置失败：" + e.message); });
}

function saveSettings(patch) {
  return request("POST", API.settings, patch).then(function (s) {
    state.settings = s || {};
    $("auto-start").checked = !!s.autoStart;
    updateStartButton();
    return s;
  }).catch(function (e) { showBanner("保存设置失败：" + e.message); });
}

/* ---------- 任务列表 ---------- */

function refreshJobs() {
  return request("GET", API.jobs).then(function (data) {
    state.jobs = (data && data.jobs) || [];
    renderQueue();
    renderDetail();
    updateStartButton();
  }).catch(function (e) { showBanner("读取任务列表失败：" + e.message); });
}

function statusBadge(status) {
  var badge = el("span", "badge " + status, STATUS_TEXT[status] || status);
  return badge;
}

function jobProgress(job) {
  if (job.status === "done") return 100;
  if (typeof job.progress === "number") return job.progress;
  return 0;
}

function actionButton(label, title, handler) {
  var b = el("button", "btn small ghost", label);
  b.title = title || label;
  b.addEventListener("click", function (ev) { ev.stopPropagation(); handler(); });
  return b;
}

function jobAction(id, act, label) {
  request("POST", API.action(id, act)).then(function (data) {
    if (data && data.jobs) { state.jobs = data.jobs; renderQueue(); renderDetail(); }
    else refreshJobs();
  }).catch(function (e) { showBanner("操作失败：" + e.message); });
}

function renderQueue() {
  var list = $("job-list");
  list.innerHTML = "";
  $("queue-empty").className = state.jobs.length ? "empty hidden" : "empty";
  var running = 0, queued = 0;
  state.jobs.forEach(function (job) {
    if (job.status === "running") running++;
    if (job.status === "queued") queued++;
  });
  $("queue-count").textContent = state.jobs.length
    ? "（共 " + state.jobs.length + "，进行中 " + running + "，排队 " + queued + "）" : "";

  state.jobs.forEach(function (job) {
    var li = el("li", "job" + (job.id === state.selectedId ? " selected" : ""));
    li.addEventListener("click", function () { selectJob(job.id); });

    var head = el("div", "job-head");
    head.appendChild(el("span", "job-name", job.inputName));
    head.appendChild(el("span", "job-size", fmtSize(job.sizeBytes)));
    head.appendChild(statusBadge(job.status));
    li.appendChild(head);

    var bar = el("div", "progress");
    var fill = el("i");
    fill.style.width = Math.max(0, Math.min(100, jobProgress(job))) + "%";
    bar.appendChild(fill);
    li.appendChild(bar);

    var foot = el("div", "job-foot");
    foot.appendChild(el("span", null, jobProgress(job).toFixed(1) + "%"));
    if (job.status === "running" || job.status === "done") {
      foot.appendChild(el("span", null, "· 帧 " + fmtInt(job.stats && job.stats.totalFrames)));
    }
    foot.appendChild(el("span", "spacer"));
    var actions = el("span", "job-actions");
    if (job.status === "queued" || job.status === "running") {
      actions.appendChild(actionButton("取消", "停止该任务", function () { jobAction(job.id, "cancel"); }));
    }
    if (job.status === "failed" || job.status === "cancelled" || job.status === "interrupted") {
      actions.appendChild(actionButton("重试", "重新排队", function () { jobAction(job.id, "retry"); }));
    }
    if (job.status !== "running") {
      actions.appendChild(actionButton("打开目录", "在资源管理器中打开输出目录", function () { openPath(job.outputDir); }));
      actions.appendChild(actionButton("删除", "从列表移除", function () { jobAction(job.id, "remove"); }));
    }
    foot.appendChild(actions);
    li.appendChild(foot);
    list.appendChild(li);
  });
}

function updateStartButton() {
  var btn = $("btn-start");
  var queued = state.jobs.filter(function (j) { return j.status === "queued"; }).length;
  var manual = !state.settings.autoStart;
  btn.classList.toggle("hidden", !manual);
  btn.disabled = queued === 0;
  btn.textContent = queued ? "开始解析（" + queued + "）" : "开始解析";
}

/* ---------- 详情 ---------- */

function selectJob(id) {
  state.selectedId = id;
  if (state.summaryFor !== id) { state.summary = null; state.summaryFor = null; }
  renderQueue();
  renderDetail();
  var job = jobById(id);
  if (job && job.status === "done") loadSummary(id);
}

function card(k, v) {
  var box = el("div", "card");
  box.appendChild(el("div", "k", k));
  box.appendChild(el("div", "v", v));
  return box;
}

function renderDetail() {
  var job = state.selectedId ? jobById(state.selectedId) : null;
  $("detail-empty").className = job ? "empty hidden" : "empty";
  $("detail-body").className = job ? "" : "hidden";
  if (!job) { $("detail-name").textContent = ""; return; }

  $("detail-name").textContent = "· " + job.inputName;
  var s = job.stats || {};
  var cards = $("stat-cards");
  cards.innerHTML = "";
  cards.appendChild(card("状态", STATUS_TEXT[job.status] || job.status));
  cards.appendChild(card("总帧数", fmtInt(s.totalFrames)));
  cards.appendChild(card("RANGE 行", fmtInt(s.rangeRows)));
  cards.appendChild(card("SATVIS2 行", fmtInt(s.satvis2Rows)));
  cards.appendChild(card("BESTPOS 行", fmtInt(s.bestposRows)));
  cards.appendChild(card("CRC 错误", fmtInt(s.crcErrors)));
  cards.appendChild(card("非法/不支持", fmtInt((s.malformed || 0) + (s.unsupported || 0))));
  cards.appendChild(card("耗时", fmtDuration(s.elapsedSec)));
  if (s.elapsedSec && job.sizeBytes) {
    cards.appendChild(card("吞吐", (job.sizeBytes / 1048576 / s.elapsedSec).toFixed(1) + " MB/s"));
  }
  cards.appendChild(card("退出码", s.exitCode === null || s.exitCode === undefined ? "-" : s.exitCode));

  renderSystems();
  renderFiles();

  var msg = $("job-message");
  var text = job.message || "";
  if (job.warnings && job.warnings.length) text += (text ? "\n" : "") + job.warnings.join("\n");
  msg.textContent = text;
  msg.className = text ? "job-message" : "job-message hidden";
}

function renderSystems() {
  var box = $("system-bars");
  box.innerHTML = "";
  var summary = state.summary;
  var datasets = (summary && summary.datasets) || {};
  var ds = datasets.range || datasets.satvis2 || null;
  if (!ds || !ds.rows) {
    box.appendChild(el("div", "empty", state.summaryLoading ? "正在统计 CSV…" : "暂无数据（解析完成后显示）"));
    return;
  }
  var names = ds.systemNames || {};
  var keys = Object.keys(names);
  if (!keys.length) { box.appendChild(el("div", "empty", "CSV 中未找到星座列")); return; }
  var total = 0;
  keys.forEach(function (k) { total += names[k]; });
  var max = Math.max.apply(null, keys.map(function (k) { return names[k]; }));
  keys.sort(function (a, b) { return names[b] - names[a]; }).forEach(function (k) {
    var row = el("div", "bar-row");
    row.appendChild(el("span", null, k));
    var track = el("div", "bar-track");
    var fill = el("div", "bar-fill");
    fill.style.width = (max ? (names[k] / max * 100) : 0).toFixed(2) + "%";
    track.appendChild(fill);
    row.appendChild(track);
    row.appendChild(el("span", null, fmtInt(names[k]) + "（" + (total ? (names[k] / total * 100).toFixed(1) : "0") + "%）"));
    box.appendChild(row);
  });
}

function renderFiles() {
  var list = $("file-list");
  list.innerHTML = "";
  var files = (state.summary && state.summary.files) || [];
  var stats = (jobById(state.selectedId) || {}).stats || {};
  if (!files.length && stats.outputFiles && stats.outputFiles.length) {
    files = stats.outputFiles.map(function (p) { return { name: p.replace(/^.*[\\/]/, ""), path: p, sizeBytes: null }; });
  }
  if (!files.length) { list.appendChild(el("li", null, "解析完成后这里会列出 CSV")); return; }
  files.forEach(function (f) {
    var li = el("li");
    li.appendChild(el("span", "name", f.name));
    li.appendChild(el("span", "muted", fmtSize(f.sizeBytes)));
    li.appendChild(actionButton("打开", "用默认程序打开", function () { openPath(f.path); }));
    list.appendChild(li);
  });
}

function loadSummary(id) {
  if (state.summaryLoading) return;
  state.summaryLoading = true;
  renderSystems();
  request("GET", API.summary(id)).then(function (data) {
    state.summaryLoading = false;
    if (state.selectedId !== id) return;      // 期间切换了任务：丢弃过期结果
    state.summary = data;
    state.summaryFor = id;
    renderSystems();
    renderFiles();
  }).catch(function (e) {
    state.summaryLoading = false;
    showBanner("统计 CSV 失败：" + e.message);
  });
}

function openPath(path) {
  if (!path) return;
  request("POST", API.open, { path: path })
    .catch(function (e) { showBanner("打开失败：" + e.message); });
}

/* ---------- 拖拽上传 ---------- */

function uploadOne(file) {
  return new Promise(function (resolve, reject) {
    var xhr = new XMLHttpRequest();
    xhr.open("POST", API.upload);
    xhr.setRequestHeader("X-Filename", encodeURIComponent(file.name));
    xhr.upload.onprogress = function (ev) {
      if (ev.lengthComputable) { state.uploads[file.name] = ev.loaded / ev.total; renderUploads(); }
    };
    xhr.onload = function () {
      var data = {};
      try { data = JSON.parse(xhr.responseText || "{}"); } catch (e) { data = {}; }
      if (xhr.status >= 200 && xhr.status < 300 && data.path) resolve(data);
      else reject(new Error(data.error || ("HTTP " + xhr.status)));
    };
    xhr.onerror = function () { reject(new Error("网络中断")); };
    xhr.send(file);
  });
}

function handleFiles(fileList) {
  var files = Array.prototype.slice.call(fileList || []);
  if (!files.length) return;
  var bins = files.filter(function (f) { return /\.bin$/i.test(f.name); });
  var skipped = files.length - bins.length;
  if (skipped > 0) showBanner("已忽略 " + skipped + " 个非 .bin 文件", true);
  if (!bins.length) return;

  var chain = Promise.resolve();
  bins.forEach(function (file) {
    state.uploads[file.name] = 0;
    renderUploads();
    chain = chain.then(function () {
      return uploadOne(file).then(function (data) {
        delete state.uploads[file.name];
        renderUploads();
        return request("POST", API.jobs, {
          paths: [data.path],
          outputDir: $("output-dir").value.trim(),
          uploaded: true
        });
      }).catch(function (e) {
        delete state.uploads[file.name];
        renderUploads();
        showBanner("上传失败（" + file.name + "）：" + e.message);
      });
    });
  });
  chain.then(refreshJobs);
}

function renderUploads() {
  var box = $("uploads");
  box.innerHTML = "";
  Object.keys(state.uploads).forEach(function (name) {
    var item = el("div", "upload-item");
    item.appendChild(el("div", null, "上传中 " + name + " " + Math.round(state.uploads[name] * 100) + "%"));
    var bar = el("div", "progress");
    var fill = el("i");
    fill.style.width = (state.uploads[name] * 100).toFixed(1) + "%";
    bar.appendChild(fill);
    item.appendChild(bar);
    box.appendChild(item);
  });
}

function initDrag() {
  var overlay = $("drop-overlay");
  var depth = 0;
  window.addEventListener("dragenter", function (ev) {
    if (!ev.dataTransfer) return;
    ev.preventDefault();
    depth++;
    overlay.className = "drop-overlay";
  });
  window.addEventListener("dragover", function (ev) { ev.preventDefault(); });
  window.addEventListener("dragleave", function () {
    depth = Math.max(0, depth - 1);
    if (depth === 0) overlay.className = "drop-overlay hidden";
  });
  window.addEventListener("drop", function (ev) {
    ev.preventDefault();
    depth = 0;
    overlay.className = "drop-overlay hidden";
    if (ev.dataTransfer) handleFiles(ev.dataTransfer.files);
  });
}

/* ---------- SSE ---------- */

function connectEvents() {
  if (typeof EventSource === "undefined") return;      // 老浏览器：退化为手动刷新
  var es = new EventSource(API.events);
  es.onmessage = function (ev) {
    var data;
    try { data = JSON.parse(ev.data); } catch (e) { return; }
    if (data.type === "hello") {
      state.jobs = data.jobs || [];
      if (data.settings) {
        state.settings = data.settings;
        $("auto-start").checked = !!data.settings.autoStart;
      }
      renderQueue(); renderDetail(); updateStartButton();
    } else if (data.type === "job") {
      var job = data.job;
      var replaced = false;
      for (var i = 0; i < state.jobs.length; i++) {
        if (state.jobs[i].id === job.id) { state.jobs[i] = job; replaced = true; break; }
      }
      if (!replaced) state.jobs.push(job);
      renderQueue();
      updateStartButton();
      if (job.id === state.selectedId) renderDetail();
      if (job.status === "done" && job.id === state.selectedId) {
        state.summary = null; state.summaryFor = null;
        loadSummary(job.id);
      }
    } else if (data.type === "progress") {
      var target = jobById(data.id);
      if (target) {
        target.progress = data.progress;
        if (data.stats) target.stats = data.stats;
        renderQueue();
        if (data.id === state.selectedId) renderDetail();
      }
    }
  };
  es.onerror = function () { refreshJobs(); };          // EventSource 会自动重连，这里只做一次校准
}

/* ---------- 绑定 ---------- */

function bindControls() {
  $("btn-save-engine").addEventListener("click", function () {
    saveSettings({ enginePath: $("engine-path").value.trim() })
      .then(function () { showBanner("引擎路径已保存", true); });
  });
  $("btn-redetect").addEventListener("click", function () {
    $("engine-path").value = "";
    saveSettings({ enginePath: "" }).then(function () { showBanner("已恢复自动探测引擎", true); });
  });
  $("btn-pick-outdir").addEventListener("click", function () {
    request("POST", API.pickOutdir).then(function (data) {
      if (data && data.path) {
        $("output-dir").value = data.path;
        saveSettings({ outputDir: data.path });
      }
    }).catch(function (e) { showBanner("选择目录失败：" + e.message); });
  });
  $("output-dir").addEventListener("change", function () {
    saveSettings({ outputDir: $("output-dir").value.trim() });
  });
  $("auto-start").addEventListener("change", function () {
    saveSettings({ autoStart: $("auto-start").checked });
  });
  $("btn-pick-files").addEventListener("click", function () {
    request("POST", API.pickFiles).then(function (data) {
      var paths = (data && data.paths) || [];
      if (!paths.length) return;
      return request("POST", API.jobs, { paths: paths, outputDir: $("output-dir").value.trim() })
        .then(refreshJobs);
    }).catch(function (e) { showBanner("选择文件失败：" + e.message); });
  });
  $("btn-pick-folder").addEventListener("click", function () {
    request("POST", API.pickFolder).then(function (data) {
      var paths = (data && data.paths) || [];
      if (!paths.length) { showBanner("该文件夹里没有 .bin 文件", true); return; }
      return request("POST", API.jobs, { paths: paths, outputDir: $("output-dir").value.trim() })
        .then(refreshJobs);
    }).catch(function (e) { showBanner("选择文件夹失败：" + e.message); });
  });
  $("btn-start").addEventListener("click", function () {
    request("POST", API.jobsStart).then(refreshJobs)
      .catch(function (e) { showBanner("启动失败：" + e.message); });
  });
  $("btn-clear").addEventListener("click", function () {
    request("POST", API.jobsClear).then(refreshJobs)
      .catch(function (e) { showBanner("清空失败：" + e.message); });
  });
}

function boot() {
  bindControls();
  initDrag();
  loadSettings().then(refreshJobs).then(connectEvents);
}

if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", boot);
else boot();
