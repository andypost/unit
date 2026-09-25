# ACME certificates with an external client

FreeUnit does not talk to a certificate authority itself. An external ACME
client, such as certbot or lego, gets and renews the certificate, and a
deploy hook uploads the new bundle through the control API. Rotation
needs no restart: `PUT /certificates/<name>` on an existing name replaces
the bundle in place, and a listener that names it is applied again.
Connections already accepted finish with the old certificate; new
handshakes get the new one.

This page is the operator-facing recipe.

## What the control API does

- `PUT /certificates/<name>` creates the bundle, or replaces it when the
  name exists. The body is PEM: the server certificate first, then its
  chain, then the private key.
- The bundle is written to a temporary file in the certificate store and
  renamed over the old one, so a crash never leaves a half-written
  certificate.
- The key must belong to the first certificate. A mismatched bundle is
  refused with `400 Invalid certificate.`, and the old bundle stays in use.
- Names that start with `.` are reserved and refused with
  `400 Invalid certificate name.`.
- When the current configuration names the bundle, the answer is
  `200 Certificate chain updated.` after the router has loaded it.
  Otherwise it is `200 Certificate chain uploaded.`.
- `500 Certificate stored but not applied.` means the bundle is on disk,
  but the router refused the configuration with it, for example because
  a `conf_commands` option does not accept the new key type. The router
  keeps serving with the old certificate. Upload a working bundle again.

## HTTP-01 route on port 80

The client answers the HTTP-01 challenge from a directory that FreeUnit
serves with `share`. The listener must pass to routes, and the router,
which runs as the `--user`, must be able to read the token files and
traverse every directory above them.

```json
{
  "listeners": {
    "*:80":  { "pass": "routes/http" },
    "*:443": { "pass": "routes/app", "tls": { "certificate": "example.org" } }
  },
  "routes": {
    "http": [
      {
        "match": { "uri": "/.well-known/acme-challenge/*" },
        "action": {
          "share": "/var/lib/unit-acme$uri",
          "chroot": "/var/lib/unit-acme/.well-known/acme-challenge/",
          "fallback": { "return": 404 }
        }
      },
      { "action": { "return": 301, "location": "https://$host$request_uri" } }
    ],
    "app": [ { "action": { "pass": "applications/app" } } ]
  }
}
```

Leave out the `*:443` listener until the first bundle exists: the
configuration is refused when a listener names a missing certificate.

## The deploy hook

The hook uploads the renewed bundle under the same name every time. It
needs only `curl` and access to the control socket.

```sh
#!/bin/sh
# certbot deploy hook; also usable from lego --run-hook / --renew-hook.
#   certbot: RENEWED_LINEAGE=/etc/letsencrypt/live/example.org
#   lego   : CERT_FULLCHAIN, CERT_KEY and UNIT_CERT_NAME are set below
set -eu

SOCK=${UNIT_CONTROL:-/var/run/unit/control.sock}
FULLCHAIN=${CERT_FULLCHAIN:-${RENEWED_LINEAGE:-}/fullchain.pem}
KEY=${CERT_KEY:-${RENEWED_LINEAGE:-}/privkey.pem}
NAME=${UNIT_CERT_NAME:-$(basename "${RENEWED_LINEAGE:?set RENEWED_LINEAGE or UNIT_CERT_NAME}")}

cat "$FULLCHAIN" "$KEY" \
    | curl -fsS --unix-socket "$SOCK" -X PUT --data-binary @- \
          "http://localhost/certificates/$NAME"
```

Install it as `/usr/local/sbin/freeunit-deploy-hook`, mode 0755.

### certbot

```sh
ln -s /usr/local/sbin/freeunit-deploy-hook \
      /etc/letsencrypt/renewal-hooks/deploy/freeunit
certbot certonly --webroot -w /var/lib/unit-acme -d example.org -d www.example.org
RENEWED_LINEAGE=/etc/letsencrypt/live/example.org /usr/local/sbin/freeunit-deploy-hook
```

The first line makes certbot run the hook after every renewal. The last
line does the first upload; add the `*:443` listener after it. The
distribution's `certbot.timer` runs `certbot renew` twice a day.

### lego

```sh
cat > /usr/local/sbin/lego-freeunit <<'EOF'
#!/bin/sh
CERT_FULLCHAIN=$LEGO_CERT_PATH CERT_KEY=$LEGO_CERT_KEY_PATH \
UNIT_CERT_NAME=$LEGO_CERT_DOMAIN exec /usr/local/sbin/freeunit-deploy-hook
EOF
chmod 0755 /usr/local/sbin/lego-freeunit

lego --email ops@example.org --accept-tos --path /var/lib/lego \
     --domains example.org --http --http.webroot /var/lib/unit-acme \
     run --run-hook /usr/local/sbin/lego-freeunit
```

For a wildcard, use `--dns <provider>` instead of `--http`; FreeUnit then
needs no challenge route at all.

### A renewal timer for lego

lego has no timer of its own. A systemd timer that runs daily is enough:
lego renews only when the certificate is close to its expiry.

```ini
# /etc/systemd/system/lego-renew.service
[Unit]
Description=Renew certificates with lego and deploy them to FreeUnit

[Service]
Type=oneshot
ExecStart=/usr/bin/lego --email ops@example.org --accept-tos \
    --path /var/lib/lego --domains example.org \
    --http --http.webroot /var/lib/unit-acme \
    renew --no-random-sleep --renew-hook /usr/local/sbin/lego-freeunit
```

```ini
# /etc/systemd/system/lego-renew.timer
[Unit]
Description=Daily certificate renewal

[Timer]
OnCalendar=daily
RandomizedDelaySec=1h
Persistent=true

[Install]
WantedBy=timers.target
```

```sh
systemctl enable --now lego-renew.timer
```

With cron instead: `17 3 * * * root /usr/bin/lego ... renew --no-random-sleep --renew-hook /usr/local/sbin/lego-freeunit`.

## Notes

- Never edit the files in the state directory by hand. A bundle changed
  there is picked up by the next reconfiguration, but `GET /certificates`
  keeps the old description until a restart.
- The hook must run as a user that may write to the control socket. Do
  not give an application process access to the socket.
