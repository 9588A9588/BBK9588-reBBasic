# Contributing

Bug reports and pull requests are welcome. Please keep changes focused and do
not attach firmware dumps, original device applications, commercial games, or
third-party `BIN`/`LIB`/`SAV` resources.

Before opening a pull request, run from the SDK root:

```powershell
.\re-bbasic\tests\run.ps1
.\re-bbasic\build.ps1
python -m bda_packer.validate .\re-bbasic\build\BB虚拟机-re.bda
```

For compatibility fixes, include a minimal redistributable fixture or document
the observed port/resource behavior without publishing proprietary data.
