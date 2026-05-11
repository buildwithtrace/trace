# Contributing to Trace

Thank you for your interest in contributing to Trace! We welcome contributions from the community.

## Join Our Discord Community

**Discord is our primary community hub** for all discussions, questions, and announcements:

- **[Join us on Discord](https://discord.gg/UNUvQxneCr)** - This is where we discuss features, answer questions, and make announcements about development progress (milestones, deadlines, new releases, etc.)
- **Report bugs** - Share issues and get help from the community
- **Discuss features** - Propose new ideas and discuss implementation
- **Ask questions** - Get help with the codebase or development setup
- **Stay updated** - Get notified about releases and important updates

**For larger changes or new features, please discuss on Discord before starting substantial work.** This ensures alignment with development goals and prevents duplicate work.

## Important Notes

- **Trace is a fork of KiCad**: This project is based on the KiCad EDA software
- **License**: All contributions must be compatible with GNU GPLv3
- **Copyright**: By contributing, you agree to license your code under GPLv3
- **Upstream**: Consider contributing improvements to [KiCad upstream](https://gitlab.com/kicad/code/kicad) when applicable

## Getting Started

Trace is a fork of KiCad, and we maintain compatibility with KiCad file formats and many of its architectural decisions. Developers familiar with KiCad will find the codebase familiar.

**New developers are encouraged to:**
- Start small with contributions and gradually work up to larger changes
- Check the [issue tracker](https://github.com/buildwithtrace/trace/issues) for issues labeled "good first issue"
- Search through issues and leave a comment if you're interested in working on something
- Ask questions in Discord or on the issue itself - other developers are happy to help!

**For larger changes:**
- Discuss on Discord or GitHub Discussions before substantial work
- This allows input from the development team to ensure alignment with goals
- Prevents duplication of work by contributors

## Development Guidelines

### Code Style

Make sure to read the [Trace Code Style Guide](https://docs.buildwithtrace.com/contributing/code-style/), which is based on the KiCad style guide with some modifications. You can use the `clang-format` tool to check many, but not all, of these style requirements. When you create a pull request, one of the CI pipeline steps will be to run a formatting check on your contribution.

**Important notes about automatic format checks:**

1. Some of our formatting guidelines have exceptions, or only apply to certain situations. `clang-format` doesn't know about these nuances, so it will sometimes suggest that you make sweeping format changes to areas of a file near your code (even if you didn't change that code). **When there is flexibility or doubt, follow the existing formatting of the file you are editing, rather than rigidly following `clang-format`.**

2. `clang-format` doesn't know about our desire for nice column-formatting where applicable.

3. `clang-format` doesn't support our preferred lambda format.

**Key style points:**
1. Always create a new branch for PRs instead of using your fork's main branch
2. Follow the Trace Code Style Guide for C++ code
3. Make sure UI changes follow the [User Interface Guidelines](https://docs.buildwithtrace.com/contributing/ui/)
4. When in doubt, follow the existing formatting of the file you're editing

### Pull Request Process

Trace welcomes contributions via pull requests on GitHub. Here are some tips to help make sure your contribution can be accepted quickly:

1. **Fork the repository** and create a feature branch (never use your fork's master branch)
2. **Make your changes** with clear, atomic commits
3. **Test thoroughly** - build and run on your platform
4. **Write a clear PR description** - Give pull requests a short and descriptive title that summarizes the major changes. A longer description of the changes should be contained inside the description of the pull request.
5. **Reference issues** if your PR fixes or relates to any
6. **Check GitHub settings** - Make sure "Allow edits by maintainers" is checked at the bottom of your pull request

### Commit Messages

Use clear, descriptive commit messages:
```
fix: resolve crash when opening large schematics
feat: add AI component suggestion for resistor values
docs: update build instructions for macOS
```

### Testing

- **Build locally** before submitting
- **Test your changes** in the UI
- **Check for regressions** in related features
- **Include test files** if applicable (example schematics/PCBs)

## Types of Contributions

### Bug Fixes
Found a bug? Please:
1. Check if it exists in upstream KiCad too
2. If KiCad bug: consider reporting there as well
3. If Trace-specific: fix it and submit a PR

### Features
New features should:
- Fit Trace's vision of AI-enhanced PCB design
- Not break existing functionality
- Be documented in code comments
- Consider UI/UX implications

### Documentation
Help improve:
- Code comments and docstrings
- README and guides
- Build instructions
- API documentation

## AI Features

If contributing to Trace's AI integration (`trace/` directory):
- Test with the backend API (contact us for dev access)
- Follow patterns in existing AI tool code
- Consider token usage and rate limits
- Document any new AI capabilities

## License & Copyright

By submitting a contribution, you certify that:
- You wrote the code, or have the right to submit it
- You agree to license your contribution under GPLv3
- You understand Trace is open source (others can use/modify your code)
- Your contribution doesn't violate any patents or copyrights

Add your copyright to files you substantially modify:
```cpp
* Copyright (C) 2026 Your Name <your@email.com>
* Copyright The KiCad Developers, see AUTHORS.txt for contributors.
```

## Getting Help

- **Discord**: [Join our community](https://discord.gg/p4TtrQf9) - Primary place for questions, discussions, and help
- **GitHub Issues**: https://github.com/buildwithtrace/trace/issues - For bug reports and feature requests
- **Email**: hello@buildwithtrace.com - For private inquiries
- **KiCad Docs** (for core EDA features): https://docs.kicad.org/

## Code of Conduct

Be respectful, constructive, and professional. We want Trace's development community to be welcoming to all contributors.

---

**Thank you for contributing to Trace!**

Every contribution, whether code, documentation, or bug reports, helps make Trace better for everyone.
