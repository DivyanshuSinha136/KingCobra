# std.nn — neural networks

```kingcobra
import std.nn;
```

A native, dependency-free deep-learning library: tensors, layers (dense, convolution,
recurrent, attention, normalisation …), losses, optimizers, LR schedules, a batteries-included
`fit`, and versioned model files. No Python, no GPU, no model downloads.

It is **modular**: every layer implements one small interface (forward / backward / save),
containers (`sequential`, `residual`) nest freely, and the training engine only talks to that
interface. It is **verified**: every layer, activation and loss is checked against numerical
gradients (`nn.gradcheck`; the repo test `tests/nn_gradcheck.kc` runs ~60 architectures) and the
loader is fuzzed with corrupted files under AddressSanitizer.

```kingcobra
nn.seed(42);
let net = nn.mlp([2, 8, 1], "activation", "tanh", "out_activation", "sigmoid");
let h = nn.fit(net, "bce", "adam", X, Y, "epochs", 400, "batch_size", 4, "lr", 0.05);
print(nn.predict_class(net, [[0, 1], [1, 1]]));
```

## Conventions

* **Tensors** are n-dimensional (≤ 6) arrays of 64-bit floats, row-major. Wherever a function
  wants a tensor you may pass a tensor handle, a number, or a (rectangular) nested array.
  Shapes are batch-first: `[N, features]`, `[N, C, H, W]`, `[N, T, features]`.
* **Options** are trailing name/value pairs: `nn.dense(4, 8, "init", "he", "bias", false)`.
  An unknown option is an error that lists the valid ones. (A single struct whose fields are the
  options is accepted too.)
* **Results** with several fields are structs (`h.loss`, `r.max_rel_error`); ranked results are
  arrays of `[item, score]`.
* **Predictions** (`predict`, `predict_class`, `to_array`) return nested arrays;
  `forward` and the math functions return tensor handles. `print` shows a tensor's shape and values.
* **Memory.** The interpreter has no garbage collector (memory is explicit). `fit`, `evaluate`, `predict` and
  `train_step` allocate nothing per step. In a manual loop, call `nn.free(t)` on per-step tensors
  (`forward` outputs); models, losses and optimizers live until the program ends.
* **Randomness** (initialisers, dropout, shuffling, splits) comes from one generator;
  `nn.seed(n)` makes a run reproducible.
* **Training vs inference.** `fit` trains; `predict`/`evaluate` run in inference mode (dropout
  off, batch-norm uses running statistics). `nn.forward(m, x)` defaults to training mode — pass
  `false` as the third argument for inference.

## Tensors

| Function | Description |
|---|---|
| `tensor(data [, shape])` `zeros(shape)` `ones(shape)` `full(shape, v)` `rand(shape)` `randn(shape [, std])` `arange(n)` / `arange(a, b [, step])` `linspace(a, b, n)` `eye(n)` | creation |
| `shape(t)` `size(t)` `ndim(t)` `to_array(t)` `item(t)` `get(t, i, j …)` `set(t, v, i, j …)` `copy(t)` `free(t)` | inspection / access (negative indices allowed) |
| `reshape(t, shape)` (one `-1` allowed) `transpose(t [, axes])` `concat([t…], axis)` `slice(t, from, to)` `rows(t, indices)` | shaping (`slice`/`rows` work along axis 0) |
| `add sub mul div pow maximum minimum` | elementwise with NumPy-style **broadcasting** |
| `neg abs exp log sqrt square sin cos sign floor ceil round clip(t, lo, hi)` | elementwise |
| `apply(name, t [, alpha])` | any activation by name elementwise (`"tanh"`, `"gelu"`, `"softmax"` …) |
| `matmul(a, b)` | 2-D, vector forms, `[..,M,K]×[K,N]`, batched `[B,M,K]×[B,K,N]` |
| `sum mean max min var std prod argmax argmin (t [, axis [, keepdims]])` | reductions; no axis = everything |
| `softmax(t [, axis])` | tensor softmax. **`nn.softmax()` with no arguments is the layer.** |

### Data preparation

`one_hot(labels, classes)` · `standardize(x [, axis])` · `minmax(x [, axis])` ·
`shuffle(x [, y])` · `randint(low, high, shape)` · `train_test_split(x, y, test_fraction)` → `[x_tr, y_tr, x_te, y_te]` ·
`pad_sequences(seqs, maxlen [, value [, "post"|"pre"]])` (pairs well with `std.nlp`) ·
`window_series(data, window [, horizon])` → `[X[S,window,F], Y[S,horizon·F]]` for forecasting.

## Advanced tensor math

Everything below works on tensors (or nested arrays, which are converted), is vectorised in C, validates its
arguments, and reports mistakes as ordinary positioned errors (`nn.einsum: subscript 'j' has inconsistent sizes (3 vs 4)`).
Axis arguments accept negative values. `[axis]` defaults to the **last** axis where an axis is required (scans, sorting) and to
"everything" for reductions.

### Elementwise & special functions

| Function | Description |
|---|---|
| `erf erfc erfinv lgamma digamma` | error function, complement, **inverse** (full double precision), log-gamma, digamma (reflection + asymptotic series, poles → NaN) |
| `normal_cdf normal_ppf logit log_sigmoid` | Φ(x), its inverse (Φ⁻¹, accurate in the far tails), logit, `log σ(x)` computed without overflow |
| `log1p expm1 log2 log10 exp2 cbrt rsqrt reciprocal trunc frac` | accurate small-argument forms and friends |
| `tan asin acos atan sinh cosh asinh acosh atanh` | trigonometric / hyperbolic |
| `atan2 hypot mod floor_div logaddexp` | binary, broadcasting. `mod` follows the sign of the divisor (Python/NumPy) |
| `eq ne lt le gt ge logical_and logical_or logical_xor logical_not` | comparisons and logic → 0/1 tensors (broadcasting) |
| `isnan isinf isfinite nan_to_num(t [, nan, posinf, neginf])` | non-finite handling |
| `where(cond, a, b)` `masked_fill(t, mask, v)` `lerp(a, b, w)` `clamp(t, lo, hi)` | selection / blending; `clamp` takes tensor bounds, `null` skips a side |
| `equal(a, b)` `allclose(a, b [, rtol, atol])` | whole-tensor comparison → `bool` (equal infinities are close, NaN never is) |
| `activation_grad(name, t [, alpha])` | **derivative** d act/dx for any elementwise activation (`apply` is the forward); for hand-written backward passes |
| `softmax_grad(y, dy [, axis])` `log_softmax_axis(t [, axis])` | softmax backward (`y·(dy − Σ dy·y)`) and a stable log-softmax (`softmax(t, axis)` is the forward) |

### Reductions, scans, sorting

| Function | Description |
|---|---|
| `any all count_nonzero (t [, axis [, keepdims]])` | boolean reductions |
| `logsumexp nansum nanmean ptp (t [, axis [, keepdims]])` | stable log-sum-exp (handles ±inf), NaN-skipping sum/mean, max − min |
| `median(t [, axis [, keepdims]])` `quantile(t, q [, axis [, keepdims]])` | linear-interpolated quantiles, `q` in [0, 1] |
| `norm(t [, ord [, axis [, keepdims]]])` | vector p-norm: `ord` is a number, `"inf"`, `"-inf"` (default 2; 2-norm is overflow-safe) |
| `cumsum cumprod cummax cummin logcumsumexp (t [, axis [, reverse]])` | scans |
| `diff(t [, order [, axis]])` | finite differences |
| `sort(t [, axis [, descending]])` `argsort(...)` | **stable**; NaN always sorts last |
| `topk(t, k [, axis [, largest]])` | struct `{values, indices}` |

### Shape manipulation & indexing

| Function | Description |
|---|---|
| `squeeze(t [, axis])` `unsqueeze(t, axis)` `swapaxes(t, i, j)` `moveaxis(t, from, to)` `broadcast_to(t, shape)` | view-style reshaping (returns a new tensor) |
| `stack([t…] [, axis])` `split(t, sections_or_sizes [, axis])` | join / cut (`split` returns an array of tensors) |
| `slice_axis(t, axis, start, stop [, step])` | slicing along any axis; `null` bounds and negative indices allowed |
| `flip(t [, axes])` `roll(t, shift [, axis])` `tile(t, reps)` `repeat(t, n [, axis])` | rearranging (`roll`/`repeat` without an axis work on the flattened tensor) |
| `pad(t, pads [, mode [, value]])` | `pads` = int, `[before, after]`, or one pair per axis; `mode` = `"constant"` `"edge"` `"reflect"` `"wrap"` |
| `take(t, indices [, axis])` `gather(t, idx, axis)` `scatter(base, idx, src, axis)` `scatter_add(...)` | index-select / gather / scatter (negative indices OK, bounds checked) |
| `tril(t [, k])` `triu(t [, k])` `diag(t [, k])` `trace(t [, k])` | triangular parts (last two axes), diagonal build/extract |

### Products

| Function | Description |
|---|---|
| `outer(a, b)` `kron(a, b)` `cross(a, b)` | outer, Kronecker (any rank), 3-vector cross product (broadcasting) |
| `bmatmul(a, b)` | `[..,M,K] × [..,K,N]` with **batch broadcasting** (e.g. `[2,1,3,4] × [5,4,2] → [2,5,3,2]`) |
| `tensordot(a, b [, axes])` | `axes` = int (last *n* of `a` with first *n* of `b`) or `[axes_a, axes_b]`; runs on GEMM |
| `einsum(spec, t…)` | e.g. `"bij,bjk->bik"`, `"ii"` (trace), `"ij->ji"`, `"i,j->ij"`, implicit output (`"ij,jk"`). Up to 8 operands. Two-operand contractions use GEMM; anything else a generic loop nest. No `...` ellipsis |

### Statistics

| Function | Description |
|---|---|
| `cov(x [, ddof])` `corrcoef(x)` | `x` is `[observations, variables]` (default `ddof` 1) |
| `cdist(a, b [, p])` | pairwise L*p* distances `[N,D] × [M,D] → [N,M]` |
| `cosine_similarity(a, b [, axis [, eps]])` `normalize(t [, p [, axis [, eps]]])` | similarity along an axis, L*p* normalisation |
| `histogram(t, bins [, lo, hi])` `bincount(t [, minlength])` | counts (NaN ignored; `bincount` needs non-negative integers) |
| `convolve(a, v [, mode])` | 1-D, `"full"` `"same"` `"valid"` (NumPy semantics) |

### Linear algebra

Square-matrix routines accept batches (`[..., n, n]`); multi-output decompositions take one 2-D matrix and return a struct.

| Function | Description |
|---|---|
| `det(A)` `slogdet(A)` `inv(A)` `solve(A, B)` | LU with partial pivoting. `slogdet` → `{sign, logabsdet}`. `solve`: `B` is `[n]`, `[n,k]` (shared across a batch) or `[..,n,k]`. Singular → error |
| `lu(A)` | `{p, l, u}` with `A = p·l·u`, also for singular matrices |
| `cholesky(A)` `chol_solve(L, B)` | lower factor (uses the lower triangle); error if not positive definite |
| `qr(A)` | reduced Householder QR → `{q, r}` (tall or wide) |
| `eigh(A)` `eigvalsh(A)` | symmetric eigendecomposition (cyclic Jacobi) → `{values (ascending), vectors (columns)}` |
| `svd(A)` `svdvals(A)` | reduced SVD (one-sided Jacobi) → `{u, s (descending), vt}`; rank-deficient inputs get a full orthonormal `u` |
| `pinv(A [, rcond])` `lstsq(A, B [, rcond])` | Moore-Penrose pseudo-inverse; minimum-norm least squares → `{x, rank, s}` |
| `matrix_rank(A [, tol])` `cond(A [, p])` `matrix_norm(A [, ord])` | `ord`: `"fro"` (default) `"nuc"` `1` `-1` `2` `-2` `"inf"` `"-inf"` |
| `matrix_power(A, k)` `expm(A)` | integer powers (negative via inverse); matrix exponential (scaling & squaring) |

### Sampling

`uniform(shape [, lo, hi])` `normal(shape [, mean, std])` `trunc_normal(shape, mean, std, lo, hi)` (exact inverse-CDF, no rejection loop)
`bernoulli(shape, p)` `randperm(n)` `multinomial(weights, n [, replace])` — all driven by `nn.seed`.

```kingcobra
// PCA in four lines
let ev  = nn.eigh(nn.cov(points));           // eigenvalues ascending
let top = nn.slice_axis(ev.vectors, 1, 1, 3); // the two leading directions
let low = nn.matmul(points, top);

// attention scores for a batch, no loops
let w = nn.softmax(nn.einsum("bqd,bkd->bqk", Q, K), -1);
```

**Accuracy & limits.** All maths is float64. Jacobi eigen/SVD solvers give high relative accuracy and are unconditionally
convergent, but cost O(n³) per sweep — fine up to a few hundred rows/columns, not a LAPACK replacement for thousands.
Complex numbers / FFT, general (non-symmetric) eigenproblems and `einsum` ellipsis are not provided. The GEMM-backed products
skip exact-zero left operands, so `NaN`/`inf` in the other operand can be masked by a zero (same behaviour as `matmul`).

## Layers

All constructors return a layer handle. Weight initialisers: `"xavier"` (default, a.k.a. glorot),
`"xavier_normal"`, `"he"`/`"kaiming"`, `"he_uniform"`, `"lecun"`, `"lecun_uniform"`, `"normal"`,
`"uniform"`, `"zeros"`, `"ones"`, `"constant"`, `"orthogonal"` (option `"init"`, scale `"init_arg"`).

| Constructor | Input → output | Notes |
|---|---|---|
| `dense(in, out [, "bias", "init"])` | `[..., in]` → `[..., out]` | applies to the last axis, so it is time-distributed on sequences |
| `relu() relu6() leaky_relu([a]) elu([a]) selu() gelu() sigmoid() tanh() softplus() silu() mish() softsign() hard_sigmoid() softmax() log_softmax()` / `activation(name [, p])` | same shape | softmax acts on the last axis |
| `dropout(p)` | same | inverted dropout, active only in training |
| `batchnorm(features [, "momentum","eps","affine"])` | `[N,C,...]` | statistics per channel; running mean/var saved with the model |
| `layernorm(features [, "eps","affine"])` | `[..., F]` | |
| `flatten()` `reshape_layer(shape)` | | per-sample shapes (batch axis kept) |
| `embedding(vocab, dim [, "padding_idx"])` | ids `[N]`/`[N,T]` → `[N,(T,)dim]` | the padding row stays fixed (zero gradient) |
| `conv2d(in, out, k [, "stride","padding","dilation","bias","init"])` | `[N,C,H,W]` | `k`, `stride`, `dilation`, `padding` are ints or `[h, w]`; `"same"` padding |
| `conv1d(in, out, k [, …])` | `[N,C,L]` | |
| `maxpool2d(k [, "stride","padding"])` `avgpool2d(…)` `maxpool1d` `avgpool1d` `global_avg_pool()` | | `global_avg_pool`: `[N,C,…]` → `[N,C]` |
| `rnn(in, hidden [, "return_sequences"])` `lstm(…)` `gru(…)` | `[N,T,F]` → `[N,H]` or `[N,T,H]` | full backprop through time; orthogonal recurrent weights; LSTM forget bias 1 (`"forget_bias"`) |
| `attention(d_model, heads [, "causal"])` | `[N,T,D]` → `[N,T,D]` | multi-head self-attention |
| `pos_encoding()` | `[N,T,D]` | fixed sinusoidal position codes |

### Containers & builders

* `sequential([layers])` — the model type; `append(model, layer)`, `layers(model)`.
* `residual([layers])` — `y = x + f(x)` (shapes must match).
* `mlp(sizes [, "activation","out_activation","dropout","batchnorm","layernorm","init"])` — picks He/LeCun/Xavier init to match the activation.
* `transformer_block(d_model, heads, ff_dim [, "dropout","causal"])` — pre-norm block: `x + Attn(LN(x))`, then `x + FFN(LN(x))` with GELU.
* A layer can belong to one model only; use `clone` to duplicate a whole model.

## Losses

`nn.loss(name [, options])`, or pass the name as a string anywhere a loss is expected.
All reduce by mean.

| Name | Prediction / target | Options |
|---|---|---|
| `mse` `mae` `logcosh` | same shape | |
| `huber` | same shape | `"delta"` (1) |
| `bce` | probabilities / 0‥1 | |
| `bce_logits` | logits / 0‥1 (numerically stable) | `"pos_weight"` |
| `ce` | logits `[..,C]` / class indices **or** a distribution | `"smoothing"`, `"ignore_index"` — works for sequences `[N,T,C]` vs `[N,T]` |
| `nll` | log-probabilities / class indices | `"ignore_index"` |
| `kl` | log-probabilities / probabilities | |
| `hinge` | scores / ±1 | |

## Optimizers & schedules

`sgd adam adamw rmsprop adagrad (model [, lr] [, options])`

| Option | Applies to | Default |
|---|---|---|
| `"momentum"`, `"nesterov"` | sgd, rmsprop (momentum) | 0 / false |
| `"weight_decay"` (`"wd"`) | all (decoupled for adamw) | 0 (adamw 0.01) |
| `"beta1"`, `"beta2"`, `"eps"`, `"amsgrad"` | adam, adamw | 0.9, 0.999, 1e-8, false |
| `"alpha"`, `"centered"` | rmsprop | 0.99, false |
| `"decay_all"` | all | false — by default biases and norm parameters are *not* decayed |

`set_lr(opt, lr)`, `get_lr(opt)`, `clip_grad_norm(model, max)` (returns the pre-clip norm),
`clip_grad_value(model, v)`.

LR schedules (the `"schedule"` option of `fit`, or `nn.lr_at(spec, base, epoch, total [, warmup])`):
`const`, `step:E:G`, `exp:G`, `cosine[:min]`, `linear[:end]`, `poly:p[:min]`,
`plateau:patience:factor[:min]` (reduce on a stalled loss). `"warmup"` adds a linear ramp.

## Training

### `fit(model, loss, optimizer, X, Y, options…)` → `History`

`loss` and `optimizer` may be handles or names (`"ce"`, `"adam"`; with a name, optimizer options
such as `"lr"` go in the same list).

| Option | Meaning | Default |
|---|---|---|
| `"epochs"`, `"batch_size"` | | 10, 32 |
| `"shuffle"`, `"drop_last"` | | true, false |
| `"val_split"` / `"val", [Xv, Yv]` | held-out validation | none |
| `"metric"` | `accuracy`, `top_k:N`, `mae`, `mse`, `rmse`, `r2`, `none` | accuracy for ce/nll/bce/hinge, else none |
| `"schedule"`, `"warmup"` | LR schedule | const, 0 |
| `"clip_norm"`, `"clip_value"` | gradient clipping | off |
| `"accum"` | gradient accumulation steps | 1 |
| `"patience"`, `"min_delta"`, `"restore_best"` | early stopping (restores best weights by default when `patience` is set) | off |
| `"callback"` | `fn(epoch, loss, val_loss, metric) -> bool`; return `true` to stop | none |
| `"verbose"` | 0 silent, *n* = print every *n* epochs | 1 |
| `"val_every"`, `"seed"` | | 1, – |

`History`: `epochs`, `loss`, `val_loss`, `metric`, `val_metric`, `lr` (arrays; `val_*` hold `null`
when not evaluated), `best_epoch` (1-based), `best_loss`, `stopped_early`, `seconds`, `metric_name`.
Training that produces a NaN/∞ loss stops with an error naming the epoch and suggesting a fix.

### Other

* `evaluate(model, loss, X, Y [, "metric","batch_size"])` → `Evaluation {loss, metric, metric_name}`
* `predict(model, X [, "batch_size"])` → nested array · `predict_class(model, X [, "threshold"])` → class ids
  (argmax over the last axis; thresholded when the output has one unit)
* Manual loops: `forward(model, x [, training])` → tensor, `loss_value(loss, pred, target)` → float,
  `zero_grad(model)`, `backward(model, loss [, input_grad])`, `step(opt)`;
  or `train_step(model, loss, opt, x, y)` → loss in one call.
* Generation: `sample(logits [, temperature [, top_k]])` draws a class/token id from `softmax(logits / temperature)`, optionally restricted to the `top_k` largest logits.
* Metrics: `metric(name, pred, target)`, `confusion_matrix(pred, target, classes)`,
  `prf(pred, target, classes)` → `PRF {precision, recall, f1, support, macro_f1, accuracy}`.

## Model management

| Function | |
|---|---|
| `summary(model, input_shape)` | layer table with output shapes and parameter counts (shape is per-sample, without the batch axis) |
| `count_params(model [, trainable_only])` `param_names(model)` `get_params(model)` `set_params(model, list)` | |
| `freeze(model [, layer_index])` `unfreeze(…)` | transfer learning; frozen layers are skipped by every optimizer |
| `clone(model)` | independent deep copy |
| `copy_weights(dst, src [, tau])` | hard copy, or soft/Polyak update `dst = tau·src + (1−tau)·dst` (RL target networks) |
| `save(model, path)` `load(path)` | full model (architecture + weights + frozen flags) |
| `save_weights(model, path)` `load_weights(model, path)` | weights only, into an identical architecture; all-or-nothing |
| `gradcheck(model, loss, x, y [, "eps","samples"])` | `GradCheck {ok, max_rel_error, checked, worst}` — compares analytic and numerical gradients |

### File format

`KCNN` magic, format version, a layer tree (type, hyper-parameters, name, parameter tensors,
frozen flags), and a CRC-32 trailer. Little-endian, 64-bit floats, so a reloaded model reproduces
predictions bit-for-bit. Files are written atomically (temp file + rename). Loading never trusts a
size field: truncated, bit-flipped, or hand-edited files produce a clear error
(`file is truncated`, `checksum mismatch`, `parameter shape does not match the layer definition`, …).
Optimizer state is not stored.

## Recipes

**Image classifier**
```kingcobra
let cnn = nn.sequential([
    nn.conv2d(1, 16, 3, "padding", 1), nn.batchnorm(16), nn.relu(), nn.maxpool2d(2),
    nn.conv2d(16, 32, 3, "padding", 1), nn.relu(), nn.global_avg_pool(), nn.dense(32, 10)]);
nn.fit(cnn, "ce", "adam", images, labels, "epochs", 20, "val_split", 0.1, "patience", 4);
```

**Time-series forecasting**
```kingcobra
let w = nn.window_series(series, 24);           // 24-step windows -> next value
let m = nn.sequential([nn.lstm(1, 32), nn.dense(32, 1)]);
nn.fit(m, "mse", "adam", w[0], w[1], "epochs", 30, "clip_norm", 1.0);
```

**Text classifier** (with `std.nlp`): tokenise → ids → `pad_sequences` →
`embedding` → `gru` → `dense` → `fit(…, "ce", …)`.

**Language model**: `embedding`, `pos_encoding`, `transformer_block(…, "causal", true)` × N,
`layernorm`, `dense(d, vocab)`, trained with `"ce"` on shifted targets (see `examples/std_nn.kc`).

**DQN-style target network**: `let target = nn.clone(q)` … every few steps
`nn.copy_weights(target, q, 0.01)`.

## Examples

Runnable programs in `examples/nn/` (each finishes in a few seconds and is checked by the smoke tests):

| File | Shows |
|---|---|
| `std_nn.kc` (in `examples/`) | quick start: XOR, regression, summary, manual loop, save/load/clone, a tiny transformer |
| `01_regression.kc` | curve fitting with validation, cosine schedule, early stopping |
| `02_spirals.kc` | a non-linear classifier, confusion matrix, F1, text-drawn decision boundary |
| `03_cnn_patterns.kc` | a CNN with batch norm and pooling on noisy images |
| `04_forecast.kc` | LSTM time-series forecasting with `window_series` and recursive multi-step prediction |
| `05_text_sentiment.kc` | `std.nlp` preprocessing + embedding/GRU classifier, compared with `nlp.sentiment` |
| `06_char_language_model.kc` | a character-level transformer that generates text (`nn.sample`) |
| `07_dqn_gridworld.kc` | Deep Q-learning: replay buffer, target network, `clone` + soft `copy_weights` |
| `08_autoencoder_transfer.kc` | denoising autoencoder, then frozen-encoder transfer learning with 4 labels |
| `09_tensor_tour.kc` | the whole tensor API, broadcasting, reductions, data helpers, metrics |
| `10_verify_your_model.kc` | `gradcheck`, parameter names, LR schedules, gradient clipping, weight files |
| `11_math_toolkit.kc` | the advanced math API on real problems: PCA, low-rank SVD, least squares, einsum attention, stable log-probs, whitening, top-k sampling |

## Limits & notes

* Single-threaded CPU, float64. Measured training throughput on one core: ~6k samples/s for a 784-256-128-10 MLP, ~3.5k for a
  small 28×28 CNN (8/16 filters), ~1.3k for an LSTM (16→64, 30 steps), ~1k for a d=64 transformer block (T=32).
* Tensors have at most 6 dimensions; attention is full (O(T²)); no cross-attention, grouped
  convolution, or bidirectional RNN wrappers.
* `padding_idx` embedding rows receive no gradient by design; `gradcheck` skips them.
* Tensor and file handles are released with their own `free`/`close` calls (the core `free()` does not apply to them) — see *Memory* above.
