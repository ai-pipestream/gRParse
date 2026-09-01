# Chart derender evidence

Enrich-side measurements for the chart derender leg (`GRPARSE_ENRICH_TARGET`,
see README "Collector scatter-gather"): the same chart images sent through
grpc-enrich's `EnrichDocument` with `do_chart_extraction`, once per VLM
endpoint, three repeats each, scored against the fixtures' own data.

```
uv run --with grpcio --with grpcio-tools python eval/chart_derender/compare.py \
    --enrich <enrich host:port> --repeats 3
```

Inputs: `renders/{bar,line,pie}.png` painted by
`eval/scorecard/fixtures/xlsx_charts.py` from `chart_data.py` (the data the
`xlsx-charts` and `pptx-charts` fixtures and their truth files share;
`renders/truth.json` is written beside them) and the corpus raster
`tests/golden/corpus/bar_chart.png` (five bars, no axis, no numbers, no
title: only the bar ranking can be checked, against heights measured off the
pixels). Metrics per row: `cells` is the share of truth cells matched at the
same grid position after the best one-step row/column shift, `numeric` the
share of truth numbers within 2 percent or 0.5 of the value on the row whose
category label matches, `title` exact/contains/none against the source
title, `latency` median and range, `stable` whether the grid and the title
came back identical across the repeats. `compare.py` writes `report.md` and
`report.json` under `--out` (default `eval/chart_derender/out`, not
committed).

## 2026-09-01, parse-stack enrich, both endpoints, 3 repeats

Endpoints: `:8085` is Qwen2.5-VL (the stack's `ENRICH_VLM_URL` default),
`:8086` is North Micro Vision, both on `krick-1.taild24b1c.ts.net`.

| image | endpoint | answered | cells | numeric | title | latency | stable |
|---|---|---|---|---|---|---|---|
| bar.png (2 series, titled) | :8085 Qwen2.5-VL | 0/3 | 0.00 | 0.00 | none | 7001 ms (1020..11133) | n/a (no table) |
| bar.png (2 series, titled) | :8086 North Micro Vision | 3/3 | 0.73 | 0.50 | none | 503 ms (492..5346) | grid yes, title yes |
| line.png (1 series, titled) | :8085 Qwen2.5-VL | 0/3 | 0.00 | 0.00 | none | 1131 ms (826..1188) | n/a (no table) |
| line.png (1 series, titled) | :8086 North Micro Vision | 3/3 | 1.00 | 1.00 | none | 452 ms (448..525) | grid yes, title yes |
| pie.png (untitled) | :8085 Qwen2.5-VL | 0/3 | 0.00 | 0.00 | exact (both empty) | 738 ms (709..1054) | n/a (no table) |
| pie.png (untitled) | :8086 North Micro Vision | 3/3 | 0.80 | 1.00 | exact (both empty) | 356 ms (351..407) | grid yes, title yes |
| bar_chart.png (corpus) | :8085 Qwen2.5-VL | 0/3 | rows 0/3 | rank rho n/a | n/a | 1223 ms (1089..1769) | n/a (no table) |
| bar_chart.png (corpus) | :8086 North Micro Vision | 3/3 | rows 3/3 | rank rho 0.80 | n/a | 374 ms (369..565) | grid yes, title yes |

What the rows say:

- Qwen2.5-VL on `:8085` produced no table at all: the first call answered
  HTTP 503 `Loading model` (a cold server), every later call was skipped by
  the enrich service with `chart model returned ragged CSV rows`, i.e. the
  model's CSV does not survive enrich's `ChartCsvParser`. Whether the fix is
  the prompt, the parser or the model is an enrich-side question; through
  this wire the endpoint yields nothing today.
- North Micro Vision on `:8086` answered every call, byte-identical across
  the three repeats (grid and title), in under a second. It rounds to
  integers (`135.5` came back `135`, `97` as `100`, `143` as `140`, `88` as
  `90`; the line chart's `3.5, 4.1, 7.8, 11.2, 15.6, 18.9` came back
  `4, 4, 8, 11, 16, 19`), so the two-series bar chart scores 0.50 on
  numbers while the one-series line chart scores 1.00 within the tolerance.
  The pie's percentages came back as `45%, 30%, 15%, 10%` under the model's
  own headers `Category,Percentage` (the source says `Segment,Share`), which
  is the 0.80 on cells. Its corpus reading `A 15, B 25, C 20, D 30, E 25`
  ranks the five bars 0.80 against the pixel heights (bars 2 and 5 tied).
- Neither endpoint returned a chart title through `ChartTable.title`, so
  the titled charts score `none`; the stability rule that treats a
  derendered title as descriptive was not exercised by these runs (nothing
  varied between repeats at all).
- `ItemAnnotation.model` came back empty from both legs; the scorecard's
  `derender.model` field will read empty until enrich fills it.

A second Qwen-only pass (3 repeats, same enrich, warm server) repeated the
outcome for the three renders (9 of 9 skipped with `ragged CSV rows`, 0.7 to
1.4 s each) and answered once out of three for the corpus raster, and that
one "table" was a 1x4 split of a chat reply: "Sure, I can help you with
that. However, I need the chart or the data you want to convert into a CSV
format. Could you please provide the data or the chart image?" The model
answered as if no image had reached it, so the `:8085` failure is a wiring
question (the image part of enrich's OpenAI-style request is not landing on
that server, or that server ignores it) before it is a chart-reading one.
That leg is also the one unstable result in the set.

## The leg itself, live (2026-09-01, private gRParse build, `GRPARSE_PICTURE_IMAGES=on`)

With `GRPARSE_ENRICH_TARGET=enrich:50056` and
`GRPARSE_ENRICH_VLM_ENDPOINT=http://krick-1.taild24b1c.ts.net:8086`, converting
`bar_chart.png` twice folded a table onto `#/pictures/0` both times
(`gRParse data: chart #/pictures/0 derendered by  (6x1)`, a
`GenerationSource` with the endpoint, `charts_derendered` +1 per run) and
showed two enrich-side gaps at once: the model name is empty (so
`created_by` stays unset and the data log names nobody), and the CSV parser
handed back one column of pipe-joined text (`Label A|Value A`,
`Data Point 1|10`, ...) because the model answered this crop with a
pipe-separated table rather than CSV. The two runs also read different
numbers for the last two bars (`25, 30` then `12, 18`), which the scorecard
reports as unstable by design: derendered cells stay in the fingerprint,
only the title is descriptive. Against the enrich default endpoint
(`:8085`, no `GRPARSE_ENRICH_VLM_ENDPOINT`) the same two conversions folded
nothing: each carried one warning under `collector_warnings:grparse-cv`
(`chart derender: #/pictures/0 skipped (SKIP_REASON_VLM_ERROR: chart model
returned ragged CSV rows)`), `chart_derender_skipped` +1 per run, and the
document was otherwise identical and stable across the two runs.

No default is changed here. The evidence favors `:8086` for the derender
leg as wired (answers, stable, fast, integers only) and shows `:8085`
cannot be used through enrich's chart path until its CSV parses; the
decision and the enrich-side fix are the coordinator's.

Determinism note: `EnrichOptions` exposes no decoding controls (no
temperature, seed or greedy flag; enrich's VLM client sends only
`max_tokens`), so gRParse cannot ask for greedy decoding on the wire. A
follow-on on grpc-enrich.
