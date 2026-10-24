# Test-only TLS fixtures

These certificates exist **only** for loopback tests. They are not secrets and
must never be used outside the test suite.

| File | Purpose |
|---|---|
| `ca.pem` / `ca.key` | Test root CA (signs the leaf certificates) |
| `server.pem` / `server.key` | Server leaf for `localhost` / `127.0.0.1` (SAN + serverAuth) |
| `client.pem` / `client.key` | Client leaf for mutual TLS tests (clientAuth) |

- Validity: 10 years from generation.
- Regenerate with any OpenSSL 3.x/Python `cryptography` toolchain; keep the
  filenames stable because tests reference them.
- `README.md` files in this directory are exempt from the vendored-dependency
  hash checks (they are test data, not third-party sources).
