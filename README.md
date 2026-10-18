# merOS Shield — Custom-Engine Web Browser

**merOS Shield** is a high-performance, security-focused web browser for Windows, built from the ground up with its own proprietary rendering engine (**MerEngine**) and JavaScript engine (**merJS**). It does **not** rely on Chromium, WebView2, Trident, Gecko, V8, or any third-party browser runtime.

![Preview](docs/onizleme.png)

---

## Building (Visual Studio 2019)

1. Double-click `merOSShield.sln`.
2. Select **Release** and **x64** from the toolbar.
3. Right-click the `merOSShield` project → **Set as Startup Project**.
4. Press **F5** (or Build Solution).

**Build Artifact:** `bin\x64\Release\merOSShield.exe` (statically-linked CRT, single standalone executable).  
Zero third-party dependencies or NuGet packages required; MSVC v142 + Windows 10 SDK (10.0.19041.0 or newer) is all you need.

**Running Test Suites:**
- `JsTests` — Comprehensive test suite for **merJS**, DOM APIs, network polyfills, TypedArrays, Intl, and i18n.
- `EngineTests` — Regression test suite for HTML parser, CSS cascade/selectors, and layout engine.

---

## Keyboard Shortcuts & CLI

| Shortcut | Action |
|---|---|
| **Ctrl + L** / **F6** / **Alt + D** | Focus address bar |
| **F5** / **Ctrl + R** | Reload current page |
| **F12** / **Ctrl + Shift + J** | Toggle developer JS console |
| **Ctrl + Shift + S** | Save full-page screenshot (PNG saved to Pictures) |
| **Alt + ← / →** | Navigate Back / Forward |
| **Alt + Home** | Go to Home (`about:home`) |
| **Ctrl + Plus / Minus / 0** | Zoom In / Out / Reset (100%) |
| **Toolbar [JS] button** | Toggle JavaScript execution on/off |

**Headless CLI Mode (Automation / Testing):**
```cmd
merOSShield.exe --snapshot snapshot.png https://example.com
```
Loads the specified webpage, captures a high-resolution screenshot to `snapshot.png`, and cleanly exits.

---

## Architecture Overview

```
URL ──► [Network] WinHTTP + Windows SChannel (HTTP/2, gzip, TLS 1.3) ──► Background Thread
           │
           ▼
        [HTML Parser] ──► DOM Tree
           │       ├─ CSS Files ┐ Parallel background downloads (Trackers blocked)
           │       └─ Scripts   ┘
           ▼
        [merJS Engine] Document-order execution (ES Modules & Classic Scripts) ──► Background
           ▼
        [Style Resolver] Cascade, inheritance, var(), @media queries ──► Background
           ▼
        [Layout Engine] Block, inline, list, table, flexbox, grid, images, forms ──► UI
           ▼
        [Renderer] Direct2D + DirectWrite (GPU-accelerated), WIC images, WMF video
           ▲
        Event Loop: Mouse/keyboard events, setTimeout/setInterval, Promises,
        dynamic <script> injection, image & CSS loading ──► Throttled restyle (~20 fps)
```

### Source File Map

| Path | Purpose |
|---|---|
| `src/html.cpp`, `dom.*` | Spec-compliant HTML parser, DOM tree, node manipulation helpers |
| `src/css.*`, `style.*` | CSS lexer/parser, selector matching, cascade and style computation |
| `src/layout.*` | Platform-independent layout engine (Block, Inline, Flex, Grid, Tables) |
| `src/js/` | **merJS**: Lexer, AST parser, bytecode/tree interpreter, mark-and-sweep GC, DOM bindings |
| `src/js/js_intl.*` | ECMA-402 Internationalization (`Intl`) engine backed by Windows NLS |
| `src/video_decoder.*` | Windows Media Foundation (WMF) hardware/software video decoding pipeline |
| `src/i18n.*` | Centralized browser localization (English default, Turkish supported) |
| `src/view.*` | DirectWrite font formatting, Direct2D GPU rendering, scrolling, hit testing |
| `src/images.*` | Windows Imaging Component (WIC) asynchronous image decoding & cache |
| `src/net.*`, `blocklist.*` | WinHTTP network client, TLS 1.3, ad/tracker blocker |
| `src/main.cpp` | Win32 application window, navigation, JS event loop, forms, security policies |

---

## merJS — JavaScript & Web Standards Engine

merJS is a custom AST-walking interpreter equipped with an exact mark-and-sweep garbage collector, full GC root registration (`HostRoot`), and strict memory limits.

### Language Specifications
- **Modern ECMAScript (ES2020+):** `let`/`const`, arrow functions, classes (`extends`, `super`, static/instance fields), tagged template literals, destructuring, spread/rest, default parameters, optional chaining (`?.`), nullish coalescing (`??`), logical assignment (`??=`), `async`/`await`, generators, `for…of`, labelled loops, property getters/setters.
- **Standard Builtins:** `Object`, `Function` (`bind`/`call`/`apply`, dynamic compilation), `Array` (methods through ES2023), `String`, `Number`, `Boolean`, `Symbol`, `Math`, `JSON`, `Date`, `Map`/`Set`/`WeakMap`/`WeakSet`, `Promise` (`all`/`allSettled`/`race`/`any`/`finally`), `Reflect`, URI encoding, base64, `eval`.
- **Typed Arrays & Buffers:** `ArrayBuffer`, `Uint8Array`, `Uint16Array`, `DataView` with complete byte manipulation capabilities.
- **Text Encoding:** `TextEncoder` and `TextDecoder` (UTF-8, byte order mark (BOM) handling, surrogate pairs).
- **Network APIs:** Standard `fetch()` with Promise support, `XMLHttpRequest`, `Headers`, `Request`, and `Response`.
- **ECMAScript Modules:** Native `<script type="module">`, static `import`/`export`, dynamic `import()`, and `import.meta.url`.
- **Media Source Extensions (MSE):** `MediaSource`, `SourceBuffer` (chunk appending for streaming protocols like DASH), and `URL.createObjectURL()`.
- **Internationalization (Intl / ECMA-402):** `Intl.DateTimeFormat`, `Intl.NumberFormat`, `Intl.Collator`, `Intl.PluralRules`, `Intl.RelativeTimeFormat`, `Intl.ListFormat`, `Intl.DisplayNames`.
- **RegExp Unicode Support:** Unicode property escapes (`\p{L}`, `\p{Number}`, `\p{ASCII}`, etc.) and Unicode control characters.

### DOM & Browser APIs
- `document` (`querySelector`/`querySelectorAll`, `getElementById`, `createElement`, `write`, `cookie` getter/setter).
- `Element` (`innerHTML`, `textContent`, `classList`, `style`, `dataset`, `value`, events, `closest`, `matches`, `insertAdjacentHTML`).
- Event propagation (bubbling, `preventDefault`, `stopPropagation`, inline `onclick`, `addEventListener`).
- `location`, `history.pushState`, in-memory `localStorage`/`sessionStorage
