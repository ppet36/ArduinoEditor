# Contributing to Arduino Editor

Thanks for your interest in contributing to **Arduino Editor** ❤️  
Feedback, bug reports, feature ideas, and code contributions are welcome.

This document explains how to report issues, suggest improvements, and
contribute code in a way that keeps the project maintainable and predictable.

---

## 🐞 Reporting bugs

Bug reports are handled via **GitHub Issues**.

Before opening a new issue:
- Please check whether a similar issue already exists.
- If possible, verify the problem with the latest released version.

When reporting a bug, please include:
- Your operating system (macOS, Linux, Windows, Raspberry Pi)
- Arduino Editor version
- What you expected to happen
- What actually happened
- Steps to reproduce the issue (if known)
- Relevant logs, screenshots, or error messages

Clear and reproducible reports make issues much easier to diagnose and fix.

### Debug-friendly release builds

Release builds of Arduino Editor are intentionally produced with debug symbols
(e.g. `-g3`) and without heavy optimization.  
This is done on purpose to make crash reports and stack traces more useful.

If the application crashes:
- please include the stack trace if available,
- and mention the exact release version and platform.

On **macOS**, you can also check **Console.app**:
- Open *Console.app* → *Crash Reports*
- Locate the latest "Arduino Editor" crash entry
- Copy the report into the GitHub issue

---

## 💡 Feature requests & ideas

Feature requests are welcome via **GitHub Issues**.

Please note:
- Not every idea can be implemented.
- Some requests may already be planned or intentionally out of scope.
- Discussion before implementation is encouraged, especially for larger changes.

If you plan to implement a feature yourself, opening an issue first is strongly
recommended.

---

## 🔧 Contributing code

Code contributions are accepted via **Pull Requests**.

General guidelines:
- Base your work on the `main` branch.
- One pull request should focus on a single change or feature.
- Keep commits reasonably small and focused.
- For larger or architectural changes, please start with an issue first.

The project prioritizes:
- Correctness over cleverness
- Readability and maintainability
- Predictable, cross-platform behavior  
  (macOS, Linux, Windows, Raspberry Pi)

---

## 🏗 Building the project

Instructions for building Arduino Editor from source are available in:

👉 **[BUILDING.md](BUILDING.md)**

Please ensure that your changes build successfully on at least one supported
platform before submitting a pull request.

---

## 🧭 Project direction

Arduino Editor aims to be:
- A professional, Clang-based Arduino IDE
- Transparent and predictable in behavior
- Focused on correctness and developer control

Contributions should respect the overall direction of the project, even if they
do not cover every possible use case.

---

Thanks again for contributing.  
Every issue report, idea, and pull request helps improve the project.

