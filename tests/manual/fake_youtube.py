# Sahte YouTube: InnerTube visitor_id/player + googlevideo benzeri aralikli GET
import http.server, json, struct, sys, urllib.parse, threading
import os
S=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'yt') + os.sep
V=open(S+'yt_v.mp4','rb').read(); A=open(S+'yt_a.mp4','rb').read()
PORT=int(sys.argv[1]) if len(sys.argv)>1 else 8765
def box(d,t):
    i=0
    while i+8<=len(d):
        sz,=struct.unpack('>I',d[i:i+4])
        if d[i+4:i+8]==t: return i,i+sz-1
        i+=sz
LOG=open(S+'fake_youtube.log','w')
def fmt(itag,path,mime,d,extra):
    m=box(d,b'moov'); s=box(d,b'sidx')
    f={"itag":itag,"url":"http://127.0.0.1:%d/%s?id=1&c=VISIONOS"%(PORT,path),"mimeType":mime,"initRange":{"start":"0","end":str(m[1])},
       "indexRange":{"start":str(s[0]),"end":str(s[1])},"contentLength":str(len(d)),"bitrate":100000,"approxDurationMs":"40000"}
    f.update(extra); return f
PLAYER={"playabilityStatus":{"status":"OK"},"videoDetails":{"videoId":"abcDEF12345","title":"Deneme","lengthSeconds":"40"},
 "streamingData":{"adaptiveFormats":[fmt(134,'v','video/mp4; codecs="avc1.4D401E"',V,{"width":640,"height":360,"fps":24}),
                                      fmt(140,'a','audio/mp4; codecs="mp4a.40.2"',A,{})]}}
class H(http.server.BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def log_message(self,*a): pass
    def send(self,code,body,ct='application/json'):
        self.send_response(code); self.send_header('Content-Type',ct); self.send_header('Content-Length',str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_POST(self):
        n=int(self.headers.get('Content-Length','0')); body=self.rfile.read(n)
        LOG.write('POST %s UA=%s body=%s\n'%(self.path,self.headers.get('User-Agent'),body[:200])); LOG.flush()
        if '/youtubei/v1/visitor_id' in self.path: return self.send(200,b'{"responseContext":{"visitorData":"TEST%3D"}}')
        if '/youtubei/v1/player' in self.path: return self.send(200,json.dumps(PLAYER).encode())
        self.send(404,b'{}')
    def do_GET(self):
        u=urllib.parse.urlparse(self.path); q=urllib.parse.parse_qs(u.query)
        LOG.write('GET %s range=%s UA=%s\n'%(u.path,q.get('range'),self.headers.get('User-Agent'))); LOG.flush()
        d=V if u.path=='/v' else A if u.path=='/a' else None
        if d is None or 'range' not in q: return self.send(404,b'')
        a,b=[int(x) for x in q['range'][0].split('-')]; b=min(b,len(d)-1)
        self.send(200,d[a:b+1],'application/octet-stream')
http.server.ThreadingHTTPServer(('127.0.0.1',PORT),H).serve_forever()
