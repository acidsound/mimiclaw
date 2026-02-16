#!/usr/bin/env node

import { execFile } from "node:child_process";
import { promisify } from "node:util";
import { promises as fs } from "node:fs";
import os from "node:os";
import path from "node:path";

const execFileAsync = promisify(execFile);

const WS_PORT = clampInt(process.env.MIMI_WS_PORT, 18789, 1, 65535);
const WS_URL = resolveWsUrl();
const HELPER_CHAT_ID = process.env.MIMI_HELPER_CHAT_ID || "ios_sim_helper";
const BOT_TOKEN = process.env.TELEGRAM_BOT_TOKEN || "";
const DEFAULT_OPEN_WAIT_MS = clampInt(process.env.MIMI_IOS_OPEN_WAIT_MS, 2500, 500, 15000);
const DEFAULT_WAIT_AFTER_TAP_MS = clampInt(
  process.env.MIMI_IOS_WAIT_AFTER_TAP_MS,
  1000,
  200,
  15000,
);
const DEFAULT_TIMEOUT_MS = clampInt(process.env.MIMI_IOS_TIMEOUT_MS, 30000, 1000, 60000);

if (!BOT_TOKEN) {
  console.error("[ios-sim-helper] TELEGRAM_BOT_TOKEN is required.");
  process.exit(1);
}

let inFlight = Promise.resolve();
let reconnectDelayMs = 1000;

function clampInt(rawValue, fallback, min, max) {
  const n = Number(rawValue);
  if (!Number.isFinite(n)) {
    return fallback;
  }
  return Math.min(Math.max(Math.floor(n), min), max);
}

function resolveWsUrl() {
  const explicitUrl = String(process.env.MIMI_WS_URL || "").trim();
  if (explicitUrl) {
    return explicitUrl;
  }

  const esp32Ip = String(process.env.ESP32_IP || "").trim();
  if (esp32Ip) {
    return `ws://${esp32Ip}:${WS_PORT}/`;
  }

  const host = String(process.env.MIMI_WS_HOST || "mimiclaw.local").trim() || "mimiclaw.local";
  return `ws://${host}:${WS_PORT}/`;
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function sanitizeError(err) {
  const msg = String(err?.message || err || "unknown_error")
    .replace(/\s+/g, "_")
    .replace(/[^a-zA-Z0-9_\-.:]/g, "");
  if (!msg) {
    return "unknown_error";
  }
  return msg.slice(0, 80);
}

async function runCommand(cmd, args, timeoutMs = 15000) {
  try {
    const { stdout, stderr } = await execFileAsync(cmd, args, {
      timeout: timeoutMs,
      maxBuffer: 1024 * 1024,
    });
    return { stdout: stdout || "", stderr: stderr || "" };
  } catch (err) {
    const stderr = String(err?.stderr || "").trim();
    const stdout = String(err?.stdout || "").trim();
    const suffix = stderr || stdout || "command_failed";
    throw new Error(`${cmd}_failed:${suffix}`);
  }
}

async function ensureBootedSimulator() {
  const { stdout } = await runCommand("xcrun", ["simctl", "list", "devices", "booted"], 15000);
  if (!stdout.includes("(Booted)")) {
    throw new Error("no_booted_simulator");
  }
}

async function openUrl(url) {
  await runCommand("xcrun", ["simctl", "openurl", "booted", url], 15000);
}

async function tapCenterOnSimulatorWindow() {
  const script = [
    'tell application "Simulator" to activate',
    'tell application "System Events"',
    '  tell process "Simulator"',
    "    set frontmost to true",
    '    if (count of windows) is 0 then error "simulator_window_not_found"',
    "    set p to position of front window",
    "    set s to size of front window",
    "    set cx to (item 1 of p) + ((item 1 of s) div 2)",
    "    set cy to (item 2 of p) + ((item 2 of s) div 2)",
    "    click at {cx, cy}",
    "  end tell",
    "end tell",
  ].join("\n");

  await runCommand("osascript", ["-e", script], 10000);
}

async function captureScreenshot(requestId) {
  const fileName = `mimi_ios_sim_${requestId}_${Date.now()}.jpg`;
  const filePath = path.join(os.tmpdir(), fileName);
  await runCommand("xcrun", ["simctl", "io", "booted", "screenshot", "--type=jpeg", filePath], 15000);
  return filePath;
}

async function sendPhotoToTelegram(tgChatId, screenshotPath, requestId) {
  const imageBytes = await fs.readFile(screenshotPath);
  const form = new FormData();
  form.set("chat_id", String(tgChatId));
  form.set("photo", new Blob([imageBytes], { type: "image/jpeg" }), path.basename(screenshotPath));

  const res = await fetch(`https://api.telegram.org/bot${BOT_TOKEN}/sendPhoto`, {
    method: "POST",
    body: form,
  });

  const text = await res.text();
  let body;
  try {
    body = JSON.parse(text);
  } catch (err) {
    throw new Error("telegram_non_json_response");
  }

  if (!res.ok || body?.ok !== true) {
    const desc = body?.description ? String(body.description) : `http_${res.status}`;
    throw new Error(`telegram_sendPhoto_failed:${desc}`);
  }

  const photos = body?.result?.photo;
  if (!Array.isArray(photos) || photos.length === 0) {
    throw new Error("telegram_missing_photo_sizes");
  }

  const largest = photos[photos.length - 1];
  if (!largest?.file_id) {
    throw new Error("telegram_missing_file_id");
  }
  return largest.file_id;
}

function parseRequestPayload(msg) {
  const payload = msg?.payload;
  if (!payload || typeof payload !== "object") {
    throw new Error("missing_payload");
  }
  const requestId = String(payload.request_id || "");
  const tgChatId = String(payload.tg_chat_id || "");
  const url = String(payload.url || "");
  const tapMode = String(payload.tap_mode || "center");
  const openWaitMs = clampInt(payload.open_wait_ms, DEFAULT_OPEN_WAIT_MS, 500, 15000);
  const waitAfterTapMs = clampInt(
    payload.wait_after_tap_ms,
    DEFAULT_WAIT_AFTER_TAP_MS,
    200,
    15000,
  );
  const timeoutMs = clampInt(payload.timeout_ms, DEFAULT_TIMEOUT_MS, 1000, 60000);

  if (!requestId) throw new Error("missing_request_id");
  if (!tgChatId) throw new Error("missing_tg_chat_id");
  if (!url || (!url.startsWith("http://") && !url.startsWith("https://"))) {
    throw new Error("invalid_url");
  }
  if (tapMode !== "center" && tapMode !== "none") {
    throw new Error("invalid_tap_mode");
  }

  return { requestId, tgChatId, url, tapMode, openWaitMs, waitAfterTapMs, timeoutMs };
}

function sendWsResult(ws, requestId, status, fields = {}) {
  if (!ws || ws.readyState !== WebSocket.OPEN) {
    return;
  }
  const payload = {
    request_id: requestId,
    status,
    ...fields,
  };
  ws.send(
    JSON.stringify({
      type: "ios_sim_capture_result",
      chat_id: HELPER_CHAT_ID,
      payload,
    }),
  );
}

async function runCaptureScenario(payload) {
  const { requestId, tgChatId, url, tapMode, openWaitMs, waitAfterTapMs, timeoutMs } = payload;
  const startedAt = Date.now();

  await ensureBootedSimulator();
  await openUrl(url);
  await sleep(openWaitMs);

  if (tapMode !== "none") {
    await tapCenterOnSimulatorWindow();
  }

  await sleep(waitAfterTapMs);

  const elapsed = Date.now() - startedAt;
  if (elapsed > timeoutMs) {
    throw new Error("ios_sim_capture_timeout_before_screenshot");
  }

  let screenshotPath = "";
  try {
    screenshotPath = await captureScreenshot(requestId);

    const elapsedBeforeUpload = Date.now() - startedAt;
    if (elapsedBeforeUpload > timeoutMs) {
      throw new Error("ios_sim_capture_timeout_before_upload");
    }

    const fileId = await sendPhotoToTelegram(tgChatId, screenshotPath, requestId);
    return fileId;
  } finally {
    if (screenshotPath) {
      await fs.unlink(screenshotPath).catch(() => {});
    }
  }
}

async function handleIncomingMessage(ws, data) {
  let msg;
  try {
    msg = JSON.parse(String(data));
  } catch (err) {
    return;
  }

  if (msg?.type !== "ios_sim_capture_request") {
    return;
  }

  let requestId = "unknown";
  try {
    const payload = parseRequestPayload(msg);
    requestId = payload.requestId;
    console.log(`[ios-sim-helper] request ${requestId} start`);
    const telegramFileId = await runCaptureScenario(payload);
    console.log(`[ios-sim-helper] request ${requestId} ok`);
    sendWsResult(ws, requestId, "ok", { telegram_file_id: telegramFileId });
  } catch (err) {
    const reason = sanitizeError(err);
    console.error(`[ios-sim-helper] request ${requestId} failed: ${reason}`);
    sendWsResult(ws, requestId, "error", { error: reason });
  }
}

function connectLoop() {
  console.log(`[ios-sim-helper] connecting to ${WS_URL} as ${HELPER_CHAT_ID}`);
  const ws = new WebSocket(WS_URL);

  ws.onopen = () => {
    reconnectDelayMs = 1000;
    console.log("[ios-sim-helper] connected");
    ws.send(
      JSON.stringify({
        type: "register",
        chat_id: HELPER_CHAT_ID,
        content: "ios_sim_helper_online",
      }),
    );
  };

  ws.onmessage = (event) => {
    inFlight = inFlight
      .then(() => handleIncomingMessage(ws, event.data))
      .catch((err) => {
        console.error(`[ios-sim-helper] internal error: ${sanitizeError(err)}`);
      });
  };

  ws.onerror = (err) => {
    const reason = sanitizeError(err);
    console.error(`[ios-sim-helper] ws error: ${reason}`);
  };

  ws.onclose = () => {
    console.error(`[ios-sim-helper] disconnected. reconnect in ${reconnectDelayMs}ms`);
    setTimeout(connectLoop, reconnectDelayMs);
    reconnectDelayMs = Math.min(reconnectDelayMs * 2, 10000);
  };
}

connectLoop();
