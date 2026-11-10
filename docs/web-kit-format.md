# The UI kit

`rbgrid-ui_vX.Y.Z.zip` contains:

```
kit/                      the files the hub serves (static/, fonts/, index.html.gz, …)
ui-manifest.json          beside kit/, not inside it
```

`ui-manifest.json`:

```json
{
  "name": "rbgrid-ui",
  "version": "1.1.2+<commit>",
  "ui_version": "1.1.2",
  "api_version": 1,
  "built_at": "…",
  "files": [
    { "path": "index.html", "stored": "index.html.gz", "size": 1234, "sha256": "…",
      "content_type": "text/html; charset=utf-8", "encoding": "gzip" }
  ]
}
```

- `api_version` must match the hub firmware's API version, or the hub refuses the install.
- `stored` is the file's name in `kit/` and on the hub; `size` and `sha256` are of that stored file.
  The hub checks every uploaded file against them before it switches to the new UI.
- Text files are stored gzip-compressed; the hub serves `P.gz` for a request for `P`.
- The hub serves only `/index.html`, `/static/*`, `/fonts/*`, `/icons/*`, `/manifest.json`,
  `/ui-manifest.json`, `/favicon.*`, `/apple-icon.png`, `/robots.txt` — the kit keeps to these.

Built by `scripts/build-embedded.js`.
