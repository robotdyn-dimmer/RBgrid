# Installing the kit on a hub

The release notes carry the canonical steps. In short, with the hub's admin token:

```bash
HUB=http://<hub>
TOKEN=<admin-token>
cd kit
for f in $(find . -type f | sed 's|^\./||'); do
  curl -sf -X POST "$HUB/api/system/ui-upload?path=$f" \
       -H "Authorization: Bearer $TOKEN" \
       -H 'Content-Type: application/octet-stream' --data-binary "@$f" || { echo "upload failed: $f"; exit 1; }
done
curl -sf -X POST "$HUB/api/system/ui-commit" \
     -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
     --data-binary @../ui-manifest.json
```

The hub stages the files, checks each against the manifest, and switches to the new UI at once;
a failed commit leaves the running UI untouched. Reload `http://<hub>/`.

From a checkout of this folder, `npm run ui:upload -- http://<hub>` does the same
(`RBGRID_TOKEN=<admin-token>` in the environment), using the kit in `build-embedded/`.
