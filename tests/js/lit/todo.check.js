var result = 'bekleniyor';
var l = document.createElement('todo-liste');
document.body.appendChild(l);
(async () => {
  await l.updateComplete;
  const r = l.shadowRoot;
  const a = [r.querySelector('h1').textContent, r.querySelectorAll('todo-oge').length,
     r.querySelector('todo-oge[data-id="2"]').hasAttribute('yapildi'), l.girdi.value === r.querySelector('input'),
     r.querySelector('h1').getAttribute('style')];
  const ilk = r.querySelector('todo-oge[data-id="1"]');
  await ilk.updateComplete;
  a.push(ilk.shadowRoot.querySelector('li').className === '' ? 'cls-bos' : 'cls:' + ilk.shadowRoot.querySelector('li').className);
  a.push(ilk.shadowRoot.querySelector('slot').assignedNodes().map(n => n.textContent).join(''));
  l.ekle('Peynir'); l.filtre = 'kalan';
  await l.updateComplete;
  a.push(r.querySelector('h1').textContent, [...r.querySelectorAll('todo-oge')].map(e => e.textContent).join('+'));
  a.push(ilk === r.querySelector('todo-oge[data-id="1"]') ? 'ayni-dugum' : 'yeni-dugum');
  a.push(window.__ilkGuncelleme, window.__guncellenen);
  l.isler = []; await l.updateComplete;
  a.push(r.querySelector('p') ? r.querySelector('p').textContent : 'p-yok');
  result = a.join(' | ');
})().catch(e => { result = 'HATA ' + e + ' ' + (e && e.stack); });
