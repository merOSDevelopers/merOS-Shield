var el = document.createElement('sayac-buton');
el.ad = 'Mert';
document.body.appendChild(el);
var result = 'bekleniyor';
el.updateComplete.then(() => {
  var r = el.shadowRoot;
  var t1 = r.querySelector('p').textContent + '|' + r.querySelectorAll('li').length + '|' + r.querySelector('button').textContent;
  r.querySelector('button').click ? r.querySelector('button').dispatchEvent(new MouseEvent('click', {bubbles: true, composed: true})) : 0;
  return el.updateComplete.then(() => {
    var t2 = r.querySelector('button').textContent + '|' + el.getAttribute('ad') + '|' + (r.adoptedStyleSheets ? 'adopted' : r.querySelectorAll('style').length + 'style');
    result = t1 + ' || ' + t2;
  });
}).catch(e => { result = 'HATA ' + e + ' ' + (e && e.stack); });
result
