var result = 'bekleniyor';
var k = document.createElement('ytd-kart');
k.appendChild(document.createTextNode('isik'));
document.body.appendChild(k);
setTimeout(function () {
  try {
    var r = k.shadowRoot || k;
    var a = [ShadyDOM.inUse, !!k.shadowRoot, r.querySelector('#baslik').textContent, r.querySelectorAll('p.v').length,
             Array.prototype.map.call(r.querySelectorAll('p.v'), function (p) { return p.textContent; }).join('/')];
    r.querySelectorAll('p.v')[1].click ? 0 : 0;
    r.querySelectorAll('p.v')[1].dispatchEvent(new MouseEvent('click', {bubbles: true, composed: true}));
    k.goster = true; k.push('videolar', {ad: 'Video C'}); k.baslik = 'Kesfet';
    setTimeout(function () {
      a.push(k.sayi, k.getAttribute('sayi'), k.sonTik, r.querySelectorAll('p.v').length, !!r.querySelector('#ek'), r.querySelector('#baslik').textContent);
      result = a.join(' | ');
    }, 50);
  } catch (e) { result = 'HATA ' + e + ' ' + e.stack; }
}, 50);
