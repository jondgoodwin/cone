# Test certificates

The `tls` package's tests and its `bench` example use these. They are test
fixtures, made once and committed; nothing here is a secret, and nothing is
installed anywhere: no test puts them in the Windows certificate store or
changes a system setting. The machine does not trust the test root, which is
what the tests of the OS's verifier rely on.

`make-certs.sh` made them, with Git for Windows' OpenSSL 3.5.7, on 3 Oct 2026:

```
cd packages/tls/tests/certs
sh make-certs.sh
```

Every key is ECDSA P-256. The good certificates are valid from 1 Jan 2026 to
31 Dec 2125.

| File | What it is |
| --- | --- |
| `ca.pem` | The test root, "Cone Test Root CA": the only trust anchor the tests give a client (`Verifier.roots`) |
| `intermediate.pem` | "Cone Test Intermediate CA", under the root |
| `server.pem`, `server.key` | The server: `localhost` and `127.0.0.1`, server authentication, under the intermediate; `server.pem` is its chain, leaf first, then the intermediate |
| `expired.pem`, `expired.key` | The same names under the intermediate, valid only on 1 Jan 2020 |
| `untrusted.pem`, `untrusted.key` | The same names under "Cone Test Other Root", which no client is given; that root's certificate is `other-ca.pem` |
| `server.pin` | The SHA-256 of `server.key`'s public key (its SubjectPublicKeyInfo), in hex: what `Verifier.pinnedSha256` takes |

The roots' and the intermediate's private keys were deleted once they had
signed, so nothing more can be issued under them; run the script again to
make a new set (every test reads the files, and `server.pin` with them).
