#!/usr/bin/env node
// ソースの文字整形。マーカー //[ の内側だけに「}x{ の連結」をかける。
//
//   chained-style.mjs <file> ...       整形して書き戻す (変わったファイル名を出す)
//
// 規則
//  - //[ と //] の内側を「中」、外側を「外」として、外→中→外→… と再帰的にたどる。
//  - 中: 直前の } から、{ も } も挟まずに次の { までを 1 セグメントと見て、
//        その間の改行を空白に畳む。セグメント内の , ; : はそのまま残す。
//        () [] の中に入った { も同じ扱いにする。
//  - 中: 次行以上に分かれた {x{ は畳まない (}x{ を定義した処理だけが畳む)。
//  - 外: 何もしない。
//
// カッコ対応は () {} [] の 3 種だけ。<> は数値の比較と区別が付かないので数えない。
// 文字列・文字リテラル・行コメント・ブロックコメントの中は触らない。

const MARKERS = new Set(["//[", "//]"]);
const OPEN = new Set(["(", "{", "["]);
const CLOSE = new Set([")", "}", "]"]);
const PAIR = { "(": ")", "{": "}", "[": "]" };

// 中身も字下げも無い行は落とす。
export function chainBody(input) {
  const source = input.replace(/^[ \t]+$/gm, "");
  const n = source.length;
  const out = [];
  let i = 0;
  let didCut = false; // 直前のセグメントを行コメントで切ったか

  // 行頭 (直前が改行 + 字下げ) の } だけを連結の起点にする。
  const startsLine = (s, at) => {
    let k = at - 1;
    while (k >= 0 && (s[k] === " " || s[k] === "\t")) k--;
    return k < 0 || s[k] === "\n";
  };

  const readLine = (s, at) => {
    let e = at;
    while (e < s.length && s[e] !== "\n") e++;
    return { text: s.slice(at, e), end: e };
  };
  const readBlock = (s, at) => {
    let e = at + 2;
    while (e < s.length && !(s[e - 1] === "*" && s[e] === "/")) e++;
    return { text: s.slice(at, Math.min(e + 1, s.length)), end: Math.min(e + 1, s.length) };
  };
  const readQuoted = (s, at) => {
    const q = s[at];
    let e = at + 1;
    while (e < s.length) {
      const d = s[e++];
      if (d === "\\" && e < s.length) { e++; continue; }
      if (d === q) break;
      if (q !== "`" && d === "\n") break;
    }
    return { text: s.slice(at, e), end: e };
  };

  while (i < n) {
    const c = source[i];

    if (c === "/" && source[i + 1] === "/") { const r = readLine(source, i); out.push(r.text); i = r.end; continue; }
    if (c === "/" && source[i + 1] === "*") { const r = readBlock(source, i); out.push(r.text); i = r.end; continue; }
    if (c === '"' || c === "'" || c === "`") { const r = readQuoted(source, i); out.push(r.text); i = r.end; continue; }

    if (c === "}" && startsLine(source, i)) {
      out.push("}"); // 起点の } はここで 1 回だけ出す
      i++;
      // } の直後、対応の取れていない { か } に当たるまでを 1 セグメントとして畳む。
      // 直前に切ったセグメントがあれば、その次の } では畳まず、素直に写す。
      if (didCut) {
        didCut = false;
        continue;
      }
      const stack = [];
      let seg = "";
      let end = ""; // セグメントを閉じた { か }
      while (i < n) {
        const d = source[i];
        // 行コメントが来たら、そこでセグメントは切れる (後ろを畳むとコメントに飲まれる)。
        if (d === "/" && source[i + 1] === "/") {
          const r = readLine(source, i);
          out.push(tidySegment(seg + r.text));
          didCut = true; // 受け取った改行を、そのまま 1 つ置き直す
          i = r.end;
          break;
        }
        if (d === "/" && source[i + 1] === "*") { const r = readBlock(source, i); seg += r.text; i = r.end; continue; }
        if (d === '"' || d === "'" || d === "`") { const r = readQuoted(source, i); seg += r.text; i = r.end; continue; }
        if (OPEN.has(d)) {
          // 対応の取れていない { はセグメントの終わり。写さずに親へ返す。
          if (d === "{" && stack.length === 0) { end = d; break; }
          stack.push(d); seg += d; i++;
          continue;
        }
        if (CLOSE.has(d)) {
          if (stack.length && PAIR[stack[stack.length - 1]] === d) { stack.pop(); seg += d; i++; continue; }
          // 対応の取れていない } もセグメントの終わり (行頭なら新しい起点になる)。
          if (d === "}" && stack.length === 0) { end = d; break; }
          seg += d; i++;
          continue;
        }
        seg += d === "\n" ? " " : d;
        i++;
      }
      out.push(tidySegment(seg));
      // 閉じた } は次の回で起点として扱う (ここでは写さない)。
      if (end === "}") continue;
      if (end === "{") { out.push("{"); i++; }
      continue;
    }

    out.push(c);
    i++;
  }

  return out.join("");
}

// セグメント内の空白を詰める (改行は空白 1 つ)。
const tidySegment = (seg) =>
  seg
    .replace(/[ \t]*\n[ \t]*/g, " ") // 改行は空白 1 つに
    .replace(/[ \t]+/g, " ")
    .replace(/^[ \t]+/, "")
    // 連結で詰まった直後の { } だけ 1 つ空ける。元から 1 行だった {はそのまま。
    .replace(/([^\s{])\{/g, "$1 {")
    .replace(/\}([^\s}])/g, "} $1");

// マーカーで 外→中→外→… と再帰的にたどる。
export function formatSource(source) {
  const lines = source.split("\n");
  const out = [];
  let buf = [];
  let inside = false;
  let afterOpen = false;
  for (const line of lines) {
    const t = line.trim();
    if (MARKERS.has(t)) {
      const body = buf.join("\n");
      out.push(inside ? chainBody(body) : inside ? "" : body);
      out.push(t);
      if (t === "//[") afterOpen = true;
      inside = !inside;
      buf = [];
      continue;
    }
    if (afterOpen && t === "") continue;
    afterOpen = false;
    buf.push(line);
  }
  out.push(inside ? chainBody(buf.join("\n")) : buf.join("\n"));
  return out.join("\n");
}

// ファイルを渡されたときだけ書き換える。import されたときは何もしない
// (名前で判定しない。呼び出し元のパスはどこに置かれるか分からない)。
const files = process.argv.slice(2);
if (files.length > 0) {
  const fs = await import("node:fs");
  for (const f of files) {
    const before = fs.readFileSync(f, "utf8");
    const after = formatSource(before);
    if (after !== before) { fs.writeFileSync(f, after); console.log("formatted:", f); }
  }
}