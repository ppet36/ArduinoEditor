# Embedded Markdown Parser (maddy)

This directory contains a **vendored copy** of the *maddy* Markdown parser.

## Origin

The original project is **maddy**, a header-only C++ Markdown parser:

- Project: https://github.com/progsource/maddy
- Author: progsource
- License: MIT

The code included here was originally taken from the upstream repository and integrated directly into this project to avoid an external dependency.

## Modifications

The following modifications have been applied compared to the upstream version:

- Performance-related patches based on discussion and proposals from this issue:
  - https://github.com/progsource/maddy/issues/50
- Minor local adjustments required for integration into this project (namespaces, includes, warnings, etc.).

No functional changes to the Markdown syntax or parsing behavior were intentionally introduced beyond the above.

## License

The original *maddy* project is licensed under the **MIT License**.

All original copyright notices and license terms remain applicable.
See the original repository for full license details:

https://github.com/progsource/maddy/blob/master/LICENSE

## Notes

This copy is intentionally kept **header-only** and self-contained.
Upstream changes may be manually merged in the future if needed.

