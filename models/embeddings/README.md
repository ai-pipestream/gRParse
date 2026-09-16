# Local embedding model package

This package pins
[`sentence-transformers/all-MiniLM-L6-v2`](https://huggingface.co/sentence-transformers/all-MiniLM-L6-v2/tree/826711e54e001c83835913827a843d8dd0a1def9)
at Hugging Face revision `826711e54e001c83835913827a843d8dd0a1def9`.
The model is Apache-2.0, fp32, 384 dimensions, and takes plain text without
query or document prefixes. The manifest URLs contain the full revision and
never resolve through `main`.

The current CPU ONNX Runtime, TensorRT, and native Intel OpenVINO Runtime paths
all load the same ONNX graph and share the tokenizer and pooling code. Fetching
the ONNX package is therefore sufficient for all three:

```bash
scripts/fetch-embedding-model.sh --backend onnx
```

The fetcher requires Bash, curl, and coreutils; no Python or model export is
needed. It checks byte size and SHA256 before atomically installing each file.
Failed downloads leave existing target files intact and remove temporary files.

The upstream OpenVINO IR pair is retained for experimentation. Fetch it alone,
or fetch both graph formats (about 181 MB plus the tokenizer), with:

```bash
scripts/fetch-embedding-model.sh --backend openvino
scripts/fetch-embedding-model.sh
```

The default destination is `models/embeddings/all-MiniLM-L6-v2/`. Model bytes
remain untracked. Verify an already populated directory without network access
or filesystem changes:

```bash
scripts/fetch-embedding-model.sh --verify
scripts/fetch-embedding-model.sh --verify --backend onnx --dir /srv/models/embeddings/all-MiniLM-L6-v2
```

## Compose stack

The opt-in `compose.stack.embeddings.yaml` overlay mounts the downloaded model
directory read-only at `/embedding-models` and sets
`GRPARSE_EMBEDDING_MODEL_DIR` to that path. Download only the tokenizer and
ONNX graph needed by every current backend, then verify them offline:

```bash
scripts/fetch-embedding-model.sh --backend onnx
scripts/fetch-embedding-model.sh --verify --backend onnx
```

Run embeddings with the CPU image and CPU backend:

```bash
docker compose -f compose.stack.yaml -f compose.stack.cpu.yaml \
  -f compose.stack.embeddings.yaml up
```

Run the OpenVINO image and select its native Intel backend explicitly:

```bash
GRPARSE_EMBEDDING_BACKEND=openvino docker compose \
  -f compose.stack.yaml -f compose.stack.openvino.yaml \
  -f compose.stack.embeddings.yaml up
```

Run the base CUDA image and select TensorRT explicitly:

```bash
GRPARSE_EMBEDDING_BACKEND=tensorrt docker compose \
  -f compose.stack.yaml -f compose.stack.embeddings.yaml up
```

The embeddings overlay defaults to `cpu` only when
`GRPARSE_EMBEDDING_BACKEND` is unset. An explicitly selected backend must be
compiled into the selected image and available at runtime; startup fails
instead of falling back to another backend. The runtime rejects any text whose
manually assembled sequence exceeds 256 tokens, including `[CLS]` and `[SEP]`.

## Runtime contract

The ONNX graph is opset 14. The ONNX and OpenVINO graphs have the same
interface:

| Name | Type | Shape |
|---|---|---|
| `input_ids` | int64 | `[batch, sequence]` |
| `attention_mask` | int64 | `[batch, sequence]` |
| `token_type_ids` | int64 | `[batch, sequence]` |
| `last_hidden_state` | fp32 | `[batch, sequence, 384]` |

`last_hidden_state` contains unpooled contextual token embeddings. The current
implementation loads `model.onnx` through ONNX Runtime CPU, TensorRT, or direct
OpenVINO Runtime. The XML/bin pair is an optional equivalent graph for future
experimentation. The application must produce the sentence embedding as
follows, independently for each batch item:

1. Remove the tokenizer's stored truncation and padding configuration. Tokenize
   the content without automatic special-token post-processing, then manually
   add `[CLS]` and `[SEP]`. Reject the input if the resulting sequence exceeds
   256 tokens, including those two special tokens; never silently truncate it.
   Pad on the right only when batching and supply zero `token_type_ids` for a
   single sequence.
2. Expand the 0/1 attention mask across the 384 hidden columns, multiply it by
   `last_hidden_state`, sum over the sequence axis, and divide by the mask sum.
   Clamp the divisor to at least `1e-9`.
3. L2-normalize the resulting 384-element fp32 vector. No prefix is added.

In notation, with token embedding `x[i]` and attention mask `m[i]`:

```text
mean = sum_i(x[i] * m[i]) / max(sum_i(m[i]), 1e-9)
embedding = mean / max(sqrt(sum_j(mean[j]^2)), 1e-12)
```

The published `tokenizer.json` records fixed padding/truncation at 128 from an
older export. The runtime removes both settings and enforces the package's
256-token limit by rejecting oversize input. The encoder itself has 512 position
embeddings, but 256 is the sentence-transformers model contract. Padding tokens
must not contribute to pooling; manually added `[CLS]` and `[SEP]` do contribute
because their attention-mask values are one.

The tokenizer is uncased WordPiece (`vocab_size=30522`), with `[PAD]=0`,
`[UNK]=100`, `[CLS]=101`, `[SEP]=102`, and `[MASK]=103`. Both graphs are dynamic
in batch and sequence length and expose only `last_hidden_state`; neither graph
contains pooling or normalization.

## Pinned artifacts

| Local file | Upstream file | Bytes | SHA256 |
|---|---|---:|---|
| `tokenizer.json` | `tokenizer.json` | 466,247 | `be50c3628f2bf5bb5e3a7f17b1f74611b2561a3a27eeab05e5aa30f411572037` |
| `model.onnx` | `onnx/model.onnx` | 90,405,214 | `6fd5d72fe4589f189f8ebc006442dbb529bb7ce38f8082112682524616046452` |
| `openvino_model.xml` | `openvino/openvino_model.xml` | 211,315 | `f87dd1482b2a745f8c699b81ddd9cbcad666a193be4693abcea44b7ac8c67c1e` |
| `openvino_model.bin` | `openvino/openvino_model.bin` | 90,265,744 | `8b86cab4722e2aefab310cf96d4d5a9eb3b187f7d9670a082afc55c7fa0d392a` |

The hashes above were computed from the downloaded bytes at the pinned
revision. The ONNX graph was inspected directly for opset, names, types, and
shapes; the OpenVINO XML was inspected directly for the matching interface.
