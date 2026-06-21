const PASSWORD_SALT = "RADXA_FIXED_SALT_v1";

const video = document.querySelector("#video");
const statusElement = document.querySelector("#status");

let config = null;
let messages = {
  readingSettings: "Reading settings...",
  missingUsername: "Please complete settings: username is empty",
  missingPassword: "Please complete settings: password is empty",
  missingSourceId: "Plugin settings are incomplete: source ID was not found",
  configFailed: "Failed to read plugin settings",
  loginFailed: "Login failed: check username or password",
  invalidWhep: "Connection failed: device returned an invalid WebRTC response",
  authFailed: "Connection failed: authentication failed, check username and password",
  apiMissing: "Connection failed: device API was not found",
  deviceUnreachable: "Connection failed: unable to reach the Linkr device",
  connectionFailed: "Connection failed",
  unknownFailed: "Connection failed: unknown error",
  loggingIn: "Logging in to Linkr...",
  connectingWebrtc: "Connecting WebRTC...",
  reconnecting: "Connection interrupted, reconnecting...",
  retriesExhausted: "Connection failed: retry limit reached, check the device and click Refresh",
  reconnectAttempt: "Connection interrupted, reconnect attempt {attempt}..."
};
let peerConnection = null;
let remoteStream = null;
let streamId = null;
let healthTimer = null;
let reconnectTimer = null;
let reconnectAttempts = 0;
let isStopping = false;

function t(key, params = {}) {
  const template = messages[key] || key;
  return template.replace(/\{([a-zA-Z0-9_]+)\}/g, (_, name) => {
    return params[name] === undefined ? "" : String(params[name]);
  });
}

function setStatus(message) {
  if (!statusElement) return;
  statusElement.textContent = message;
  statusElement.classList.remove("hidden");
}

function clearStatus() {
  if (!statusElement) return;
  statusElement.classList.add("hidden");
}

function userMessage(error) {
  const message = error instanceof Error ? error.message : String(error || "");
  if (/Missing username/i.test(message)) return t("missingUsername");
  if (/Missing password/i.test(message)) return t("missingPassword");
  if (/Missing source id/i.test(message)) return t("missingSourceId");
  if (/Config failed/i.test(message)) return `${t("configFailed")}\n${message}`;
  if (/Login failed/i.test(message)) return t("loginFailed");
  if (/WHEP response/i.test(message)) return t("invalidWhep");
  if (/HTTP 401|HTTP 403/i.test(message)) return t("authFailed");
  if (/HTTP 404/i.test(message)) return t("apiMissing");
  if (/HTTP 502/i.test(message)) return t("deviceUnreachable");
  if (/HTTP/i.test(message)) return `${t("connectionFailed")}\n${message}`;
  return message ? `${t("connectionFailed")}\n${message}` : t("unknownFailed");
}

function getSourceId() {
  const query = new URLSearchParams(window.location.search);
  return query.get("source") || "";
}

async function loadConfig() {
  setStatus(t("readingSettings"));
  const sourceId = getSourceId();
  if (!sourceId) throw new Error("Missing source id");

  const response = await fetch(`/config/${encodeURIComponent(sourceId)}`, {
    cache: "no-store"
  });
  if (!response.ok) throw new Error(`Config failed: HTTP ${response.status}`);
  const loaded = await response.json();
  messages = {
    ...messages,
    ...(loaded.messages || {})
  };
  return loaded;
}

function normalizeDeviceBase(value) {
  const trimmed = String(value || "").trim();
  const withProtocol = /^https?:\/\//i.test(trimmed) ? trimmed : `http://${trimmed}`;
  return withProtocol.replace(/\/+$/, "");
}

async function sha256Hex(value) {
  const data = new TextEncoder().encode(value);
  const hash = await crypto.subtle.digest("SHA-256", data);
  return Array.from(new Uint8Array(hash))
    .map((byte) => byte.toString(16).padStart(2, "0"))
    .join("");
}

async function linkrFetch(path, options = {}) {
  const headers = new Headers(options.headers || {});

  if (options.json !== undefined) {
    headers.set("content-type", "application/json");
    options.body = JSON.stringify(options.json);
  }

  headers.set("x-linkr-base", config.deviceUrl);

  const response = await fetch(`/proxy${path}`, {
    ...options,
    headers
  });

  const contentType = response.headers.get("content-type") || "";
  const payload = contentType.includes("application/json") ? await response.json() : await response.text();

  if (!response.ok) {
    const detail = typeof payload === "string" ? payload : payload.message || JSON.stringify(payload);
    throw new Error(`HTTP ${response.status}: ${detail}`);
  }

  return payload;
}

async function login() {
  if (!config.username) throw new Error("Missing username");
  if (!config.password) throw new Error("Missing password");

  setStatus(t("loggingIn"));
  const passwd = await sha256Hex(PASSWORD_SALT + config.password);
  const payload = await linkrFetch("/api/account/login", {
    method: "POST",
    json: {
      username: config.username,
      passwd
    }
  });

  if (payload.code !== 0 || !payload.data?.token) {
    throw new Error(payload.msg || payload.message || "Login failed");
  }

  return payload.data.token;
}

function preferH264(transceiver) {
  try {
    const capabilities = RTCRtpReceiver.getCapabilities?.("video");
    if (!capabilities?.codecs?.length) return;

    const h264 = capabilities.codecs.filter((codec) => /H264/i.test(codec.mimeType));
    const rest = capabilities.codecs.filter((codec) => !h264.includes(codec));
    if (h264.length) transceiver.setCodecPreferences([...h264, ...rest]);
  } catch {}
}

function waitForIceGathering(pc, timeoutMs = 5000) {
  if (pc.iceGatheringState === "complete") return Promise.resolve();

  return new Promise((resolve) => {
    const timeout = setTimeout(done, timeoutMs);

    function done() {
      clearTimeout(timeout);
      pc.removeEventListener("icegatheringstatechange", onStateChange);
      resolve();
    }

    function onStateChange() {
      if (pc.iceGatheringState === "complete") done();
    }

    pc.addEventListener("icegatheringstatechange", onStateChange);
  });
}

function parseWhepAnswer(payload) {
  const answer = payload?.data?.answer ?? payload?.answer ?? payload?.data ?? payload;

  if (payload?.data?.stream_id) streamId = payload.data.stream_id;
  if (payload?.stream_id) streamId = payload.stream_id;

  if (typeof answer === "string") return answer;
  if (answer?.sdp) return answer.sdp;
  if (answer?.data?.sdp) return answer.data.sdp;

  throw new Error("WHEP response has no SDP answer");
}

async function startPlayback() {
  if (peerConnection) return;

  isStopping = false;
  clearTimeout(reconnectTimer);
  reconnectTimer = null;

  const token = await login();
  setStatus(t("connectingWebrtc"));
  const pc = new RTCPeerConnection({
    sdpSemantics: "unified-plan",
    bundlePolicy: "max-bundle"
  });

  peerConnection = pc;
  streamId = null;

  const videoTransceiver = pc.addTransceiver("video", { direction: "recvonly" });
  preferH264(videoTransceiver);

  if (config.receiveAudio) {
    pc.addTransceiver("audio", { direction: "recvonly" });
    video.muted = false;
  } else {
    video.muted = true;
  }

  pc.createDataChannel("chat", {
    ordered: false,
    maxRetransmits: 0,
    priority: "high",
    protocol: "binary"
  });

  pc.addEventListener("track", (event) => {
    const [stream] = event.streams;
    if (!stream) return;

    remoteStream = stream;
    video.srcObject = stream;
    video.play().catch(() => {});
    clearStatus();
  });

  pc.addEventListener("connectionstatechange", () => {
    if (pc.connectionState === "connected") reconnectAttempts = 0;
    if (pc.connectionState === "connected") clearStatus();
    if (["failed", "disconnected"].includes(pc.connectionState) && !isStopping) {
      setStatus(t("reconnecting"));
      scheduleReconnect();
    }
  });

  try {
    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    await waitForIceGathering(pc, 5000);

    const response = await linkrFetch("/api/rtc/whep", {
      method: "POST",
      headers: {
        authorization: `Bearer ${token}`
      },
      json: {
        offer: pc.localDescription?.sdp || offer.sdp
      }
    });

    const answerSdp = parseWhepAnswer(response);
    await pc.setRemoteDescription({ type: "answer", sdp: answerSdp });

    if (streamId) startHealthTimer(token);
  } catch (error) {
    stopPlayback();
    throw error;
  }
}

function startHealthTimer(token) {
  clearInterval(healthTimer);
  healthTimer = setInterval(() => {
    if (!streamId) return;
    linkrFetch("/api/rtc/health", {
      method: "POST",
      headers: {
        authorization: `Bearer ${token}`
      },
      json: {
        stream_id: Number(streamId)
      }
    }).catch(() => {});
  }, 60000);
}

function stopPlayback() {
  isStopping = true;
  clearInterval(healthTimer);
  clearTimeout(reconnectTimer);
  healthTimer = null;
  reconnectTimer = null;
  streamId = null;

  if (peerConnection) {
    peerConnection.getSenders().forEach((sender) => sender.track?.stop());
    peerConnection.getReceivers().forEach((receiver) => receiver.track?.stop());
    peerConnection.getTransceivers().forEach((transceiver) => {
      try {
        transceiver.stop();
      } catch {}
    });
    peerConnection.close();
  }

  if (remoteStream) {
    remoteStream.getTracks().forEach((track) => track.stop());
  }

  peerConnection = null;
  remoteStream = null;
  video.pause();
  video.removeAttribute("src");
  video.srcObject = null;
  video.load();
}

function scheduleReconnect() {
  if (reconnectTimer) return;
  if (reconnectAttempts >= Number(config.maxRetries || 5)) {
    setStatus(t("retriesExhausted"));
    return;
  }

  reconnectAttempts += 1;
  setStatus(t("reconnectAttempt", { attempt: reconnectAttempts }));
  reconnectTimer = setTimeout(async () => {
    reconnectTimer = null;
    try {
      stopPlayback();
      isStopping = false;
      await startPlayback();
    } catch (error) {
      setStatus(userMessage(error));
      scheduleReconnect();
    }
  }, Number(config.retryMs || 3000));
}

async function main() {
  config = await loadConfig();
  config.deviceUrl = normalizeDeviceBase(config.deviceUrl || "linkr-device.local");
  await startPlayback();
}

window.addEventListener("beforeunload", () => stopPlayback());

main().catch((error) => {
  stopPlayback();
  setStatus(userMessage(error));
});
