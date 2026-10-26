# merOS Shield - YouTube yerel oynatici akisini gercek YouTube'a karsi dener (yalnizca standart Python).
# Kullanim: python yt_probe.py [VIDEO_ID]
# merOS'un yaptigi isteklerin aynisi: visitor_id -> player (visionOS) -> bicim secimi -> aralikli indirme.
import json, random, string, sys, urllib.request

VID = sys.argv[1] if len(sys.argv) > 1 else "jNQXAC9IVRw"
UA = "com.google.visionos.youtube/1.04(RealityDevice17,1; U; CPU visionOS 26_6_0 like Mac OS X; TR)"
HDR = {"User-Agent": UA, "X-Goog-Api-Format-Version": "2", "Content-Type": "application/json", "Accept-Language": "tr-TR, tr;q=0.9"}

def nonce(n):
    return "".join(random.choice(string.ascii_letters + string.digits + "-_") for _ in range(n))

def ctx(visitor=None):
    c = {"clientName": "VISIONOS", "clientVersion": "1.04", "clientScreen": "WATCH", "platform": "MOBILE",
         "deviceMake": "Apple", "deviceModel": "RealityDevice17,1", "osName": "visionOS", "osVersion": "26.6.0.23O770",
         "hl": "tr", "gl": "TR", "utcOffsetMinutes": 0}
    if visitor:
        c["visitorData"] = visitor
    return {"client": c, "request": {"internalExperimentFlags": [], "useSsl": True}, "user": {"lockedSafetyMode": False}}

def post(url, body):
    req = urllib.request.Request(url, data=json.dumps(body).encode(), headers=HDR, method="POST")
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.status, json.loads(r.read().decode())

st, v = post("https://www.youtube.com/youtubei/v1/visitor_id?prettyPrint=false", {"context": ctx()})
visitor = v.get("responseContext", {}).get("visitorData")
print("visitor_id:", st, "visitorData" if visitor else "YOK")
cpn = nonce(16)
st, p = post("https://youtubei.googleapis.com/youtubei/v1/player?prettyPrint=false&t=%s&id=%s" % (nonce(12), VID),
             {"context": ctx(visitor), "videoId": VID, "cpn": cpn, "contentCheckOk": True, "racyCheckOk": True})
ps = p.get("playabilityStatus", {})
print("player:", st, ps.get("status"), ps.get("reason", ""))
fmts = [f for f in p.get("streamingData", {}).get("adaptiveFormats", []) if "url" in f]
print("bicimler:", [(f["itag"], f["mimeType"].split(";")[0], f.get("height")) for f in fmts])
avc = [f for f in fmts if 'avc1' in f["mimeType"] and f.get("height", 0) <= 720]
aac = [f for f in fmts if 'mp4a.40.2' in f["mimeType"]]
for f in (max(avc, key=lambda f: f.get("height", 0)) if avc else None, aac[0] if aac else None):
    if not f:
        continue
    url = f["url"] + "&cpn=" + cpn + "&range=0-" + f["indexRange"]["end"] + "&rn=1"
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            data = r.read()
            print("itag", f["itag"], "baslik:", r.status, len(data), "bayt", data[4:8], "n=" in f["url"])
    except Exception as e:
        print("itag", f["itag"], "HATA:", e)
