# HAProxy edge backup

Sanitized backup of the live `leestrippai.com` edge proxy shape as of
2026-11-18. The supported edge is HAProxy; **nginx is unsafe/deprecated and
must not be restored**.

Files:

- `haproxy.cfg` — HAProxy listeners/backends for HiveMind web, worker
  WebSocket, and authenticated SearXNG.
- `leestrippai.com.conf` — certbot renewal configuration.
- `50-haproxy-leestrippai` — deploy hook that rebuilds the HAProxy PEM and
  reloads HAProxy after renewal.

Secrets are intentionally absent:

- The real SearXNG userlist hash is replaced with `<sha512-crypt-hash>`.
- The Let's Encrypt account id is replaced with `<letsencrypt-account-id>`.
- The PEM under `/etc/haproxy/certs/` is **not** backed up here because it
  contains the private key.

To refresh after an intentional live config change:

```sh
ssh root@leestrippai.com 'cat /etc/haproxy/haproxy.cfg' > haproxy/haproxy.cfg
ssh root@leestrippai.com 'cat /etc/letsencrypt/renewal/leestrippai.com.conf' > haproxy/leestrippai.com.conf
ssh root@leestrippai.com 'cat /etc/letsencrypt/renewal-hooks/deploy/50-haproxy-leestrippai' > haproxy/50-haproxy-leestrippai
# Restore the placeholders shown above; never commit live hashes or PEMs.
```

Validate the live config on the host with:

```sh
ssh root@leestrippai.com 'haproxy -c -f /etc/haproxy/haproxy.cfg && systemctl reload haproxy'
```
