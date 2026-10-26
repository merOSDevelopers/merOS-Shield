import {PolymerElement, html} from '@polymer/polymer/polymer-element.js';
import '@polymer/polymer/lib/elements/dom-repeat.js';
import '@polymer/polymer/lib/elements/dom-if.js';
class YtdKart extends PolymerElement {
  static get template() { return html`<style>:host{display:block} .t{font-weight:bold}</style>
    <div class="t" id="baslik">[[baslik]]</div><span id="sayi">{{sayi}}</span>
    <template is="dom-repeat" items="[[videolar]]"><p class="v" on-click="_tik">[[index]]:[[item.ad]]</p></template>
    <template is="dom-if" if="[[goster]]"><b id="ek">ek icerik</b></template><slot></slot>`; }
  static get properties() { return { baslik: {type: String, value: 'Ana Sayfa'}, sayi: {type: Number, value: 0, notify: true, reflectToAttribute: true},
    videolar: {type: Array, value: () => [{ad: 'Video A'}, {ad: 'Video B'}]}, goster: {type: Boolean, value: false} }; }
  _tik(e) { this.sayi++; this.sonTik = e.model.item.ad; }
}
customElements.define('ytd-kart', YtdKart);
