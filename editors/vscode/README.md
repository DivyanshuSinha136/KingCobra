# KingCobra syntax highlighting

GitHub highlights code with [Linguist](https://github.com/github-linguist/linguist), which uses
**TextMate grammars**. This package contains one, plus everything needed to use it.

```
grammars/kingcobra.tmLanguage.json   the grammar (scope: source.kingcobra)
languages.yml.snippet                entry for Linguist's lib/linguist/languages.yml
samples/*.kc                         sample files Linguist requires for a new language
vscode/                              ready-to-install VS Code extension (same grammar)
```

## 1. Highlight `.kc` on GitHub right now (interim, 1 line)

Linguist can't know `.kc` until it is merged upstream, but you can map it to a similar language
for *your* repo. Add to `.gitattributes`:

```
*.kc linguist-language=Rust
```

(Rust is the closest built-in: `let`, `fn`, `->`, `struct`, `impl`, `match`, `=>`.)
This also affects the language bar of your repository. Fenced code blocks in Markdown can use ```` ```rust ````
the same way.

## 2. Get real "KingCobra" highlighting on GitHub (Linguist PR)

1. Fork `github-linguist/linguist` and clone it.
2. Copy `grammars/kingcobra.tmLanguage.json` to `vendor/grammars/` **or** (preferred) add it as a git
   submodule from *your* repo and register it:
   `script/add-grammar https://github.com/<you>/kingcobra`
   (the grammar must be in a public repo with a license; KingCobra's LICENSE applies).
3. Add the `languages.yml.snippet` entry to `lib/linguist/languages.yml` (alphabetical order).
4. Put the sample files in `samples/KingCobra/` (use `language_tour.kc` and `b_style.kc`).
5. Run `bundle install && bundle exec rake samples` and `script/update-ids`, then `bundle exec rake test`.
6. Open the PR. Linguist's maintainers require that the extension is in real use: **at least 200
   distinct repositories** on GitHub using `.kc` (a search link for this goes in the PR description),
   and no more than a small share from a single owner. Until KingCobra reaches that, use option 1.

Syntax-theme colors on GitHub come from the scope names in the grammar, so no theme work is needed.

## 3. VS Code

Copy the `vscode/` folder to `~/.vscode/extensions/kingcobra-0.1.0/` and restart VS Code, or package it
with `npx @vscode/vsce package` and install the `.vsix`. Also works for `github.dev` / Codespaces.

## What is highlighted

Everything in `docs/GRAMMAR.md` (v0.8): keywords and the contextual ones (`type`, `enum`, `impl`, `const`,
`try`, `throw`, `defer`, `packed`, B-style `auto`/`extrn`, C-style `switch`/`case`/`goto`/`typedef`), types
(`int`, `u8`, `f64`, `*Node`, PascalCase), `fn` / `struct` / `enum` / `impl` names, `import std.X as Y`,
`std.*` module paths, builtins, f-strings with nested `{ expr }` and `{{ }}` escapes, char literals,
`0x`/`0b`/`0o`/`_`-separated numbers, floats, `//` and `/* */` comments, loop labels, `...rest`, `..`, `??`,
`=>`, `->`, `>>>`, and `#define`/`#if` lines.
