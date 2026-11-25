// ===== Ready2Gear - JavaScript block ใน CiRA CORE =====
// วางต่อจาก Deep Detect แล้วส่งต่อไป Mqtt Pub
// ใช้โค้ดเดียวกันได้ทั้งโฟลว์ใบหน้าและโฟลว์ของ แก้แค่ MODEL
//
// ผลลัพธ์ที่ส่งออก:
//   {"model":"item","counts":{"hammer":1,"screwdriver":2},"best":{"label":"screwdriver","score":0.93},"n":3,"request_id":"..."}
//   counts = จำนวนของแต่ละชนิดที่เจอในภาพ (ใช้ตอนเปลี่ยนเป็นตรวจทั้งถาด)
//   best   = ชนิดที่มั่นใจที่สุด (ใช้ตอนนี้ ตรวจทีละชิ้น / ระบุใบหน้า)

var MODEL = "item";     // โฟลว์ใบหน้าเปลี่ยนเป็น "face"
var MIN_SCORE = 0.5;    // ความมั่นใจขั้นต่ำ 0-1 (ต่ำกว่านี้ไม่นับ)

var counts = {};
var best = { label: "", score: 0 };
var seen = 0;
var reqId = "";   // request_id จาก Mqtt Sub (ถ้ามี) ส่งกลับไปให้ Node-RED จับคู่

function toScore(v) {
  var n = parseFloat(v);
  if (isNaN(n)) return null;
  return n > 1 ? n / 100 : n;          // รองรับทั้ง 0.93 และ 93
}

function isArray(o) {
  return Object.prototype.toString.call(o) === "[object Array]";
}

function visit(o) {
  if (o === null || typeof o !== "object") return;
  if (isArray(o)) {
    for (var i = 0; i < o.length; i++) visit(o[i]);
    return;
  }
  if (typeof o.request_id === "string" && !reqId) reqId = o.request_id;
  var name = o.label || o.name || o["class"] || o.obj_name || o.className;
  if (typeof name === "string" && name.length > 0) {
    var raw = (o.score !== undefined) ? o.score :
              (o.conf !== undefined) ? o.conf :
              (o.confidence !== undefined) ? o.confidence : o.prob;
    var s = toScore(raw);
    if (s === null) s = 1;
    if (s >= MIN_SCORE) {
      counts[name] = (counts[name] || 0) + 1;
      seen++;
      if (s > best.score) best = { label: name, score: Math.round(s * 100) / 100 };
    }
    return;                             // วัตถุนี้นับแล้ว ไม่ต้องลงไปข้างใน
  }
  for (var k in o) {
    if (o.hasOwnProperty(k) && typeof o[k] === "object") visit(o[k]);
  }
}

visit(payload);
payload = { model: MODEL, counts: counts, best: best, n: seen, request_id: reqId };
