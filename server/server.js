import http from "node:http";
import net from "node:net";
import { Aedes } from "aedes";

const aedes = await Aedes.createBroker({ id: "lights-local-broker" });

const HTTP_HOST = process.env.HTTP_HOST || "127.0.0.1";
const HTTP_PORT = Number(process.env.HTTP_PORT || 8080);
const MQTT_HOST = process.env.MQTT_HOST || "0.0.0.0";
const MQTT_PORT = Number(process.env.MQTT_PORT || 1883);
const MQTT_TOPIC = "lights/rgb";

let currentColor = "#000000";
let lastPublishedAt = null;
const clients = new Set();

const page = `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 RGB control</title>
  <style>
    :root { color-scheme: dark; font-family: ui-sans-serif, system-ui, sans-serif; }
    * { box-sizing: border-box; }
    body { margin: 0; min-height: 100vh; display: grid; place-items: center; background: #101114; color: #f5f5f5; }
    main { width: min(92vw, 430px); padding: 28px; border: 1px solid #303238; border-radius: 20px; background: #1a1c21; box-shadow: 0 24px 70px #0008; }
    h1 { margin: 0 0 8px; font-size: 1.55rem; }
    p { color: #afb3bd; margin: 0 0 24px; }
    input[type=color] { width: 100%; height: 180px; padding: 0; border: 0; border-radius: 14px; overflow: hidden; background: transparent; cursor: pointer; }
    .row { display: flex; gap: 10px; margin-top: 16px; }
    input[type=text] { flex: 1; min-width: 0; padding: 12px 14px; border: 1px solid #3c3f47; border-radius: 10px; background: #111216; color: inherit; font: 600 1rem ui-monospace, monospace; text-transform: uppercase; }
    button { padding: 12px 16px; border: 0; border-radius: 10px; background: #f1f1f1; color: #111; font-weight: 700; cursor: pointer; }
    .presets { display: grid; grid-template-columns: repeat(7, 1fr); gap: 8px; margin-top: 18px; }
    .swatch { aspect-ratio: 1; min-width: 0; padding: 0; border: 2px solid #ffffff22; border-radius: 50%; }
    #status { min-height: 1.4em; margin-top: 18px; color: #9ea3ad; font-size: .9rem; }
  </style>
</head>
<body>
  <main>
    <h1>ESP32 RGB light</h1>
    <p>Choose a color to publish it over MQTT.</p>
    <input id="picker" type="color" value="#000000" aria-label="Light color">
    <div class="row">
      <input id="hex" type="text" value="#000000" maxlength="7" aria-label="Hex color">
      <button id="send" type="button">Send</button>
    </div>
    <div class="presets" id="presets"></div>
    <div id="status">Loading…</div>
  </main>
  <script>
    const picker = document.querySelector('#picker');
    const hex = document.querySelector('#hex');
    const status = document.querySelector('#status');
    const colors = ['#FF0000', '#FF7A00', '#FFFF00', '#00FF00', '#00BFFF', '#0000FF', '#FF00FF'];
    let timer;

    for (const color of colors) {
      const button = document.createElement('button');
      button.className = 'swatch';
      button.style.background = color;
      button.title = color;
      button.addEventListener('click', () => selectAndSend(color));
      document.querySelector('#presets').append(button);
    }

    function normalizeColor(value) {
      const candidate = value.trim().toUpperCase();
      return /^#[0-9A-F]{6}$/.test(candidate) ? candidate : null;
    }

    async function publish(color) {
      status.textContent = 'Sending ' + color + '…';
      try {
        const response = await fetch('/api/color', {
          method: 'POST',
          headers: { 'content-type': 'application/json' },
          body: JSON.stringify({ color })
        });
        const result = await response.json();
        if (!response.ok) throw new Error(result.error || 'Request failed');
        status.textContent = 'Sent ' + result.color + ' · ' + result.mqttClients + ' MQTT client(s)';
      } catch (error) {
        status.textContent = error.message;
      }
    }

    function selectAndSend(color) {
      picker.value = color;
      hex.value = color;
      publish(color);
    }

    picker.addEventListener('input', () => {
      const color = picker.value.toUpperCase();
      hex.value = color;
      clearTimeout(timer);
      timer = setTimeout(() => publish(color), 80);
    });

    document.querySelector('#send').addEventListener('click', () => {
      const color = normalizeColor(hex.value);
      if (!color) {
        status.textContent = 'Enter a color like #FF00AA';
        return;
      }
      selectAndSend(color);
    });

    fetch('/api/status').then(response => response.json()).then(result => {
      picker.value = result.color;
      hex.value = result.color;
      status.textContent = 'Ready · ' + result.mqttClients + ' MQTT client(s)';
    }).catch(error => { status.textContent = error.message; });
  </script>
</body>
</html>`;

function sendJson(response, statusCode, body) {
  const encoded = Buffer.from(JSON.stringify(body));
  response.writeHead(statusCode, {
    "content-type": "application/json; charset=utf-8",
    "content-length": encoded.length,
    "cache-control": "no-store"
  });
  response.end(encoded);
}

function publishColor(color, response) {
  aedes.publish(
    { topic: MQTT_TOPIC, payload: Buffer.from(color), qos: 0, retain: true },
    (error) => {
      if (error) {
        sendJson(response, 500, { error: error.message });
        return;
      }

      currentColor = color;
      lastPublishedAt = new Date().toISOString();
      console.log(`[web] published ${color} to ${MQTT_TOPIC}`);
      sendJson(response, 200, {
        color: currentColor,
        mqttClients: clients.size,
        topic: MQTT_TOPIC
      });
    }
  );
}

const httpServer = http.createServer((request, response) => {
  if (request.method === "GET" && request.url === "/") {
    response.writeHead(200, {
      "content-type": "text/html; charset=utf-8",
      "content-length": Buffer.byteLength(page),
      "cache-control": "no-store"
    });
    response.end(page);
    return;
  }

  if (request.method === "GET" && request.url === "/api/status") {
    sendJson(response, 200, {
      color: currentColor,
      mqttClients: clients.size,
      lastPublishedAt,
      topic: MQTT_TOPIC
    });
    return;
  }

  if (request.method === "POST" && request.url === "/api/color") {
    let body = "";
    request.setEncoding("utf8");
    request.on("data", (chunk) => {
      body += chunk;
      if (body.length > 1024) request.destroy();
    });
    request.on("end", () => {
      try {
        const color = String(JSON.parse(body).color || "").toUpperCase();
        if (!/^#[0-9A-F]{6}$/.test(color)) {
          sendJson(response, 400, { error: "Color must use #RRGGBB format" });
          return;
        }
        publishColor(color, response);
      } catch (error) {
        sendJson(response, 400, { error: "Invalid JSON request" });
      }
    });
    return;
  }

  sendJson(response, 404, { error: "Not found" });
});

const mqttServer = net.createServer(aedes.handle);

aedes.on("client", (client) => {
  clients.add(client.id);
  console.log(`[mqtt] connected: ${client.id}`);
});

aedes.on("clientDisconnect", (client) => {
  clients.delete(client.id);
  console.log(`[mqtt] disconnected: ${client.id}`);
});

mqttServer.listen(MQTT_PORT, MQTT_HOST, () => {
  console.log(`MQTT broker: mqtt://192.168.31.10:${MQTT_PORT}`);
});

httpServer.listen(HTTP_PORT, HTTP_HOST, () => {
  console.log(`Color control: http://${HTTP_HOST}:${HTTP_PORT}`);
});

function shutdown() {
  console.log("Shutting down…");
  httpServer.close();
  mqttServer.close();
  aedes.close(() => process.exit(0));
}

process.once("SIGINT", shutdown);
process.once("SIGTERM", shutdown);
