# std.nlp — natural-language processing

```kingcobra
import std.nlp;
```

A native, dependency-free NLP toolkit: no model files, no Python, no network.
Everything is plain C inside the interpreter, so it is fast enough for
corpora of hundreds of thousands of tokens.

**Language coverage.** Text is UTF-8 throughout. Word segmentation, case
mapping and accent stripping understand Latin (Latin-1, Extended-A, Extended
Additional), Greek and Cyrillic; Han/Kana text is segmented one character per
token. The stemmer, English stopwords, sentiment lexicon and readability
formulas are **English**. Language detection covers 7 Latin-script languages
plus script-level detection for the rest. Invalid UTF-8 never crashes or gets
silently rewritten: undecodable bytes pass through verbatim, and overlong /
surrogate encodings are rejected rather than decoded.

**Token lists.** Anywhere a function takes "tokens" you can pass an array of
strings *or* a plain string (auto-tokenized, lowercased). Functions that take
"docs" want an array of documents, each a token array or a string.

**Results.** Multi-field results are structs (`r.compound`, `s.words`); ranked
results are arrays of `[item, score]` pairs, best first.

## Cleanup

| Function | Description |
|---|---|
| `lower(s)` `upper(s)` `title(s)` `capitalize(s)` | Unicode-aware case mapping (`upper("straße")` → `STRASSE`) |
| `strip_accents(s)` | `Crème Łódź` → `Creme Lodz` (Latin-1/Ext-A, combining marks) |
| `strip_punct(s [, "k"])` | punctuation → space; `"k"` keeps inner apostrophes/hyphens |
| `strip_html(s)` | drops tags, comments, `<script>`/`<style>` bodies; decodes entities |
| `strip_emoji(s)` | removes emoji, variation selectors, ZWJ, flags |
| `collapse_ws(s)` | collapses Unicode whitespace runs, trims |
| `normalize(s)` | lowercase, accent-free, curly quotes/dashes → ASCII, fullwidth → ASCII |
| `slugify(s [, sep])` | `Hello, Crème!` → `hello-creme` (non-Latin letters kept) |
| `expand_contractions(s)` | `can't` → `cannot`, `they're` → `they are`, case preserved |

## Segmentation

| Function | Description |
|---|---|
| `tokenize(s [, flags])` | words & numbers. Flags: `l` lowercase, `p` keep punctuation tokens, `d` drop numbers, `h` split on hyphens, `c` split clitics (`don't` → `do n't`, `can't` → `can n't`) |
| `count_words(s)` | number of tokens |
| `sentences(s)` | abbreviation-aware splitter (`Dr.`, `U.S.`, `3 p.m.`, decimals, ellipses, blank lines, `。！？`) |
| `ngrams(tokens, n [, sep])` | word n-grams joined by `sep` (default space) |
| `char_ngrams(s, n [, "p"])` | character n-grams; `"p"` pads with `_` |
| `chunk(text, max_chars [, overlap])` | sentence-aware chunks ≤ `max_chars` with optional overlap — for RAG / LLM pipelines |

## Linguistics

`stem(w)`, `stem_all(tokens)` (Porter 1980) · `stopwords([lang])`,
`is_stopword(w [, lang])`, `remove_stopwords(tokens [, lang])` (en is a full
list; es fr de it pt nl are short function-word lists) · `syllables(w)` ·
`readability(s)` → `{flesch, grade, fog, words, sentences, syllables}` ·
`soundex(w)`.

## Similarity & fuzzy matching

| Function | Returns |
|---|---|
| `levenshtein(a,b)` `damerau(a,b)` `hamming(a,b)` `lcs(a,b)` | integer distances / LCS length (code-point based) |
| `jaro(a,b)` `jaro_winkler(a,b [,scale])` `ratio(a,b)` `dice(a,b)` | similarity 0‥1 |
| `jaccard(a,b)` | token-set overlap |
| `cosine(a,b)` | token-count cosine for strings/arrays of strings, or vector cosine for two numeric arrays |
| `fuzzy(q, candidates [, n [, min_score]])` | `[[candidate, score], …]` |
| `closest(q, candidates)` | best candidate or `null` |
| `simhash(text)` / `hamming_bits(a,b)` | 64-bit fingerprint / bit distance — near-duplicate detection |

## Statistics

`freq(tokens [, top])` · `vocab(docs [, min_count [, max_size]])` ·
`bow(tokens, vocab)` · `collocations(tokens [, n [, min_count]])` (PMI) ·
`stats(text)` → `{chars, words, unique_words, sentences, paragraphs,
avg_word_len, avg_sentence_len, lexical_diversity}`.

## Models (handles)

**TF-IDF** — `tfidf_fit(docs [, min_df [, max_features [, "s"]]])`,
`tfidf_vector(m, text)` (dense, L2-normalised, smooth idf — feeds `std.vector`
or `cosine`), `tfidf_sparse(m, text)` → `[[col, weight], …]`,
`tfidf_top(m, text [, n])` → `[[term, weight], …]`, `tfidf_vocab(m)`,
`tfidf_size(m)`. `"s"` = sublinear tf.

**BM25 ranked search** — `bm25_new(docs [, k1 [, b]])`,
`bm25_search(m, query [, k])` → `[[doc_index, score], …]`,
`bm25_score(m, query, doc_index)`, `bm25_size(m)`.

**Naive Bayes classifier** — `nb_new([alpha])`, `nb_train(m, text, label)`,
`nb_train_many(m, docs, labels)`, `nb_predict(m, text)`,
`nb_scores(m, text)` → `[[label, probability], …]`, `nb_labels(m)`.

**Spell correction** — `spell_new([corpus])`, `spell_add(m, corpus)`,
`spell_known(m, w)`, `spell_correct(m, w [, max_dist])`,
`spell_suggest(m, w [, n [, max_dist]])` → `[[word, distance, freq], …]`.
Ranking: smallest edit distance (Damerau), then corpus frequency. Capitalisation
of the input is carried over to the answer.

```kingcobra
let idx = nlp.bm25_new(docs);
for (hit in nlp.bm25_search(idx, "markets fell", 3)) { print(docs[hit[0]], hit[1]); }
```

## Analysis

`sentiment(s)` → `{compound, pos, neu, neg, label}` — VADER-style lexicon
scoring with negation, intensifiers/dampeners, ALL-CAPS emphasis, `!`/`?`
emphasis and "but" re-weighting (English; ~350-word core lexicon, so it is a
fast baseline, not a replacement for a trained model). `sentiment_label(s
[, threshold])`.
`detect_lang(s)` → `{lang, confidence, script}` (`"und"` if undecidable) and
`detect_script(s)`. `keywords(s [, n [, max_words]])` — RAKE, returns
`[[phrase, score], …]`. `summarize(s [, n])` — extractive, original order.
`top_sentences(s [, n])` → `[[sentence, score], …]`.

## Extraction

`emails(s)` · `urls(s)` · `hashtags(s)` · `mentions(s)` · `numbers(s)`
(`1,234.50` → `1234.5`, `-42` → `-42`; integers stay integers).

## Limits & notes

* Models are handles; they live for the life of the program.
* Quadratic string distances (`levenshtein`, `lcs`, …) are O(len(a)·len(b)).
  `spell_correct` scans the vocabulary, pruned by length; fine for typical
  dictionaries (≤ ~100k words).
* Not implemented (by design): lemmatisation, POS tagging, NER, and
  non-English stemmers — those need data files this module deliberately avoids.
