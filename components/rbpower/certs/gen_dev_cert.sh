#!/usr/bin/env bash
# Regenerate the self-signed DEV TLS cert+key embedded by rbpower (Step 3b HTTPS server).
#
# The PRIVATE KEY is intentionally NOT committed (no secrets in the repo). Run this once before
# building if rbgrid_dev_key.pem is missing. The cert is dev-only (LAN, curl -k / browser exception);
# PRODUCTION uses a real Cloud-DNS cert installed to littlefs /https_cert.pem + /https_key.pem, which
# the firmware auto-prefers over this embedded dev cert.
set -euo pipefail
cd "$(dirname "$0")"
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout rbgrid_dev_key.pem \
  -out rbgrid_dev_cert.pem \
  -days 3650 \
  -subj "/C=US/O=RBgrid/CN=rbgrid.local" \
  -addext "subjectAltName=DNS:rbgrid.local,DNS:*.local,IP:192.168.4.1"
echo "Regenerated rbgrid_dev_{cert,key}.pem (self-signed, valid 10y)."
