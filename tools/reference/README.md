# The reference implementation (frozen)

This is the Python 3.10+ stdlib implementation the contract was cut from
(0.1.x). It is kept **as the frozen specification** for contract regression:
`python -m pytest` here must stay green against the native C++ implementation
in `../../modules/` — it is not the program users run, and it does not gain
features.

```
python -m pytest          # 50 contract tests + 1 auto-skipped qemu integration
bin/disk-run --help
```

Everything user-facing (CLI, exit codes, marker protocol, `--expect` JSON,
`--file` semantics, error/fix sentences) is identical to the native one; see
the repository's README for the contract.
