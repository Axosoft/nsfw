# Changelog

## 2.3.0

### Security
- Update `minimatch` 3.1.2 → 3.1.3 and 5.1.6 → 5.1.8 to fix ReDoS vulnerability
- Force `serialize-javascript` ≥ 7.0.3 via yarn resolution to fix RCE vulnerability (CVE-2020-7660)

### Breaking (development only)
- Node.js 20+ is now required for development/testing due to `serialize-javascript` v7. This does **not** affect consumers of the package at runtime.
- CI no longer tests against Node.js 18
