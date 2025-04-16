#include <ESP8266WiFi.h>
#include <painlessMesh.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

#define MESH_SSID     "MyMesh1"       // Mesh network SSID
#define MESH_PASSWORD "password123"  // Mesh password
#define MESH_PORT     5555           // Mesh communication port
#define AP_PASSWORD   "appassword"   // AP password

String NODE_ID; // Unique node ID based on MAC address

AsyncWebServer server(80); // Web server on port 80

// Data structures for messages and nodes
struct Message {
  String sender;
  String ipAddress;
  String content;
};
std::vector<Message> messageHistory; // Stores all messages

struct NodeInfo {
  String nodeId;
  String ipAddress;
};
std::vector<NodeInfo> nodeList; // Stores connected node info

unsigned long lastBroadcast = 0;
const unsigned long broadcastInterval = 5000; // 5 seconds

// Initialize PainlessMesh
painlessMesh mesh;

// Custom hash function for generating a string identifier from bytes
String customHash(const uint8_t* data, size_t len) {
  uint32_t hash = 5381; // DJB2
  for (size_t i = 0; i < len; i++) {
    hash = ((hash << 5) + hash) + data[i];
  }
  char hashStr[7];
  snprintf(hashStr, sizeof(hashStr), "%06X", (hash & 0xFFFFFF));
  String result = String(hashStr);
  result.toUpperCase();
  Serial.printf("Debug: Generated hash signature: %s for input length %u\n", result.c_str(), len);
  return result;
}

// Broadcast node info periodically
void broadcastNodeInfo() {
  DynamicJsonDocument doc(256);
  doc["type"]      = "nodeinfo";
  doc["nodeId"]    = NODE_ID;
  doc["ipAddress"] = WiFi.softAPIP().toString();
  String jsonMsg;
  serializeJson(doc, jsonMsg);
  mesh.sendBroadcast(jsonMsg);
  Serial.println("Broadcasted node info: " + jsonMsg);
}

// Callback when a message is received
void receivedCallback(uint32_t from, String &msg) {
  Serial.printf("Received message from %u: %s\n", from, msg.c_str());
  DynamicJsonDocument doc(1024);
  auto error = deserializeJson(doc, msg);
  if (error) {
    Serial.println("Failed to parse message: " + String(error.c_str()));
    return;
  }
  String type = doc["type"] | "message";
  if (type == "nodeinfo") {
    // existing nodeinfo handling...
    String nodeId    = doc["nodeId"];
    String ipAddress = doc["ipAddress"];
    bool exists = false;
    for (auto &n : nodeList) {
      if (n.nodeId == nodeId) {
        n.ipAddress = ipAddress;
        exists = true;
        break;
      }
    }
    if (!exists) {
      nodeList.push_back({nodeId, ipAddress});
      Serial.printf("Added node: %s (%s)\n", nodeId.c_str(), ipAddress.c_str());
    }
  } else {
    // handle chat message
    String sender    = doc["sender"];
    String ipAddress = doc["ipAddress"];
    String content   = doc["content"];
    messageHistory.push_back({sender, ipAddress, content});
    Serial.printf("CHAT from %s (%s): %s\n", sender.c_str(), ipAddress.c_str(), content.c_str());
  }
}

// Callback when a new node connects
void newConnectionCallback(uint32_t nodeId) {
  Serial.printf("New connection, nodeId: %u\n", nodeId);
  broadcastNodeInfo();
}

// Callback when a node disconnects
void droppedConnectionCallback(uint32_t nodeId) {
  Serial.printf("Node disconnected: %u\n", nodeId);
  String id = String(nodeId);
  nodeList.erase(std::remove_if(
    nodeList.begin(), nodeList.end(),
    [&](const NodeInfo &n){ return n.nodeId == id; }
  ), nodeList.end());
}

// Serve the chat web page
void serveChatPage(AsyncWebServerRequest *request) {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>Mesh Chat</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial; margin: 20px; }
    .container { max-width:600px; margin:0 auto; }
    #chatBox { height:300px; overflow-y:scroll; border:1px solid #ccc; padding:10px; background:#f9f9f9; }
    .msg-row { padding:5px 0; }
    .input-area { display:flex; margin-bottom:10px; }
    #msg { flex:1; padding:8px; border:1px solid #ccc; }
    button { background:#4CAF50; color:#fff; border:none; padding:10px 15px; cursor:pointer; }
    table { width:100%; border-collapse:collapse; margin-top:10px; }
    th, td { border:1px solid #ccc; padding:8px; text-align:left; }
    th { background:#f0f0f0; }
  </style>
</head>
<body>
  <div class="container">
    <h2>Mesh Chat</h2>
    <div id="chatBox"></div>
    <div class="input-area">
      <input id="msg" placeholder="Type message..." onkeypress="if(event.key==='Enter') sendMsg();" />
      <button onclick="sendMsg()">Send</button>
    </div>
    <div>Your Node ID: <span id="nodeId"></span></div>
    <h3>Network Nodes</h3>
    <table>
      <thead><tr><th>Node ID</th></tr></thead>
      <tbody id="nodesTable"></tbody>
    </table>
  </div>
  <script>
    let lastCount = 0;

    // show our own ID
    fetch('/nodeid').then(r=>r.text()).then(id=>{
      document.getElementById('nodeId').textContent = id;
    });

    // refresh nodes table
    async function updateNodes() {
      const res = await fetch('/nodesinfo');
      const data = await res.json();
      const tbody = document.getElementById('nodesTable');
      tbody.innerHTML = '';
      data.nodes.forEach(m => {
        const row = document.createElement('tr');
        row.innerHTML = `<td>${m.nodeId}</td>`;
        tbody.appendChild(row);
      });
    }
    setInterval(updateNodes, 5000);
    updateNodes();

    // send a new message
    function sendMsg() {
      const txt = document.getElementById('msg');
      if (!txt.value.trim()) return;
      fetch('/send', {
        method: 'POST',
        headers: {'Content-Type':'application/x-www-form-urlencoded'},
        body: 'msg=' + encodeURIComponent(txt.value)
      });
      txt.value = '';
    }

    // pull in new messages
    async function getMessages() {
      const res = await fetch('/messages?lastCount=' + lastCount);
      const data = await res.json();
      if (data.messages.length) {
        const box = document.getElementById('chatBox');
        data.messages.forEach(m => {
          const div = document.createElement('div');
          div.className = 'msg-row';
          div.textContent = `${m.sender} (${m.ipAddress}): ${m.content}`;
          box.appendChild(div);
        });
        lastCount = data.messageCount;
        box.scrollTop = box.scrollHeight;
      }
    }
    setInterval(getMessages, 1000);
    getMessages();
  </script>
</body>
</html>
  )rawliteral";
  request->send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // make our node ID
  uint8_t mac[6];
  WiFi.macAddress(mac);
  NODE_ID = customHash(mac, 6);

  // start AP
  String apName = "MeshNode-" + NODE_ID;
  WiFi.softAP(apName.c_str(), AP_PASSWORD);

  // initialize mesh
  mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);
  mesh.init(MESH_SSID, MESH_PASSWORD, MESH_PORT);
  mesh.onReceive(&receivedCallback);
  mesh.onNewConnection(&newConnectionCallback);
  mesh.onDroppedConnection(&droppedConnectionCallback);

  // add self to list
  nodeList.push_back({NODE_ID, WiFi.softAPIP().toString()});

  // HTTP routes
  server.on("/", HTTP_GET, serveChatPage);
  server.on("/nodeid", HTTP_GET, [](AsyncWebServerRequest *r){
    r->send(200, "text/plain", NODE_ID);
  });
  server.on("/nodesinfo", HTTP_GET, [](AsyncWebServerRequest *r){
    DynamicJsonDocument doc(1024);
    auto arr = doc.createNestedArray("nodes");
    for (auto &n : nodeList) {
      auto obj = arr.createNestedObject();
      obj["nodeId"] = n.nodeId;
      obj["ipAddress"] = n.ipAddress;
    }
    String out;
    serializeJson(doc, out);
    r->send(200, "application/json", out);
  });
  server.on("/send", HTTP_POST, [](AsyncWebServerRequest *r){
    if (r->hasParam("msg", true)) {
      String txt = r->getParam("msg", true)->value();
      DynamicJsonDocument doc(256);
      doc["type"]      = "message";
      doc["sender"]    = NODE_ID;
      doc["ipAddress"] = WiFi.softAPIP().toString();
      doc["content"]   = txt;
      String jm;
      serializeJson(doc, jm);
      mesh.sendBroadcast(jm);
      messageHistory.push_back({NODE_ID, WiFi.softAPIP().toString(), txt});
      Serial.println("Sent: " + jm);
    }
    r->send(200, "text/plain", "OK");
  });
  server.on("/messages", HTTP_GET, [](AsyncWebServerRequest *r){
    int last = 0;
    if (r->hasParam("lastCount")) 
      last = r->getParam("lastCount")->value().toInt();
    DynamicJsonDocument doc(2048);
    doc["messageCount"] = messageHistory.size();
    auto arr = doc.createNestedArray("messages");
    for (size_t i = last; i < messageHistory.size(); i++) {
      auto &m = messageHistory[i];
      auto obj = arr.createNestedObject();
      obj["sender"]    = m.sender;
      obj["ipAddress"] = m.ipAddress;
      obj["content"]   = m.content;
    }
    String out;
    serializeJson(doc, out);
    r->send(200, "application/json", out);
  });
  server.begin();

  Serial.println("Node ID: " + NODE_ID);
  Serial.println("AP SSID: MeshNode-" + NODE_ID);
  Serial.println("AP IP: " + WiFi.softAPIP().toString());
}

void loop() {
  mesh.update();
  if (millis() - lastBroadcast >= broadcastInterval) {
    broadcastNodeInfo();
    lastBroadcast = millis();
  }
}
