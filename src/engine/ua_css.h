// Default (user-agent) stylesheet for the Quill engine. Author styles override it; libcss applies it
// with origin UA. Based on the HTML living standard's rendering section, trimmed to what the engine
// can lay out so far. Keep every rule here honest: if layout can't do it yet, don't claim it.
#pragma once

namespace quill::engine {

inline const char* kUaCss = R"CSS(
html, address, blockquote, body, center, dialog, div, figure, figcaption, footer, form, header, hr,
legend, listing, main, p, plaintext, pre, search, xmp, article, aside, nav, section, details, summary,
fieldset, dd, dl, dt, menu, dir, ol, ul, noscript, hgroup, h1, h2, h3, h4, h5, h6, optgroup, caption {
  display: block;
}
[hidden], area, base, basefont, datalist, head, link, meta, noembed, param, rp, script, style, template,
title, source, track {
  display: none;
}
dialog { display: none; }
dialog[open] { display: block; }
audio { display: none; }
audio[controls] { display: inline-block; }
li { display: list-item; }
table { display: table; }
caption { display: table-caption; }
colgroup { display: table-column-group; }
col { display: table-column; }
thead { display: table-header-group; }
tbody { display: table-row-group; }
tfoot { display: table-footer-group; }
tr { display: table-row; }
td, th { display: table-cell; }
img, input, button, select, textarea, video, canvas, svg, iframe, object, embed, meter, progress {
  display: inline-block;
}
input[type=hidden] { display: none; }
marquee { display: block; overflow: hidden; white-space: nowrap; text-align: start; }
marquee[direction=up], marquee[direction=down], marquee[direction=UP], marquee[direction=DOWN] { white-space: normal; height: 200px; }
blink { text-decoration: blink; }
iframe { border: 2px inset #a0a0a0; }
body { margin: 8px; }
p, dl, menu, ol, ul, pre, listing, plaintext, xmp, blockquote, figure, fieldset { margin: 1em 0; }
blockquote, figure { margin-left: 40px; margin-right: 40px; }
dd { margin-left: 40px; }
ol, ul, menu { padding-left: 40px; }
ul ul, ol ul, ul ol, ol ol { margin-top: 0; margin-bottom: 0; }
ul { list-style-type: disc; }
ol { list-style-type: decimal; }
ul ul { list-style-type: circle; }
ul ul ul { list-style-type: square; }
h1 { font-size: 2em; margin: 0.67em 0; }
h2 { font-size: 1.5em; margin: 0.83em 0; }
h3 { font-size: 1.17em; margin: 1em 0; }
h4 { margin: 1.33em 0; }
h5 { font-size: 0.83em; margin: 1.67em 0; }
h6 { font-size: 0.67em; margin: 2.33em 0; }
h1, h2, h3, h4, h5, h6, b, strong, th { font-weight: bold; }
i, cite, em, var, address, dfn { font-style: italic; }
pre, listing, plaintext, xmp, code, kbd, samp, tt { font-family: monospace; }
pre, listing, plaintext, xmp { white-space: pre; }
small { font-size: smaller; }
big { font-size: larger; }
sub { vertical-align: sub; font-size: smaller; }
sup { vertical-align: super; font-size: smaller; }
u, ins { text-decoration: underline; }
s, strike, del { text-decoration: line-through; }
mark { background: yellow; color: black; }
a:link { color: #0000ee; text-decoration: underline; }
a:visited { color: #551a8b; text-decoration: underline; }
hr { color: gray; border: 1px inset; margin: 0.5em auto; overflow: hidden; }
center { text-align: center; }
th { text-align: center; }
table { border-spacing: 2px; border-collapse: separate; box-sizing: border-box; }
td, th { padding: 1px; vertical-align: inherit; }
thead, tbody, tfoot, tr { vertical-align: middle; }
caption { text-align: center; }
img { }
nobr { white-space: nowrap; }
input, button, select, textarea { font-family: sans-serif; font-size: 13px; font-weight: normal; font-style: normal; line-height: normal; margin: 0; vertical-align: middle; }
input, textarea, select, button { text-align: start; text-indent: 0; box-sizing: content-box; }
input, textarea { background: #ffffff; color: #000000; border: 1px solid #767676; padding: 2px 4px; }
button, input[type=button], input[type=submit], input[type=reset] {
  background: #efefef; color: #000000; border: 1px solid #767676; padding: 2px 10px; text-align: center;
}
input[type=checkbox], input[type=radio] { width: 13px; height: 13px; padding: 0; border: none; background: transparent; margin: 3px 4px 3px 2px; }
input[type=image] { border: 0; padding: 0; background: none; }
textarea { font-family: monospace; white-space: pre-wrap; }
select { background: #ffffff; color: #000000; border: 1px solid #767676; padding: 2px 6px; }
select[multiple] { padding: 1px; }
input[type=file] { border: none; padding: 0; background: transparent; }
fieldset { margin: 1em 0; }
blockquote, figure { margin-left: 40px; margin-right: 40px; }
dd { margin-left: 40px; }
ol, ul, menu { padding-left: 40px; }
ul ul, ol ul, ul ol, ol ol { margin-top: 0; margin-bottom: 0; }
ul { list-style-type: disc; }
ol { list-style-type: decimal; }
ul ul { list-style-type: circle; }
ul ul ul { list-style-type: square; }
h1 { font-size: 2em; margin: 0.67em 0; }
h2 { font-size: 1.5em; margin: 0.83em 0; }
h3 { font-size: 1.17em; margin: 1em 0; }
h4 { margin: 1.33em 0; }
h5 { font-size: 0.83em; margin: 1.67em 0; }
h6 { font-size: 0.67em; margin: 2.33em 0; }
h1, h2, h3, h4, h5, h6, b, strong, th { font-weight: bold; }
i, cite, em, var, address, dfn { font-style: italic; }
pre, listing, plaintext, xmp, code, kbd, samp, tt { font-family: monospace; }
pre, listing, plaintext, xmp { white-space: pre; }
small { font-size: smaller; }
big { font-size: larger; }
sub { vertical-align: sub; font-size: smaller; }
sup { vertical-align: super; font-size: smaller; }
u, ins { text-decoration: underline; }
s, strike, del { text-decoration: line-through; }
mark { background: yellow; color: black; }
a:link { color: #0000ee; text-decoration: underline; }
a:visited { color: #551a8b; text-decoration: underline; }
hr { color: gray; border: 1px inset; margin: 0.5em auto; overflow: hidden; }
center { text-align: center; }
th { text-align: center; }
table { border-spacing: 2px; border-collapse: separate; box-sizing: border-box; }
td, th { padding: 1px; vertical-align: inherit; }
thead, tbody, tfoot, tr { vertical-align: middle; }
caption { text-align: center; }
img { }
fieldset { border: 2px groove; padding: 0.35em 0.75em 0.625em; }
legend { padding: 0 2px; }
summary { display: block; }
details { display: block; }
)CSS";

} // namespace quill::engine
