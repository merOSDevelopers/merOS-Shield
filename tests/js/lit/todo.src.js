import {LitElement, html, css, nothing} from 'lit';
import {repeat} from 'lit/directives/repeat.js';
import {classMap} from 'lit/directives/class-map.js';
import {styleMap} from 'lit/directives/style-map.js';
import {when} from 'lit/directives/when.js';
import {ref, createRef} from 'lit/directives/ref.js';
class ToDoOge extends LitElement {
  static properties = { yapildi: {type: Boolean, reflect: true} };
  static styles = css`:host([yapildi]) { text-decoration: line-through; }`;
  render() { return html`<li class=${classMap({bitti: this.yapildi})}><slot></slot></li>`; }
}
class ToDoListe extends LitElement {
  static properties = { isler: {state: true}, filtre: {} };
  constructor() { super(); this.isler = [{id: 1, ad: 'Ekmek', ok: false}, {id: 2, ad: 'Süt', ok: true}]; this.filtre = 'hepsi'; this.girdi = createRef(); }
  get gorunen() { return this.filtre === 'hepsi' ? this.isler : this.isler.filter(i => !i.ok); }
  ekle(ad) { this.isler = [...this.isler, {id: this.isler.length + 1, ad, ok: false}]; }
  firstUpdated() { window.__ilkGuncelleme = (window.__ilkGuncelleme || 0) + 1; }
  updated(ch) { window.__guncellenen = [...ch.keys()].join(); }
  render() {
    return html`<h1 style=${styleMap({color: 'red'})}>Liste (${this.gorunen.length})</h1>
      <input ${ref(this.girdi)} value="x">
      <ul>${repeat(this.gorunen, i => i.id, i => html`<todo-oge ?yapildi=${i.ok} data-id=${i.id}>${i.ad}</todo-oge>`)}</ul>
      ${when(this.gorunen.length === 0, () => html`<p>Bos</p>`, () => nothing)}`;
  }
}
customElements.define('todo-oge', ToDoOge);
customElements.define('todo-liste', ToDoListe);
