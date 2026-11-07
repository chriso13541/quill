# Quill

A small document browser that builds in minutes, not a day. No JavaScript, HTTPS first,
forms and first-party session cookies, DNS-over-HTTPS built in, and a request-filter hook where the
ad/tracker blocker plugs in (milestone 4). Written in C++17, built on
its own layout/paint engine on the vendored NetSurf parsers (hubbub HTML5, libdom, libcss), libcurl, SDL2 and
two single-header stb libraries. No GTK, Qt, Cairo, FreeType, fontconfig or Boost.

> **Status:** own engine (HTML5 parse, CSS cascade with `var()`/`calc()`, blocks, inline, tables, floats, flexbox,
> grid, positioning, opacity, SVG and mask icons, old-web HTML attributes such as `bgcolor`/`<font>`/`align`, animated GIFs, framesets, `alt` text), live window with tabs, links, `:hover` / `:focus` / `:active`,
> forms (GET/POST, label click-through), images, session cookies, self-signed LAN HTTPS, back/forward and a
> search-or-URL bar. See *Limitations*.

## Build

Everything is built from this tree: the HTML/CSS parsers (NetSurf hubbub, libdom, libcss, libparserutils,
libwapcaplet) and the image/font headers are vendored in `third_party/`, so nothing is downloaded at build time and
no Perl, gperf or Python is needed. You install a C++17 compiler, CMake >= 3.16 (Ninja optional), **SDL2**,
**libcurl** (with an SSL backend) and, optionally, **libwebp** (WebP images) and **Brotli**'s decoder (WOFF2 web
fonts, which most sites send; WOFF and TrueType work without it). System fonts are found at runtime (Liberation,
DejaVu, Noto, FreeFont, or Windows' own).

```sh
# Gentoo (names may differ slightly by overlay/version)
emerge --ask media-libs/libsdl2 net-misc/curl media-libs/libwebp app-arch/brotli dev-build/cmake dev-build/ninja media-fonts/liberation-fonts

cmake -S . -B build -G Ninja          # omit -G Ninja to use Makefiles
cmake --build build -j"$(nproc)"      # builds the browser, the NetSurf libraries and the tests
./build/quill                         # or: ./build/quill "some search terms"
ctest --test-dir build                # core + engine tests
```

Targets (each `cmake --build build --target NAME`, handy to rebuild less):

| Target | What it is |
|---|---|
| `quill` | the browser |
| `quill_tests` | networking / URL / cookie / policy tests |
| `browser_tests` | browser behaviour (http rule, `:visited`, framesets, meta refresh, render cache) on a fake network |
| `net_tests` | real libcurl against a little HTTP server on 127.0.0.1: connection sharing, per-host limit, HTTP cache revalidation, sign-in (not on Windows) |
| `engine_tests` | layout and style tests: every `tests/engine/cases/*.html` carries `<!-- rect: #id = x y w h -->` / computed-style expectations |
| `enginedump`, `enginerender`, `engine_smoke` | dev tools, only with `-DQUILL_ENGINE_TOOLS=ON` |

Options: `-DQUILL_BUILD_TESTS=OFF` (skip the tests), `-DQUILL_WEBP=OFF`, `-DQUILL_WOFF2=OFF`, `-DQUILL_HARDENING=OFF`,
`-DCMAKE_BUILD_TYPE=Debug`. After the first build only the files you touched recompile; the NetSurf libraries
(about 400 files) are the bulk of a clean build and never change. Flags are `-O2`, no LTO.

Dev tools: `build/enginedump page.html [--viewport WxH] [--boxes] [--rect '#id']` (set `QUILL_DUMP_GEOM=1` to print box
geometry), `build/enginerender page.html out.png`, and the headless screenshot of any URL:
`quill --dump out.png --width 1280 --height 800 URL`. `QUILL_PROF=1` prints style-cascade timings.

**Windows:** the code is written to be portable (winsock, `std::filesystem`, `SDL_MAIN_HANDLED`, Schannel/native CA
store through `CURLSSLOPT_NATIVE_CA`). In an MSYS2 **UCRT64** shell:
`pacman -S --needed git mingw-w64-ucrt-x86_64-{toolchain,cmake,ninja,SDL2,curl,libwebp,brotli}`, then the cmake commands above.
Use one environment consistently (UCRT64 *or* MINGW64) and delete `build/` if you switch.

## Using it

Type in the address bar (`Ctrl+L`):

| You type | Quill opens |
|---|---|
| `example.com/page` | `https://example.com/page` |
| `192.168.1.1`, `router.lan`, `localhost:8080`, `nas/` | `http://...` (private hosts open directly) |
| `https://...` / `http://...` | as typed (public `http://` asks first) |
| anything else, e.g. `how does dns work` | your search engine (`search_url`, default DuckDuckGo HTML) |

If a bare LAN address refuses plain http (e.g. Crafty Controller on `192.168.1.5:8443`), Quill retries it as https.

A single bare word (`dns`) is a search, not a host. Write `dns/` or `http://dns` for a LAN host named `dns`.

Tabs: `Ctrl+T` new tab, `Ctrl+W` close (the last one quits), `Ctrl+Shift+T` reopen the last closed tab,
`Ctrl+Tab` / `Ctrl+Shift+Tab` or `Ctrl+PgDn` / `Ctrl+PgUp` switch, `Ctrl+1`..`9` jump (9 = last). Middle-click
or `Ctrl`+click a link opens it in a background tab; middle-click a tab, or its ×, closes it. Every tab keeps
loading in the background.

Keys: `Enter` go, `Esc` leave the bar, `Alt+Left/Right` back/forward, `F5`/`Ctrl+R` reload, `Ctrl+H` history,
`Ctrl +` / `Ctrl -` / `Ctrl 0` (or `Ctrl`+wheel) zoom in / out / back to normal,
arrows / `PgUp` / `PgDn` / `Space` / `Home` / `End` / mouse wheel scroll, `Ctrl+Q` quit.

Selecting text: drag across it (dragging past the top or bottom of the window scrolls), double-click a word,
triple-click a paragraph, shift+click to extend, `Ctrl+A` for the whole page. `Ctrl+C` or the right-click menu's
*Copy* puts it on the clipboard (wrapped lines come out as one line, paragraphs and preformatted lines on their
own); *Paste* in a text field's menu, or `Ctrl+V`.

Sign-in prompts (HTTP authentication): a site that answers `401` with `WWW-Authenticate: Basic` or `Digest` (routers,
NAS boxes, self-hosted dashboards) gets Quill's sign-in page in place of its error page: the site's address and
the realm it named, a warning when a Basic password would go over plain http, *Sign in* and *Cancel* (which shows
the site's own 401 page). What you enter is kept in memory for that origin until you quit and sent only for pages
opened on it and those pages' own requests (a page on another site that embeds the router's pictures doesn't get
to use your login); a wrong one is forgotten and you are asked again. Never written to disk, never in history or
the address bar.

Zoom: like browsers' full-page zoom, from 30% to 300%. Everything is laid out again at the new size (text stays
sharp, `srcset` picks sharper pictures when zoomed in, media queries see the narrower page, so a zoomed-in site may
switch to its phone layout). It is remembered per site for the session, every tab on that site follows, and a
badge in the address bar shows the level (click it to reset). `zoom = 150` in the settings makes 150% the
default, e.g. for a high-DPI screen (Quill's own toolbar doesn't scale with it).

Scrolling: the wheel (and the arrow / page keys) scroll whatever is under the mouse: a box with its own scroll bar
(`overflow: auto` / `scroll`, such as the panes of an app-style layout) until it reaches its end, then the box
around it, then the page. `Shift`+wheel (or a sideways wheel) scrolls boxes sideways. The page's scroll bar and
boxes' scroll bars can be dragged, and a click on the page's scroll track jumps there.

History (`Ctrl+H`, or `about:history`): the pages you visited, newest first and grouped by day, with a search box
(matches addresses and titles) and a *Clear history* button. Kept between sessions in `history_file` (default
`~/.local/share/quill/history.tsv`, readable only by you), visits older than `history_days` (90) are dropped, and
`history = false` turns it off. Only pages you open count, not frames or Quill's own pages; links to pages in the
history stay purple (`:visited`) after a restart. Web pages can't open or clear it.
Forms: click a field to type (text, password, textarea, checkbox, radio, select, buttons); `Enter` submits a
single-line field's form. Reload/back/forward onto a POST result asks "Resend form data?" first.
In the bar: `Ctrl+A`, `Ctrl+C`, `Ctrl+V`, `Home`, `End`, arrows.

Slow or failing pages: run `QUILL_NETLOG=1 quill` from a terminal. Every request prints a line with its status,
HTTP version and where the time went (DNS, and whether that was DNS-over-HTTPS; connect; TLS; first byte; total;
whether a connection was reused). A slow `dns(doh)` points at the DoH server (try another `doh_url`, or
`doh_fallback = true`); a slow first byte is the site itself.

Caching: stylesheets, pictures and fonts are kept in memory (64 MB, `http_cache_mb`) for as long as their
`Cache-Control` / `Expires` say, and revalidated with `If-None-Match` / `If-Modified-Since` after that, so the next
page on a site doesn't download its stylesheets again. The cache is split by the site that loaded them, like
browsers' (one site can't time whether you've been to another), and lasts for the session (`http_cache = false`
turns it off). A request that fails because the server had quietly closed the kept-alive connection it went over
is sent again once on a new connection (GET only).

Loading: Quill looks up and connects to the search engine as it starts, and to a link's host while the mouse
button is still down on it (`preconnect = false` turns both off). Looked-up addresses are kept 10 minutes. A page
waits at most 1 s for its web fonts (none for `font-display: swap`, 100 ms for `fallback` / `optional`), then
shows in fallback fonts and switches when they arrive.

Headless screenshot of a whole page (handy for debugging and tests):
`quill --dump out.png --width 1000 URL`

## Start page and icons

`about:home` (the start page, also the Home button) has a search box using your `search_url` and shortcut tiles you
set with `home_links = Name|https://url, ...`. The toolbar and tab icons are Google's Material Icons, embedded as SVG
paths. To use another set (e.g. Material Symbols or Noto), set `icon_dir` to a folder of `<name>.svg` files; any of
`back forward reload close add home shield-check lock lock-open key info warning search` found there replaces the
built-in one (drawn as a tinted single-colour mask).

## Configuration

`~/.config/quill/config.ini` (Linux) or `%APPDATA%\quill\config.ini`; override with `--config FILE` or
`QUILL_CONFIG`. Copy [`quill.ini.example`](quill.ini.example), every key is optional and documented there:
search engine, DoH server, `http_allow`, `private_allow`, user agent, proxy, font directory, limits.

## HTML support worth knowing about

- **`<iframe>`**: each one is a small page of its own (up to 16 per page, nested 4 deep), painted into the page so it
  scrolls and clips with it. Links inside stay in the frame (`target=_top` / `_parent` / a frame's `name` work), the
  wheel scrolls the frame while it can move, `srcdoc` works. An iframe's page is fetched like any other part of its
  page: no private-network addresses from a public page, no plain http inside an https page, and sites that forbid
  framing (`X-Frame-Options`, CSP `frame-ancestors`) show a "doesn't allow being shown inside another page" note with a
  link instead. Nothing a frame loads ever adds a plain-http exception.
- **New tabs**: `target="_blank"` links and forms (POST included), and targets naming no frame, open a new tab next
  to the current one.
- **`<details>`/`<summary>`**: closed until clicked (or `open`), with the disclosure triangle unless the page styles
  its own.
- **`srcset`, `sizes`, `<picture>`**: `<source media type srcset>` is honoured (types Quill can't decode are skipped),
  `w`/`x` descriptors pick the smallest picture that is sharp enough, and a 2x picture is drawn at its CSS size.
- **Image maps**: `<img usemap>` with `rect`/`circle`/`poly`/`default` areas (hover shows the URL, `target` works), and
  server-side `<a><img ismap></a>` maps (the click position is sent as `?x,y`).
- Media Queries 4 range syntax (`@media (width >= 768px)`, which current Bootstrap builds use) and flex items'
  automatic minimum size (flex columns overflow instead of squashing their items).
- **Web fonts** (`@font-face`): TrueType, WOFF and WOFF2 (with Brotli), `font-weight` ranges, `font-style`,
  `unicode-range` (only the subsets the page's text needs are downloaded) and `src` lists (the first format Quill can
  use wins). Fonts the page has in use arrive before the first layout if they come within 1.5 s; slower ones swap in
  when they land. Icon fonts (Font Awesome, Material Icons, Bootstrap Icons...) work; without their font, icon
  characters are left blank instead of drawn as boxes. A page's fonts are its own: another tab can't see them (a page
  defining "Arial" restyles only itself). CFF-outline `.otf` fonts aren't supported (Quill draws TrueType outlines),
  and variable fonts show their default instance (bold is synthesised).
- **Site icons**: `<link rel="icon">` (PNG, ICO, SVG, GIF, JPEG, WebP, preferring a 32x32 or 16x16 one) or the site's
  `/favicon.ico`, shown in the tab strip. Fetched like any part of the page (same private-network / mixed-content rules).
- **Gradients**: `linear-`, `radial-` and `conic-gradient()` and their `repeating-` forms (angles, `to` sides and
  corners, colour-stop positions, radial sizes and positions, the old `-webkit-` syntax), in `background` /
  `background-image`, layered with `url()` images and `background-size` / `-repeat`. Stops blend in premultiplied
  colour and hard stops are anti-aliased.
- **`transform`**: `translate*`, `rotate`, `scale*`, `skew*`, `matrix` and the `rotate` / `scale` / `translate`
  properties, about `transform-origin`. A transformed element is painted off-screen and resampled (smooth edges),
  and is hit where it is drawn for translations; rotated or scaled boxes are hit at their untransformed place. No 3D.
- **`position: sticky`**: `top` / `bottom` insets, kept inside the containing block; clickable where it is drawn.
  (Fixed boxes are now hit where they are drawn after scrolling, too.)
- **Hover without layout**: a `:hover` / `:focus` rule that only changes colours, backgrounds, borders' colour,
  shadows, opacity or rotate / scale transforms restyles the affected boxes in place and repaints, without building boxes or laying
  out again (about 3.5 ms instead of 55 ms on a long Wikipedia article). Rules that change sizes still lay out.
- **Forms**: `<select multiple>` and `<select size=N>` are list boxes (click, ctrl+click to add or remove, shift+click
  for a range, arrow keys, the wheel scrolls the list; every chosen value is submitted). `<input type=file>` opens the
  system file chooser (zenity or kdialog on Linux/BSD, the standard dialog on Windows; `accept` becomes its filter,
  `multiple` works) and forms with `enctype="multipart/form-data"` upload the files (read when the form is sent, 100 MB
  in all at most). Without multipart only the file names are sent, as browsers do.
- **`<base href>`** sets the base of every relative URL on the page (links, images, stylesheets, forms, frames).
- **`<object>` / `<embed>`**: Quill has no plugins. An `<object data>` / `<embed src>` that is a picture is shown as an
  image, one that is a page is shown like an `<iframe>` (same rules), and anything else (Flash, Java, PDF...) shows the
  `<object>`'s fallback content, or an empty box of the `<embed>`'s size.
- **`<marquee>` and `<blink>`**, for the old-net crowd: `behavior` (scroll, slide, alternate), `direction` (left, right,
  up, down), `scrollamount`, `scrolldelay` (60 ms at least unless `truespeed`), `loop`, `bgcolor` / `width` / `height`;
  links in a marquee can be clicked where they are drawn. `<blink>` and `text-decoration: blink` blink once a second.
- **Overflow and full-height layouts**: `overflow` on `<html>` / `<body>` applies to the window (so a
  `body { overflow-x: hidden }` page, like the Gentoo package site's full-height flex layout, still scrolls and paints
  below the first screen). A single-line flex container with a fixed height keeps its row that height, so
  `overflow: auto` columns in it scroll on their own instead of stretching the page.
- **Caches**: sanitized fonts (48 MB in memory, shared by all tabs) and icons are kept in memory, and on disk under
  `cache_dir` (default `~/.cache/quill`; fonts up to 96 MB / 30 days, icons 8 MB / 7 days, oldest first out).
  `disk_cache = false` keeps them in memory only; `web_fonts = false` / `favicons = false` turn the features off.

## Rendering and memory

Painting skips everything outside the screen (each box knows how far its content reaches, computed once per layout),
so a frame costs what is visible, not the length of the page. On top of that each tab keeps the page it has already
painted in memory, in 256 px strips (`render_cache_mb`, 128 MB by default), and while nothing else is happening it
paints the strips above and below the screen ahead of time (`prerender`). Scrolling over painted parts is then a
memory copy. `position:fixed` boxes, the focus ring, dropdowns and the scrollbar are drawn live on top every frame.
A tab that has not been shown for 30 seconds frees its strips.

Measured on an 84 000 px article at 1000x700, per scroll frame: 42 ms before, 3.7 ms with culling alone, 0.7 ms
with the cache filling as you scroll, 0.2 ms after the idle pre-render. `enginerender page.html out.png
--bench-scroll` prints the same numbers for any page on your machine; `--verify-cull` checks that culling paints
exactly what a full paint does.

Loading: text measurement caches each font's kerning and word widths, styles remember their font, and the box
builder reads each element's attributes once, which made layout of a long Wikipedia-sized article about 6x faster.
Stylesheets download in parallel. Every request (all tabs, frames and worker threads) runs in one libcurl "multi"
transfer loop: one connection pool for the whole browser, at most 6 connections per host over HTTP/1.1, and over
HTTP/2 a page's requests to a site share a single connection, side by side (`QUILL_NO_MULTI=1` runs each transfer
on its own thread instead, for comparison); DNS and TLS sessions are cached across them too;
a picture whose box already has a fixed size (every Wikipedia thumbnail) is painted when it arrives without
re-laying out the page. `QUILL_PROF=1 quill <url>` prints where a load spends its time (download, stylesheets,
CSS compile, selector matching, box building, layout).

On slow CPUs (tuned against a dual-core ThinkPad-class machine, where a load runs several times slower than on a
desktop):

- A page load never waits behind images. Loads run on their own worker threads; on the shared image pool a click
  used to queue behind every picture the current page still had coming (seconds, on a slow link or CPU).
- The page you leave is freed on a worker after the new one is up, instead of on the UI thread in between, and a
  page laid out on the worker is not laid out a second time when it is shown.
- Parsing skips libdom's mutation events (nothing listens without script) and passes ASCII straight through the
  UTF-8 decoder; selector matching caches tag names, class lists and :link / :visited answers per element and
  shares one computed style between alike siblings; compiled stylesheets are shared between pages (the next page
  of a site reuses them); the CSS pre-passes lost a quadratic scan; rounded corners and box shadows paint only
  their edges per pixel.
- While pictures keep arriving, re-layouts are spaced out by how long a layout takes (so scrolling stays smooth),
  hover / focus changes no longer block the mouse while a worker holds the engine, and `font-display: auto` web
  fonts are waited for 400 ms (was 1 s) before the page shows in a fallback font and swaps when they arrive.

CPU work for loading a page twice and painting it (callgrind instruction counts, same pages, before -> after): a
Wikipedia-sized article 5.5 G -> 3.7 G (-33 %), a Bootstrap site 1.5 G -> 0.83 G (-45 %), the DuckDuckGo
results page 0.41 G -> 0.33 G (-21 %).

## Security model

Enforced for every request (page, stylesheet, image and **each redirect hop**) in `src/policy.cpp` and `src/net.cpp`:

- **HTTPS first.** Bare hostnames become `https://`. Public `http://` top-level pages show a warning page with
  "try https" / "continue" (remembered per host for the session, permanent via `http_allow`). By default
  (`http_warning = typed`) that is asked for addresses you type and when an https page sends you to http; links
  followed from a page that is already plain http open directly, like in Firefox. `always` / `never` are the
  alternatives. On an https page,
  plain-http *subresources* are blocked (images are first retried over https). A plain-http page you chose to open
  may load plain-http images and stylesheets from other public hosts, since it gains nothing from blocking them.
  TLS 1.2+, certificate verification always on, no bypass button.
- **HSTS** (RFC 6797). A site that sends `Strict-Transport-Security` over a verified https connection is only ever
  contacted over https for the rest of the session (`includeSubDomains` honoured, `max-age=0` clears it, never for IP
  addresses or pinned LAN certificates). Built in: the HTTPS-only TLDs (`.app`, `.dev`, `.page`, `.bank`, ...) and a few
  big preloaded sites (Wikipedia, GitHub, DuckDuckGo, PayPal, Proton, EFF, ...). The upgrade happens before the request
  leaves, on every redirect hop, so a plain-http link or a tampering network can't downgrade those sites.
- **Lookalike domains.** Hosts with non-ASCII characters are shown in the address bar and status bubble as their
  punycode (`xn--...`) form, so `аpple.com` written with a Cyrillic `а` can't pass for `apple.com`.
- **Address bar indicator.** Green shield = verified https; amber key = a LAN host you trusted by key; red open lock =
  public plain http ("Not secure"); grey open lock = plain http on your own network. Click it for the details panel.
- **Private network isolation.** A page from the public internet cannot load subresources from, or redirect to,
  private addresses (`10/8`, `172.16/12`, `192.168/16`, `127/8`, `169.254/16`, `100.64/10`, `fc00::/7`, `fe80::/10`,
  single-label names, `.lan`, `.local`, ...). This stops "visit evil.com, it hits your router's admin URL".
  Typing or clicking a private URL is allowed.
- **DNS-rebinding defence.** Public names that resolve to private addresses are refused at connect time
  (libcurl socket hook), so `evil.com -> 192.168.1.1` fails. Names you list in `private_allow` are exempt.
- **DNS-over-HTTPS** for public names via libcurl's `CURLOPT_DOH_URL`; private names use the system resolver.
- **Self-signed certificates (LAN only).** A private host (e.g. Crafty Controller, a NAS) with a certificate that
  does not verify shows its fingerprint and lets you trust *that exact key* (pinned by SPKI SHA-256, session-only, or
  permanent via `trusted_certs`). If the key later changes, the connection is refused. Public hosts never get this.
- **Visited links** (`:visited`) come from this session and, with `history` on, your history. Pages can't read
  them back (no JavaScript); when scripting arrives, `:visited` styles must stay invisible to scripts.
- **Referer, capped.** Like browsers' default (`strict-origin-when-cross-origin`): a page's full address only goes to
  its own origin, other https sites get just its origin (`https://site/`), plain http gets nothing. Pages may ask
  for less (`Referrer-Policy`, `<meta name="referrer">`, `rel="noreferrer"`) but never more (`unsafe-url` is
  treated as the default). `referrer = origin` sends only origins, `off` none at all. Quill's own pages never refer.
  (Without one, many sites' form checks fail: DuckDuckGo's HTML search form, Django sites' CSRF check, ...)
- **Request headers like the browser it names.** The default User-Agent is Firefox's, so Quill asks like Firefox:
  the same `Accept`, `Upgrade-Insecure-Requests` and Fetch Metadata (`Sec-Fetch-Site/-Mode/-Dest/-User`). Bot
  filters notice a "Firefox" that doesn't send them. (They can still tell Quill's TLS handshake isn't Firefox's.)
- **Cookies:** first-party only, session-only (never written to disk unless you set it), `Secure` honoured, not sent on
  cross-site POSTs. A plain-http response can't overwrite or delete a `Secure` cookie (RFC 6265bis). A form on a public page cannot submit to a private address.
- No JavaScript, no `file:`/`ftp:`/`data:` navigation, no credentials in URLs,
  size and time limits, images restricted to PNG/JPEG/GIF/BMP.
- Internal `quill:` commands (the "continue over HTTP" button) only work from Quill's own pages, never from web content.
- **Web fonts are sanitized** (`src/fontsan.cpp`), like browsers' OpenType Sanitizer: stb_truetype is not written for
  hostile input, so a downloaded font is never handed to it. Quill decodes WOFF/WOFF2 itself (with size limits),
  checks every table it keeps (each glyph's outline data the way stb_truetype will read it, composite glyphs for
  loops and depth, `cmap` subtables, `hmtx` length) and writes a fresh minimal TrueType font from the checked data;
  hinting programs, kerning and layout tables are dropped. Fuzzed (hundreds of thousands of mutated TrueType / WOFF /
  WOFF2 files under AddressSanitizer + UBSan, drawing every glyph of each accepted one). The disk cache only stores
  such rebuilt fonts, and they are checked again when read back.

Not covered: there is **no process sandbox**; a memory bug in the NetSurf parsers, the engine, stb, nanosvg, libwebp or libcurl is
exploitable. (Image decoders are the usual weak spot: keep libwebp up to date, it is the system's copy on purpose.)
Keep libraries updated and treat Quill as a *reading* browser. Don't log into anything valuable with it yet.
Certificate revocation (OCSP/CRL) and Certificate Transparency are not checked; HSTS
is remembered for the session only. The DoH provider sees your lookups and SNI still reveals hostnames (no ECH). When an HTTP proxy is configured, the
rebinding check is skipped because the proxy does the resolving.

## Code map

```
src/url.*          URL parsing, RFC 3986 resolution, helpers
src/policy.*       HTTPS-first / private-network / resolver decisions, IP classification
src/net.*          libcurl fetching (one multi transfer loop), manual redirects, DoH, socket-open hook, HTTP auth
src/reqheaders.*   Accept / Referer / Sec-Fetch headers, credential store (no libcurl: the tests use it too)
src/httpcache.*    in-memory HTTP cache (Cache-Control, revalidation, per-site partitions)
src/engine/selection.*  text selection: what text is where, the text between two points
src/filter.h       RequestFilter interface: where the ad/tracker blocker attaches (blocks + cosmetic CSS)
src/input.*        address bar text -> URL or search
src/settings.*     config file
src/engine/*       Quill's engine: document (libdom/libcss + interaction state), var()/calc pre-pass, per-element CSS (elemcss),
                   HTML presentational attributes (hints: bgcolor, text/link, <font>, table border/cellpadding, align, ...),
                   box builder, layout (block/inline/table/float/flex/grid), grid.* (grid CSS parsing), paint, hit-testing
src/page.*         per-page shared state and the async image loader
src/browser.*      navigation, history, page loading on worker threads, layout, scrolling, mouse input; framesets
                   (each <frame> is a child Browser; input, keys and status are routed to it)
src/framebuffer.*  software renderer (clip, fills, rounded rects, image blits)
src/font.*         stb_truetype fonts, system font discovery, glyph fallback
src/forms.*        form controls (FormControl state, painting, list boxes, urlencoded / multipart encoding)
src/history.*      browsing history (about:history): visits on disk, search, clearing
src/filepick.*     the native file chooser for <input type=file> (zenity / kdialog / GetOpenFileNameW)
src/cookies.*      first-party session cookie jar
src/certpin.*      SPKI pinning, SHA-256, base64
src/fontsan.*      web font sanitizer: WOFF / WOFF2 decoding, validation, rebuilding a minimal TrueType font
src/diskcache.*    on-disk cache (fonts, site icons): hashed file names, per-bucket size caps, expiry
src/image.*        image decoding (stb_image, libwebp, SVG), sniffing by content, resampling
src/svg.*          nanosvg wrapper: SVGs are re-rendered at the size they are drawn at
src/hsts.*         Strict-Transport-Security store and preload list
src/chrome.*       window UI drawn headless: tabs, toolbar, address bar + security indicator, status bubble, menus,
                   site-info panel, built-in icons (testable without SDL)
src/main.cpp       SDL window and event handling, --dump mode
tests/test_core.cpp        core tests
tests/engine/             engine tests: cases/*.html with inline expectations
tests/browser/            browser tests (http rule, :visited, framesets) against an in-memory site (fake_net.cpp)
tests/net/                network tests: real libcurl against a small HTTP server on 127.0.0.1 (Linux / BSD / macOS)
```

## Limitations (what you will notice)

No JavaScript (by design) so JS-built pages and web apps (Google search results, the Crafty dashboard's live panels)
are partial; forms and logins work. No find-in-page; Quill's own toolbar doesn't scale for HiDPI (pages do, with
`zoom`); no animated WebP (first frame only; animated GIFs play), no `<video>` playback. Text selection is the page's
text only (not text inside form fields, which select all or nothing), and doesn't follow `user-select: none`.
Back/forward inside a frameset steps through the frames' pages; a frameset nested inside a frame keeps no history of
its own, and navigation inside an `<iframe>` is not a history entry. A link to a named window that doesn't exist
opens a new tab each time (no reuse by name).
The page itself doesn't scroll sideways (boxes do); a page wider than the window is cut off on the right.
Inside a scrolled box, iframes take clicks (and text fields place the caret, list boxes pick options) as if the box
were unscrolled, and `#anchor` links don't scroll a box to their target.
CSS: no transitions/animations or 3D transforms (rotated / scaled boxes are clicked at their untransformed place),
`em` inside radius/shadow values is approximated, subgrid and
`calc()` inside grid tracks are not supported, `:focus-visible` behaves like `:focus`. SVG is nanosvg's plus
Quill's mask/CSS-property handling: shapes, paths, gradients and transforms work; text, filters and clip paths do not.
Quill draws its own focus ring on the focused control, even when the page sets `outline: none`.

## Roadmap

3. find-in-page, HiDPI toolbar, session restore.
4. Ad/tracker blocker: EasyList-style network rules with token-indexed matching, cosmetic filters via
   `RequestFilter::cosmetic_css`, tracking-parameter stripping, list updates. The hooks already exist.
5. Polish: HarfBuzz shaping, Windows build, hardening.

## Third-party

NetSurf hubbub, libdom, libcss, libparserutils, libwapcaplet (MIT, vendored in `third_party/netsurf`), stb_truetype / stb_image / stb_image_write
(public domain / MIT, vendored in `third_party/stb`), nanosvg (zlib license, vendored in `third_party/nanosvg`),
Material Icons paths in `src/chrome.cpp` (Google, Apache License 2.0), Brotli (MIT, system library, optional),
test fonts in `tests/fonts` (subset of DejaVu Sans, Bitstream Vera / DejaVu license), SDL2 (zlib), libcurl (curl license), libwebp (BSD-3-Clause, system library).
