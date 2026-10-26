import {LitElement, html, css} from 'lit';
class SayacButon extends LitElement {
  static properties = { sayi: {type: Number}, ad: {type: String, reflect: true} };
  static styles = css`:host { display: block; } button { color: blue; }`;
  constructor() { super(); this.sayi = 0; this.ad = 'merOS'; }
  render() {
    return html`<p>Merhaba ${this.ad}</p>
      <ul>${[1, 2, 3].map(i => html`<li>oge ${i}</li>`)}</ul>
      <button @click=${() => this.sayi++}>Tik: ${this.sayi}</button><slot></slot>`;
  }
}
customElements.define('sayac-buton', SayacButon);
window.SayacButon = SayacButon;
